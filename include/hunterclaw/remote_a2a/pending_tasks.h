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

    /// 标记终结。
    ///
    /// ★ 粘性规则（対标 AgentWaitResult::is_sticky_terminal，run_wait.h:105-108）：
    ///   已有粘性终态（HardTimeout / Canceled）时，后到的普通终态【不覆盖】。
    ///   场景：用户取消后远端才把 completed 发回来 —— 用户看到的必须是"已取消"。
    /// @return 是否发生了状态变更
    bool update_terminal(const std::string& handle, TerminalReason reason,
                        const std::string& text, const std::string& error_text);

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
