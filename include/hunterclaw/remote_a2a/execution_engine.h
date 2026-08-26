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

    std::string text;       // 结果或让渡说明
    std::string error_text;

    TaskState state = TaskState::Submitted;
    std::optional<TerminalReason> reason;

    /// 面向模型的一段文本（工具返回值）
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

    /// 取消（best-effort）。
    /// ⚠ 対标 ADK executor/utils.py:31-48：远端的 cancel 往往只塞一个
    ///   canceled 事件而【不中断】实际执行 —— 远端会继续烧 token。
    ///   因此本地【立即】标记 Canceled（粘性），不等远端确认。
    core::Result<bool> cancel(const std::string& handle);

    /// 请求所有后台线程退出（析构时自动调用）
    void shutdown();

private:
    // ── 分支实现 ────────────────────────────────────────────────────────
    /// ② SSE。返回是否已取得终态。
    bool run_stream(const Message& msg, const std::string& handle,
                    ExecutionResult* out, ProgressFn on_progress,
                    int64_t deadline_ms);

    /// ① 退避探测。返回是否已取得终态。
    bool run_probe(const std::string& handle, ExecutionResult* out,
                   ProgressFn on_progress, int64_t deadline_ms);

    /// ③ 注册回调（失败不阻断，只降级）
    void try_register_callback(const std::string& handle, const std::string& task_id);

    /// 让渡后的后台跟进线程体
    void background_loop(std::string handle);

    /// 由当前记录构造 TaskTracking 并交给 classify()。
    /// ★ 终态判定的唯一入口 —— 不要在别处重复实现。
    std::optional<TerminalReason> evaluate(const PendingTask& t, bool transport_dead,
                                          const std::string& last_err) const;

    /// 把一次 Task 快照并入登记表（指纹、artifact 增量、终态）
    void ingest_task(const std::string& handle, const Task& t, ProgressFn on_progress);

    std::shared_ptr<RemoteAgentClient> client_;
    std::shared_ptr<PendingTaskStore> store_;
    ExecutionConfig cfg_;

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
