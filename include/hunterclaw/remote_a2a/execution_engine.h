#pragma once

// ============================================================================
// remote_a2a/execution_engine.h
// 三分支执行引擎 —— 这是整套设计里唯一复杂的地方。
//
//                         ┌─ 分支② SSE (message/stream)
//   send/stream ──────────┼─ 分支① 退避探测 (tasks/get)
//                         └─ 分支③ 回调 (pushNotificationConfig)
//
// ── 分支选择顺序（不是并列的三个选项，而是一条降级链）──────────────────
//   ① 若配了回调 且 push 能力未被否 → 先注册回调（它是「兜底」，不是主路径）
//   ② 若 streaming 未被否 → 走 SSE（延迟最低）
//   ③ SSE 失败/不支持 → send + 退避探测
//   ④ 让渡超时 → 交给后台线程继续，工具先返回 handle
//
// ★ 回调与 SSE/探测【不是互斥】的，而是叠加：
//   回调只当「信号」（"任务有更新了，快来拉"），权威数据【始终】来自
//   tasks/get。理由：回调 payload 由远端构造、经公网传输、可被伪造，
//   而 tasks/get 是我们主动发起的、走已鉴权通道。
//   若直接采信回调 payload，攻击者只要猜到句柄就能伪造"任务已完成 + 结果是X"。
//
// ── 让渡（yield）语义：沿用 exec ─────────────────────────────────────────
// exec.cc:930  yield_window = clamp(yieldMs, 10, 120000)
// exec.cc:975-986 等到窗口结束仍未完成 → 返回「still running + session id」
//                 且这是 【成功】结果（tool_result::text），不是 error
// ★ 本引擎照此：超时返回 success=true + handle，而不是 error。
//   若返回 error，模型会认为工具坏了并重试 —— 于是同一个远端任务被起两遍。
//
// ── 状态判定全部委托 task_tracking ───────────────────────────────────────
// 本文件【不自己判】终态，一律构造 TaskTracking 交给 classify()。
// 理由：classify() 的顺序即优先级（HardTimeout > Zombie > RemoteFailed >
// Canceled > ConnLost），在两处实现必然漂移。
//
// ── 并发原语约束（项目级）────────────────────────────────────────────────
// ✅ thread / mutex / condition_variable / atomic
// ❌ future / promise / async / packaged_task
// 后台跟进用 detached thread + PendingTaskStore 的 cv 交接。
// ============================================================================

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "hunterclaw/core/types.h"
#include "hunterclaw/remote_a2a/pending_tasks.h"
#include "hunterclaw/remote_a2a/probe_policy.h"
#include "hunterclaw/remote_a2a/remote_agent_client.h"
#include "hunterclaw/remote_a2a/task_tracking.h"

namespace hunterclaw {
namespace remote_a2a {

// ── 分支标识 ────────────────────────────────────────────────────────────────

enum class ExecBranch {
    None,
    Stream,    // ② SSE
    Probe,     // ① 退避探测 tasks/get
    Callback,  // ③ 回调（叠加在 ①/② 之上）
};

const char* exec_branch_text(ExecBranch);

// ── 配置 ────────────────────────────────────────────────────────────────────

struct ExecutionConfig {
    /// 让渡窗口。★ clamp 到 [10, 120000]，与 exec.cc:930 完全一致。
    int64_t yield_ms = 30000;
    /// 立即让渡（対标 exec 的 background=true）
    bool background = false;

    /// 单个任务的总时长上限（超过 → HardTimeout，粘性）
    int64_t max_task_ms = 30 * 60 * 1000;

    /// SSE 静默多久后转入探测
    int64_t idle_timeout_ms = 120000;

    /// ★★ 强制要求：该远端的所有 input-required 都必须由【用户】确认。
    ///
    /// 与远端 metadata 的 requiresUserAction 取「或」——
    /// 远端【能加严不能放松】，与 requires_approval 绝不从卡读同一原则。
    ///
    /// ⚠ 只听远端是不安全的：远端声明 requiresUserAction=false 就能让
    ///   模型代答本应由用户决定的事（如"确认转账"）。
    bool require_user_confirm_on_input = false;

    ProbePolicy probe;

    /// 是否启用回调分支（需要远端支持 pushNotificationConfig）
    bool enable_callback = false;
    /// 回调基址（HunterClaw 侧对外可达的 URL 前缀）
    std::string callback_base_url;

    /// 让渡后是否起后台线程继续跟进。
    /// ★ false 时任务会停在最后已知状态，靠回调或用户主动查询推进。
    bool background_follow_up = true;

    /// clamp 后的让渡窗口
    int64_t effective_yield_ms() const;
};

// ── 进度通道 ────────────────────────────────────────────────────────────────

/// 中间态进度回调。
///
/// ⚠ `chat.progress` 【不存在】—— 全仓 grep 只有 send/abort/history/inject。
///   实际用 system-event 通道（exec 同通道）。中间态必须带
///   reason="heartbeat" 之类的上下文键，避免每条进度都触发入队
///   （system_events.cc:86-88 会跳过连续重复文本，但不同文本仍会入队，
///    20 条上限 MAX_EVENTS 很快被进度刷满，把真正的事件挤掉）。
using ProgressFn = std::function<void(const std::string& text, bool is_heartbeat)>;

// ── 二次确认信号的解析（★ 隔离在一处，便于对接后调整）──────────────────────
//
// ⚠⚠ tokenIds 在 A2A 报文里的位置【尚未与企微确认】。
//   当前按 `status.message.parts[].metadata` 读取，因为:
//     · Part 已有 metadata 字段（types.h:78），是 A2A 规范内的扩展位
//     · 独立 artifact 语义不符（artifact 是"产出"而非"待办"）
//     · Task.metadata 在 A2A 规范里用途较模糊
//
// ★ 全部解析集中在这一个函数里 —— 位置定了只改这里，
//   调用方（ingest_task / render）不受影响。
//
// 识别的 metadata 键（大小写敏感，两种命名都收）:
//   requiresUserAction / requires_user_action : bool
//   tokenIds / token_ids                      : string[]
struct ConfirmSignal {
    bool requires_user_action = false;
    std::vector<std::string> token_ids;
};

/// 从一组 Part 中提取二次确认信号。
///
/// ★★ 这是【唯一】的解析实现。Task 版与流式版都委派到这里。
///
/// ⚠⚠ 曾经只有 Task 版，于是 SSE 路径（run_stream::on_event）完全没有
///   confirm 提取 —— 远端通过流式推 input-required + token_id_list 时，
///   requires_user_action 始终为 false，方案 C 的提问与方案 A 的审批
///   【两道闸门同时失效】，模型会代用户答"确认"。
///   同一判据散落/缺失的代价在这里是"决定权被悄悄拿走"，不是少个字段。
///
/// @param force_user_action 本地配置强制要求用户确认（取「或」）
ConfirmSignal extract_confirm_signal_from_parts(const std::vector<Part>& parts,
                                               bool force_user_action);

/// 从远端 Task 中提取二次确认信号。
/// @param force_user_action 本地配置强制要求用户确认（取「或」）
ConfirmSignal extract_confirm_signal(const Task& t, bool force_user_action);

/// 从流式事件中提取二次确认信号。
///
/// ★ 依赖 ParsedStreamEvent::status_message —— 那个字段就是为本函数补的。
///   在它存在之前，流式事件里的 data/metadata 在 parse_stream_event
///   就被丢弃了，任何下游都不可能再拿到。
ConfirmSignal extract_confirm_signal(const ParsedStreamEvent& ev, bool force_user_action);

// ── 执行结果 ────────────────────────────────────────────────────────────────

struct ExecutionResult {
    /// ★ 让渡超时也是 true（対标 exec.cc:983 返回 tool_result::text）。
    ///   返回 false 会让模型重试，导致同一远端任务被起两遍。
    bool success = false;

    /// 本轮是否真的拿到了终态结果
    bool completed = false;
    /// 是否已让渡（此时 handle 有效，结果稍后取）
    bool yielded = false;

    ExecBranch branch = ExecBranch::None;

    std::string handle;     // 让渡时的本地句柄
    std::string task_id;
    std::string context_id;

    /// 正式结果（来自 artifacts）。可能为空。
    std::string text;

    /// 远端的追问 / 中间态文本（来自 status.message）。
    ///
    /// ★ 必须与 text 【分开】存放，不能二选一。
    ///   input-required 时远端可以【同时】给出「已完成的部分结果」(artifacts)
    ///   与「还需要什么」(status.message)。若合并成一个字段并取其一，
    ///   有部分结果的场景下追问会被吞掉 —— 模型看到
    ///   「需要补充输入: <部分结果>」，完全不知道要补什么，流程直接死锁。
    std::string prompt_text;

    std::string error_text;

    TaskState state = TaskState::Submitted;
    std::optional<TerminalReason> reason;

    /// 本轮是否处于「挂起等外部输入」状态（input-required / auth-required）。
    /// ★ 这不是失败 —— 是一次正常的往返中断，需要模型带 task_id 再调一次。
    bool is_suspended() const {
        // ★ 委派给 types.h 的公共判定, 不要在这里重写条件 ——
        //   同一判据出现在多处时, 改一处漏一处就静默错
        //   (update_terminal 当初就是这么错的)。
        return is_suspended_state(state);
    }

    /// ★★ 必须由【用户】决定，模型不得代答。
    ///
    /// 判据取「或」，与 requires_approval 同一原则 —— 远端能加严不能放松：
    ///   · 本地配置 requireUserConfirmOnInput=true → 该远端所有 input-required
    ///     一律必须问用户
    ///   · 本地为 false 时才听远端 metadata 的标记
    ///
    /// ⚠ 为什么需要这个字段：is_suspended() 只说"挂起了"，区分不了
    ///   「模型可代答」（"用哪种格式导出？"）与「必须用户决定」
    ///   （"确认转账 5000 元？"）。后者若让模型代答，决定权被悄悄拿走，
    ///   而这类错误极难发现 —— 流程走通了、结果返回了，只是那个"确认"
    ///   是模型猜的。
    bool requires_user_action = false;

    /// 远端侧的待确认凭据（如企微二次确认的 tokenId 列表）。
    ///
    /// ★ 我们【只透传】不解析其含义 —— 它是远端与终端之间的约定。
    ///   带出来是为了进 details 供终端消费（拉起对应确认页面）。
    std::vector<std::string> pending_token_ids;

    /// 面向模型的一段文本（工具返回值）。
    ///
    /// ⚠ 这是模型能看到的【唯一】内容 —— ToolResult.details 不进 prompt
    ///   （react_engine.cc:1076-1088 只读 content）。因此续接所需的
    ///   task_id 必须出现在这段文本里，否则模型无法续接。
    std::string render() const;
};

// ── 引擎 ────────────────────────────────────────────────────────────────────

class ExecutionEngine {
public:
    static std::shared_ptr<ExecutionEngine> create(
        std::shared_ptr<RemoteAgentClient> client,
        std::shared_ptr<PendingTaskStore> store, const ExecutionConfig& cfg);

    ExecutionEngine(std::shared_ptr<RemoteAgentClient> client,
                    std::shared_ptr<PendingTaskStore> store,
                    const ExecutionConfig& cfg);
    ~ExecutionEngine();

    /// 执行一次远端调用（同步，直到完成或让渡）。
    ///
    /// @param session_key 归属会话（system-event 投递与权限校验用）
    /// @param on_progress 中间态通道；可为空
    ExecutionResult execute(const Message& msg, const std::string& session_key,
                           ProgressFn on_progress = nullptr);

    /// 单次调用覆盖让渡窗口（工具的 yield_ms / background 参数）。
    ///
    /// ★ 只允许覆盖【让渡窗口】，不允许覆盖 max_task_ms / probe / 回调配置 ——
    ///   那些是运维策略（资源上限、安全边界），不能由模型参数改。
    ///   模型能改的只有「我愿意等多久」。
    /// @param yield_override_ms 生效前会 clamp 到 [10,120000]
    ExecutionResult execute_with_yield(const Message& msg,
                                      const std::string& session_key,
                                      int64_t yield_override_ms, bool background,
                                      ProgressFn on_progress = nullptr);

    /// 继续跟进一个已让渡的任务（供 remote_a2a_status 工具调用）。
    /// @param wait_ms 0 = 只看当前状态
    ExecutionResult follow_up(const std::string& handle, int64_t wait_ms);

    /// 单次权威拉取 —— ★ 回调分支专用。
    ///
    /// 做一次 tasks/get 并把快照并入登记表（含终态判定）。
    ///
    /// ★ 为什么回调必须走这个而不是直接采信回调 payload：
    ///   回调 payload 由远端构造、经公网传输、可被伪造。攻击者只要猜到句柄
    ///   与 token 就能伪造「任务已完成，结果是 X」。tasks/get 是我们主动发起、
    ///   走已鉴权通道的，是唯一可信来源。回调只是「有更新了，快来拉」的信号。
    ///
    /// @return 是否已到终态（false 表示仍在跑，或拉取失败）
    bool poll_once(const std::string& handle);

    /// 取消（best-effort）。
    /// ⚠ 対标 ADK executor/utils.py:31-48：远端的 cancel 往往只塞一个
    ///   canceled 事件而【不中断】实际执行 —— 远端会继续烧 token。
    ///   因此本地【立即】标记 Canceled（粘性），不等远端确认。
    core::Result<bool> cancel(const std::string& handle);

    /// 登记表访问器。
    ///
    /// ★ 供工具层做去重闸（try_claim_question）用。
    ///   ⚠ 刻意返回 engine 自己持有的那一份，而不是让调用方另传一个 ——
    ///     去重闸必须作用在【同一个】store 上。发送工具本身不持有 store，
    ///     若让它另传，很容易传进一个不同实例，那样 CAS 形同虚设:
    ///     每条通道都能抢到锁，用户被问 5 遍。
    const std::shared_ptr<PendingTaskStore>& store() const { return store_; }

    /// 请求所有后台线程退出（析构时自动调用）
    void shutdown();

    // ── 挂起态主动通知（SSE / 回调 / 后台跟进 三条通道共用）────────────────
    //
    // ★★ 解决的问题: 「远端要求用户确认」这件事原先【只能在模型主动调工具
    //   的那一刻被发现】。三条异步通道各自在自己的线程里把状态写进 store，
    //   然后就静默了 ——
    //     · run_stream::on_event      （SSE 线程）
    //     · poll_once                  （回调 HTTP 线程）
    //     · background_loop → run_probe（detached 后台线程）
    //   模型若判断"已让渡，等下轮再看"或直接收尾，用户【永远不会被问到】，
    //   远端任务一直挂到 HardTimeout。
    //
    // ★ 用回调注入而非让 engine 直接依赖提问模块:
    //   remote_a2a 是 tools 的下游，不能反向依赖 question 编排层；
    //   而且"怎么问、怎么续接"是工具层的知识（它知道 session_key、
    //   agent 名、进度通道），engine 只负责"什么时候该问"。
    //
    // ⚠ 实现方必须自己保证幂等 —— 用 store_->try_claim_question(handle)。
    //   engine 不替它去重: 工具线程里的同步路径也会走同一个 claim，
    //   去重必须是【跨线程跨通道】的单一闸门。
    using SuspendResolver = std::function<void(const std::string& handle)>;

    /// 注入挂起解析器。
    ///
    /// ⚠ 只允许设置一次（与 QuestionRuntime 同理）。后设的会让先前
    ///   已在飞的挂起任务的通知落到旧回调上，那批任务再也不会被处理。
    bool set_suspend_resolver(SuspendResolver resolver);

private:
    // ── 分支实现 ────────────────────────────────────────────────────────

    /// SSE 流的处理结果。
    struct StreamRunResult {
        /// 是否已取得终态（含挂起态）
        bool terminal = false;
        /// 登记后的 handle（首事件拿到 task_id 时惰性登记）
        std::string handle;
        std::string task_id;
        std::string context_id;
        /// 流本身是否成功建立（false → 调用方应降级到 send + 探测）
        bool stream_ok = false;
        std::string error;
    };

    /// ② SSE：**一次** message/stream 走完全程。
    ///
    /// ⚠⚠ 历史 bug：曾经把它拆成"先开一条流探 task_id、关掉、再开第二条流
    ///   读全程"，导致【同一条 msg 被 message/stream 发送两次】：
    ///     · 首次调用（msg 无 task_id）→ 远端创建【两个 task】，我们只跟踪
    ///       第一个，第二个成为孤儿继续跑 —— 资源泄漏 + 副作用执行两次
    ///     · 续接（msg 带 task_id）→ 同一条"确认，请继续"被投递两次 ——
    ///       远端可能把邮件发两遍，二次确认的意义被彻底摧毁
    ///
    /// ★ 正确做法是单次流 + 惰性登记：第一个带 task_id 的事件到达时就地
    ///   register_task，后续事件正常处理。既消除重复发送，也省掉一次 RPC。
    ///
    /// ★ 不用 tasks/resubscribe：那需要先知道 task_id（鸡生蛋），而且多一次
    ///   往返。单次流天然没有这个问题。
    StreamRunResult run_stream(const Message& msg, const std::string& session_key,
                              ExecutionResult* out, ProgressFn on_progress,
                              int64_t deadline_ms);

    /// ① 退避探测。返回是否已取得终态。
    bool run_probe(const std::string& handle, ExecutionResult* out,
                   ProgressFn on_progress, int64_t deadline_ms);

    /// ③ 注册回调（失败不阻断，只降级）
    void try_register_callback(const std::string& handle, const std::string& task_id);

    /// 让渡后的后台跟进线程体。
    /// ⚠ on_progress 必须按值传: 让渡后调用方栈帧立刻销毁。
    ///   原先这里不带进度通道，后台期间用户看不到任何输出。
    void background_loop(std::string handle, ProgressFn on_progress);

    /// 挂起态收敛后的统一通知点。
    ///
    /// ★★ 只允许从【两处】调用: ingest_task 与 run_stream::on_event。
    ///   前者覆盖 探测/回调/后台 三条（它们都经 ingest_task），
    ///   后者覆盖 SSE（它不经 ingest_task —— 这正是当初 SSE 漏掉
    ///   confirm 提取的同一个原因）。
    ///
    /// ⚠ 必须在 update_terminal 【之后】调用: resolver 会去读快照，
    ///   状态没落定就读会拿到 Running。
    void maybe_notify_suspended(const std::string& handle);

    // ── 终态收敛的【唯一】入口 ──────────────────────────────────────────
    //
    // ★★ 这个函数存在的唯一理由: "落终态" 这件事需要严格按序做四步，
    //   而这四步曾经在【三个地方】各写一遍，结果漏了三次:
    //     ① ingest_task            —— 最早只有这一处有 confirm 提取
    //     ② run_stream::on_event   —— 漏了 confirm 提取（SSE 二次确认全废）
    //     ③ execute 的 SSE 首事件即终态 —— 同样漏了 confirm 提取 + 通知
    //
    //   ⚠⚠ 漏掉的后果全部是【静默放行】: requires_user_action 恒 false →
    //     interrupt 不产生 → 模型代用户答"确认" → 邮件真的发出去。
    //     流程走通、不报错、只是决定权被悄悄拿走 —— 最难发现的一类缺陷。
    //
    // ── 四步的顺序都有硬理由，不可调换 ────────────────────────────────
    //   ① 提 confirm 信号 → note_confirm_signal
    //        必须最先: update_terminal 会 notify 等待方，先落终态会让
    //        等待方读到 requires_user_action=false 的快照。
    //   ② 挂起时把追问写进 progress_text（note_progress）
    //        必须在 ③ 之前: note_progress 会拒绝已终结的记录
    //        （pending_tasks 的 phase != Running 检查）。
    //   ③ update_terminal（带远端【真实 state】）
    //        不传 state 会让 input-required 被 reason(Completed) 推导覆盖成
    //        Completed —— 追问变成"最终答案"。
    //   ④ maybe_notify_suspended
    //        必须在 ③ 之后: resolver 会读快照，状态没落定会拿到 Running。
    //
    // @param parts 用于提取 confirm 信号的 status.message.parts。
    //        ★ 传 nullptr 表示"本次没有 status.message"（例如纯 error 收敛）。
    // @param result_text 结果文本。⚠ 挂起时【调用方应传空】——
    //        挂起态的 status.message 是追问不是结果，写进 result_text 会让
    //        harvest 取到一句提问。本函数不替调用方判断，因为"哪段文本是结果"
    //        只有调用方知道。
    // @param prompt_text 挂起时的追问文本（非挂起时忽略）
    // @param epoch 调用方进入时取得的交互代号；0 = 不校验。
    //        ★★ 非 0 时，代号不符的写入会被 store 全部丢弃 ——
    //          这是"旧探测线程不能污染续接后的新一代记录"的强制保障。
    //          ⚠ 别在这里自己比对 current_epoch 再调用无代号版本: 那是
    //            TOCTOU，两次加锁之间续接可能发生。必须把 epoch 传下去，
    //            由 store 在写入的同一临界区内校验。
    void settle_terminal(const std::string& handle, TerminalReason reason,
                         TaskState remote_state, const std::string& result_text,
                         const std::string& error_text,
                         const std::vector<Part>* parts,
                         const std::string& prompt_text, uint64_t epoch = 0);

    /// 由当前记录构造 TaskTracking 并交给 classify()。
    /// ★ 终态判定的唯一入口 —— 不要在别处重复实现。
    std::optional<TerminalReason> evaluate(const PendingTask& t, bool transport_dead,
                                          const std::string& last_err) const;

    /// 把一次 Task 快照并入登记表（指纹、artifact 增量、终态）
    /// @param epoch 见 settle_terminal；0 = 不校验（用于无并发风险的调用点）
    void ingest_task(const std::string& handle, const Task& t, ProgressFn on_progress,
                     uint64_t epoch = 0);

    std::shared_ptr<RemoteAgentClient> client_;
    std::shared_ptr<PendingTaskStore> store_;
    ExecutionConfig cfg_;

    /// ★ 用 mutex 而非 atomic: SuspendResolver 是 std::function，
    ///   拷贝不是原子的。读取时先在锁内拷一份再出锁调用 ——
    ///   ⚠ 持锁调用 resolver 会死锁: resolver 里会 engine->execute() 续接。
    mutable std::mutex resolver_mu_;
    SuspendResolver suspend_resolver_;

    std::atomic<bool> stopping_{false};
    std::atomic<int> live_threads_{0};
};

/// 生成回调 token（32 字节十六进制）。★ 不进日志。
std::string generate_callback_token();

/// 构造 pushNotificationConfig。
/// ★ 一定带 authentication.credentials（我们生成的 token）——
///   不带 token 的回调端点等于开放写入口，任何人猜到 handle 就能伪造结果。
nlohmann::json build_push_config(const std::string& base_url,
                                const std::string& handle,
                                const std::string& token);

}  // namespace remote_a2a
}  // namespace hunterclaw
