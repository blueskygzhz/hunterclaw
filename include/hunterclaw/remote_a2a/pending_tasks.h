#pragma once

// ============================================================================
// remote_a2a/pending_tasks.h
// 已让渡（yield）的远端任务登记表 —— ★ 一期纯内存。
//
// ── 为什么需要它 ────────────────────────────────────────────────────────────
// 工具调用有让渡语义（対标 exec 的 yieldMs/background）：等 N 秒还没完就
// 先返回 taskId，让模型继续做别的事。那么「后来的结果」必须有地方放，
// 且要能被三条互不相干的路径写入：
//   ① 后台探测线程（tasks/get 退避）
//   ② SSE 后台线程
//   ③ 回调（远端 POST 回来）
// 三者都可能同时到达同一个任务 —— 这是本文件所有加锁与"粘性"逻辑的来源。
//
// ── 一期内存存储的三个后果（明确接受）────────────────────────────────────
//   ① 进程重启 → 所有在途任务丢失（远端仍在跑，我们不再收结果）
//   ② 多进程部署 → 回调可能打到没有该任务的进程（返回 404，远端重试）
//   ③ 有界容量 → 超限时淘汰最老的【已终结】任务，绝不淘汰在途任务
// ★ ③ 的取舍很关键：宁可拒绝新任务，也不能把在途任务踢掉 ——
//   被踢掉的任务会永远等不到结果，而调用方还在等它。
//
// ── 与 transcript 的分工 ───────────────────────────────────────────────────
// `a2a:task_id` / `a2a:context_id` 写进 session metadata 随 transcript 天然
// 持久化；本表只存「运行时状态」（指纹、探测次数、累积文本）。
// 因此进程重启后，虽然本表空了，但从 transcript 仍能恢复出 task_id 去补拉。
//
// ── 并发原语约束（项目级）────────────────────────────────────────────────
// ✅ mutex / condition_variable / atomic     ❌ future / promise / async
// ============================================================================

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hunterclaw/core/types.h"
#include "hunterclaw/remote_a2a/task_tracking.h"
#include "hunterclaw/remote_a2a/types.h"

namespace hunterclaw {
namespace remote_a2a {

// ── 记录 ────────────────────────────────────────────────────────────────────

enum class PendingPhase {
    Running,     // 在途（★ 绝不被容量淘汰）
    Terminal,    // 已终结，等被取走
    Harvested,   // 已被取走（可淘汰）
};

const char* pending_phase_text(PendingPhase);

struct PendingTask {
    /// 本地句柄（返回给模型的那个 id）。★ 与远端 task_id 分开：
    /// 远端 task_id 可能重复或被复用，句柄由我们生成，保证唯一。
    std::string handle;

    std::string agent_name;
    std::string task_id;
    std::string context_id;
    /// 归属会话（用于 system-event 投递与权限校验）
    std::string session_key;

    PendingPhase phase = PendingPhase::Running;

    /// 远端最近报告的状态
    TaskState state = TaskState::Submitted;

    /// ★ 只在终结时有值。粘性规则见 update_terminal()。
    std::optional<TerminalReason> reason;

    /// 累积的正式结果文本（只累 artifact 增量，不累中间态）
    std::string result_text;
    /// 最近一条中间态文本（进度展示用，★ 不进 result_text）
    std::string progress_text;
    std::string error_text;

    /// ★★ 必须由【用户】决定，模型不得代答（见 ExecutionResult 同名字段）。
    ///
    /// ⚠ 必须持久在这里而非只存于 ExecutionResult：
    ///   input-required 最常见的入口是让渡后模型调 status 工具（follow_up），
    ///   那条路径【只读 store】—— 若信号不落库，follow_up 拿到的结果
    ///   会丢掉"必须用户确认"这件事，措辞退回"请补充输入"，
    ///   模型于是自行代答。
    bool requires_user_action = false;

    /// 远端侧的待确认凭据（透传给终端，我们不解析其含义）
    std::vector<std::string> pending_token_ids;

    /// 是否已经为本任务向用户发起过提问。
    ///
    /// ★★ 这是【去重闸】。同一个挂起态可能被多条通道同时发现:
    ///     · SSE  run_stream::on_event
    ///     · 探测 run_probe → ingest_task
    ///     · 回调 poll_once → ingest_task
    ///     · 后台 background_loop → run_probe → ingest_task
    ///     · 工具线程 try_resolve_input_required_via_question
    ///   没有这个闸，用户会为同一件事被问 2~5 次；而 PendingQuestionRegistry
    ///   每会话只允许一个 pending question，后来者全部注册失败并降级为
    ///   "仅终端"，于是纯文本认领这条路对谁都不生效。
    ///
    /// ⚠ 只能通过 try_claim_question() 置位（CAS 语义），不要直接写。
    bool question_raised = false;

    /// 交互代号（每次续接 +1）。
    ///
    /// ★★ 解决「僵尸线程复活」: background_loop / run_probe 的退出判定
    ///   发生在 interruptible_sleep 之后（退避最长 60s）。挂起时 phase 变
    ///   Terminal，但线程可能正睡着；等它醒来时用户已经答复、续接已把
    ///   phase 拉回 Running —— 于是老线程判定"任务还在跑"而继续探测，
    ///   与新一轮抢同一条记录。
    ///
    ///   ⚠ 双线程同时探测的后果不是"慢一点"，而是:
    ///     · consecutive_no_progress 被双倍推进 → 提前触顶 →
    ///       【误判 Zombie 把正常任务杀掉】
    ///     · fp 比较基准互相覆盖 → 推进判定错乱
    ///     · live_threads_ 虚高 → shutdown 白等 grace
    ///
    /// ★ 线程启动时记下自己那一代，每轮循环校验 epoch 是否仍相符；
    ///   不符即"我这一代已经结束了"，立刻退出。
    uint64_t epoch = 0;

    /// 本轮交互的起始时刻（每次续接刷新）。
    ///
    /// ⚠ 与 created_at_ms 分开: HardTimeout 必须按【本轮】计时。
    ///   沿用 created_at_ms 会让多轮确认的任务在第二轮就被判超时 ——
    ///   用户花 3 分钟考虑，这 3 分钟被算进远端的执行时限里。
    int64_t turn_started_at_ms = 0;

    ProgressFingerprint fp;
    int consecutive_no_progress = 0;

    int64_t created_at_ms = 0;
    int64_t updated_at_ms = 0;
    /// 最近一次收到【任何】数据的时刻（判 idle 用）
    int64_t last_event_ms = 0;

    /// 回调绑定 token（回调分支用；★ 不进日志）
    std::string callback_token;

    bool is_terminal() const { return phase != PendingPhase::Running; }
};

// ── 存储 ────────────────────────────────────────────────────────────────────

struct PendingStoreConfig {
    /// 容量上限。★ 只淘汰已终结的记录；全是在途时 register 会失败。
    size_t max_entries = 256;
    /// 已终结记录的保留时长（超时后可被清理）
    int64_t terminal_ttl_ms = 30 * 60 * 1000;
    /// 在途记录的绝对上限（超过即判 HardTimeout，防泄漏）
    int64_t running_max_ms = 6 * 60 * 60 * 1000;
};

class PendingTaskStore {
public:
    static std::shared_ptr<PendingTaskStore> create(const PendingStoreConfig& = {});

    explicit PendingTaskStore(const PendingStoreConfig& cfg);

    /// 登记一个新任务，返回句柄。
    /// ★ 容量满且无可淘汰项时返回 Err —— 宁可拒绝新任务，
    ///   也不能淘汰在途任务（被淘汰者会永远等不到结果）。
    core::Result<std::string> register_task(const std::string& agent_name,
                                           const std::string& task_id,
                                           const std::string& context_id,
                                           const std::string& session_key);

    /// 按句柄读取快照
    std::optional<PendingTask> get(const std::string& handle) const;

    /// 取当前交互代号。
    ///
    /// ★ 供 background_loop / run_probe 在【每轮循环】校验自己是否已过期。
    ///   ⚠ 不要用 get()->epoch 代替: get() 拷贝整条记录（含两个 vector
    ///     与多个 string），而这个校验在退避循环里每轮都做。
    ///
    /// @return 0 表示记录不存在（调用方应视为"我该退出了"）
    uint64_t current_epoch(const std::string& handle) const;

    // ── 代号守卫（epoch-guarded 写入）★★ ───────────────────────────────────
    //
    // ★★ 为什么必须有这一组: 探测线程与 resume 新线程【会真实并发】。
    //   resume 不等旧线程退出（旧线程可能正卡在 get_task 的网络 IO 里，
    //   最长还会活到退避上限 60s）。安全性靠"旧线程的写入无效"来保证，
    //   而不是靠"旧线程已退出"。
    //
    // ⚠ 只在调用方自己 current_epoch() 比对是【不够】的 —— 那是 TOCTOU:
    //     if (current_epoch(h) == mine)   ← 锁放开
    //         note_progress(h, ...)       ← 这之间续接可能已发生
    //   校验必须与写入在【同一临界区】内完成，所以只能由 store 提供。
    //
    // ⚠ 更关键的是可维护性: run_probe / poll_once / run_stream 共有 5+ 个
    //   写入点，靠每个调用点自觉补校验，将来新增探测通道必然漏。
    //   这组接口让"漏校验"变成写不出来 —— 需要代号就用 *_if_current。
    //
    // @return false = 代号不符（本次写入已被丢弃，调用方应立即退出）
    //         ★ 记录不存在也返回 false（视同过期）

    /// 带代号校验的进度记录。
    ///
    /// ★★ 返回值语义与 note_progress 【不同】: 这里返回的是"代号是否有效"，
    ///   而"是否有推进"由 out_advanced 带出。
    ///   ⚠ 两者绝不能合并成一个 bool: 调用方必须区分
    ///     「没推进」(要计 no_progress、继续探测) 与
    ///     「代号过期」(必须立刻退出，不能再碰这条记录)。
    ///     合并后上一代线程会把"过期"当成"没推进"，继续跑完整轮循环。
    ///
    /// @param epoch 调用方进入时取得的代号；0 = 不校验（等同旧行为）
    /// @param out_advanced 非空时写入"是否确实推进"
    /// @return false = 代号不符或记录不存在（调用方应立即退出）
    bool note_progress_if_current(const std::string& handle, uint64_t epoch,
                                  const ProgressFingerprint& fp, TaskState state,
                                  const std::string& progress_text,
                                  bool* out_advanced = nullptr);

    /// 带代号校验的 artifact 追加。
    /// @return false = 代号不符或记录不存在；true = 代号有效（含"记录已终结
    ///         故未写入"的情况 —— 与 note_progress_if_current 语义一致）
    bool append_result_if_current(const std::string& handle, uint64_t epoch,
                                  const std::string& delta);

    /// 带代号校验的无推进计数。
    /// ⚠ 这个尤其重要: 两代线程各自 note_no_progress 会【双倍推进】
    ///   consecutive_no_progress，把正常运行的任务误判成 Zombie 杀掉。
    /// @return false = 代号不符（未计数）
    bool note_no_progress_if_current(const std::string& handle, uint64_t epoch);

    /// 带代号校验的终结。语义同 update_terminal。
    /// ⚠ 最危险的一个: 上一代线程用过期状态把新一代记录写成终态，
    ///   会让正在进行的续接直接"完成"，用户拿到上一轮的结果。
    bool update_terminal_if_current(const std::string& handle, uint64_t epoch,
                                    TerminalReason reason, const std::string& text,
                                    const std::string& error_text,
                                    std::optional<TaskState> remote_state = std::nullopt);

    /// 按远端 task_id 反查句柄（★ 回调只带 task_id，没有我们的句柄）
    std::optional<std::string> handle_of_task(const std::string& task_id) const;

    /// 记录一次进度推进（SSE 或探测均可）。
    /// @param fp_changed 由调用方比较前后指纹得出
    /// @return 是否确实推进了
    bool note_progress(const std::string& handle, const ProgressFingerprint& fp,
                       TaskState state, const std::string& progress_text);

    /// 追加 artifact 增量文本。
    /// ⚠ 必须只传【本次增量】。ADK 注释（remote_a2a_agent.py:1327-1331）指出
    ///   若传归一化后的累积值，之前的分片会被重复发出，内容成倍重复。
    void append_result(const std::string& handle, const std::string& delta);

    /// 记录一次探测无推进（用于 Zombie 判定计数）
    int note_no_progress(const std::string& handle);

    /// 记录二次确认信号（挂起态专用）。
    ///
    /// ★ 单调置位：一旦标记为"需用户确认"就【不再取消】。
    ///   ⚠ 远端后续快照若漏带 metadata（很常见 —— 只有触发那一刻带），
    ///     取消标记会让措辞退回"请补充输入"，模型于是自行代答。
    ///   token_ids 为空时不覆盖已有值，同理。
    bool note_confirm_signal(const std::string& handle, bool requires_user_action,
                            const std::vector<std::string>& token_ids);

    /// 抢占「为本任务发起提问」的权利（CAS 语义）。
    ///
    /// @return true = 抢到了，调用方负责发起提问；false = 别人已经在做
    ///
    /// ★★ 必须在【持锁】的同一临界区内完成"读旧值 + 置新值"。
    ///   ⚠ 拆成 get() 判断 + 单独 set 会留下竞态窗口: SSE 线程与探测线程
    ///     可能同时看到 false 然后都去提问 —— 而 PendingQuestionRegistry
    ///     每会话只收一个，第二个静默降级成"仅终端"，纯文本认领对谁都不灵。
    ///
    /// ⚠ 抢到之后【不回滚】。提问失败也不重置 —— 重置会让下一次探测
    ///   再问一遍，在无人应答的部署里变成每轮探测都推一次卡片。
    ///   失败信息应通过工具结果文本告知模型，而不是靠重试提问。
    bool try_claim_question(const std::string& handle);

    /// 标记终结。
    ///
    /// ★ 粘性规则（対标 AgentWaitResult::is_sticky_terminal，run_wait.h:105-108）：
    ///   已有粘性终态（HardTimeout / Canceled）时，后到的普通终态【不覆盖】。
    ///   场景：用户取消后远端才把 completed 发回来 —— 用户看到的必须是"已取消"。
    ///
    /// @param remote_state
    ///   远端报告的真实 TaskState。给出时【原样保留】，不给则由 reason 推导。
    ///
    /// ★★ 这个参数是必需的，不是便利参数。
    ///   `reason` 是【我们的归因】，`state` 是【远端的事实】，两者不同源。
    ///   input-required / auth-required 的归因是 Completed（它们不是失败），
    ///   但若让 state 跟着 reason 推导，就会被写成 TaskState::Completed ——
    ///   远端"我还需要订单号"的事实被抹成"任务成功了"，
    ///   于是 render() 把追问当最终答案输出，续接流程彻底断掉。
    ///
    ///   反过来，HardTimeout / Zombie 是我们【自己】判的（远端根本没回话），
    ///   此时不传 remote_state，由 reason 推导出 Failed 才是对的。
    ///
    /// ⚠ 只接受终态。传入非终态会被忽略并回落到 reason 推导 ——
    ///   否则一条 working 会把记录写成"已终结但状态是进行中"的矛盾态。
    /// @return 是否发生了状态变更
    bool update_terminal(const std::string& handle, TerminalReason reason,
                        const std::string& text, const std::string& error_text,
                        std::optional<TaskState> remote_state = std::nullopt);

    /// 取走结果并标记 Harvested。★ 未终结时返回 nullopt（不阻塞）。
    std::optional<PendingTask> harvest(const std::string& handle);

    /// 阻塞等待终结。
    /// ⚠ 用 condition_variable，不用 future（项目级约束）。
    /// @param timeout_ms 0 表示不等待，直接看当前状态
    /// @return 终结则返回快照；超时返回 nullopt（★ 记录仍在表里，不删）
    std::optional<PendingTask> wait_terminal(const std::string& handle,
                                            int64_t timeout_ms);

    /// 唤醒等待者（回调线程写入后调用；update_terminal 已内含此操作）
    void notify_all();

    /// 绑定回调 token
    void set_callback_token(const std::string& handle, const std::string& token);
    /// 校验 token。★ 恒定时间比较，避免时序侧信道。
    bool verify_callback_token(const std::string& handle,
                              const std::string& token) const;

    /// 清理过期记录。
    /// ★ 两件事：① 删除超 TTL 的已终结记录
    ///          ② 把超 running_max_ms 的在途记录判为 HardTimeout（防泄漏）
    /// @return 被清理/判死的条数
    size_t sweep();

    /// 某会话的在途任务（供 /tasks 类命令展示）
    std::vector<PendingTask> list_by_session(const std::string& session_key) const;

    size_t size() const;
    size_t running_count() const;

private:
    /// 淘汰一个可淘汰项。★ 只考虑 Harvested / 超 TTL 的 Terminal。
    /// 调用方必须已持锁。
    bool evict_one_locked();

    PendingTask* find_locked(const std::string& handle);
    const PendingTask* find_locked(const std::string& handle) const;

    // ── 带代号校验的写入实现（调用方必须已持锁）────────────────────────────
    //
    // ★ 有锁版与无锁版共用同一份实现，避免两套逻辑漂移。
    //   epoch == 0 表示不校验（无代号的旧接口走这条）。
    bool note_progress_locked(const std::string& handle, uint64_t epoch,
                              const ProgressFingerprint& fp, TaskState state,
                              const std::string& progress_text,
                              bool* out_advanced);
    bool append_result_locked(const std::string& handle, uint64_t epoch,
                              const std::string& delta);

    PendingStoreConfig cfg_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, PendingTask> tasks_;
    /// task_id → handle 反查索引
    std::map<std::string, std::string> by_task_id_;
    uint64_t seq_ = 0;
};

/// 生成本地句柄，形如 "ra2a-7-1a2b3c"。
std::string make_pending_handle(uint64_t seq);

/// 恒定时间字符串比较（token 校验用）。
/// ⚠ 不要换成 == ：长度前缀比较会泄漏 token 前缀。
bool constant_time_equals(const std::string& a, const std::string& b);

}  // namespace remote_a2a
}  // namespace hunterclaw
