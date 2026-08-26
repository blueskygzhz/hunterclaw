#pragma once

// ============================================================================
// remote_a2a/task_tracking.h
//
// 长任务状态机。★ 纯函数、零 IO、零线程 —— 全部可单测。
//
// ── 为什么必须有这一层 ──────────────────────────────────────────────────────
// Google ADK 在此几乎空白（已逐行核实）：
//   · 客户端【无】tasks/get（grep 'get_task|tasks/get|resubscribe' 无结果）
//   · 唯一兜底是 remote_a2a_agent.py:105 的 DEFAULT_TIMEOUT = 600.0 httpx 超时
//   · _compat.py:806-808 显式 setdefault("polling", False)
//   · executor/utils.py:31-48 的 cancel() 只塞一个 canceled 事件，
//     【不中断】runner.run_async() —— 远端继续烧 token
// 后果：无法区分「任务在算」与「远端死了」，无断线恢复，无判死机制。
//
// ── 核心思路 ────────────────────────────────────────────────────────────────
// working 不是一个状态，而是四种。判据是「推进证据」而非 state 本身
// ——因为卡死任务的 state 永远是 working。
//
// 方法论対标 agents/run_wait.h：
//   · ProgressFingerprint  ←→ AssistantReplySnapshot::fingerprint
//     （run_wait.h:185-189 注释：「同样的文本但属于新一条消息」也应判为已更新，
//       故不能只比单一字段）
//   · classify() 的顺序即优先级  ←→ run_wait_pure.cc:150-164 的 error 取值链
//   · is_sticky()  ←→ AgentWaitResult::is_sticky_terminal()（run_wait.h:105-108）
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>

#include "hunterclaw/remote_a2a/probe_policy.h"
#include "hunterclaw/remote_a2a/types.h"

namespace hunterclaw {
namespace remote_a2a {

// ── 推进指纹 ────────────────────────────────────────────────────────────────

/// 一次观测的推进指纹。任一字段变化即视为「有推进」。
///
/// ⚠ 只比 TaskState 是【不够】的 —— 卡死任务的 state 永远是 working，
///   而正常输出中的任务 state 也是 working。两者用 state 无法区分。
///
/// 五个字段的作用：
///   status_timestamp        —— 远端每次更新状态都会刷新（最灵敏）
///   history_count           —— 新增消息
///   artifact_count          —— 新增 artifact
///   total_artifact_parts    —— 已有 artifact 内新增分片（流式输出）
///   last_status_message_len —— 状态消息内容变化（同长度不同内容会漏判，
///                              但配合 timestamp 足够；不用 hash 是为了
///                              避免为此引入 sha256 依赖）
struct ProgressFingerprint {
    std::string status_timestamp;
    size_t history_count = 0;
    size_t artifact_count = 0;
    size_t total_artifact_parts = 0;
    size_t last_status_message_len = 0;
    /// SSE 路径下的单调事件计数（Task 快照路径不用）
    uint64_t event_seq = 0;

    static ProgressFingerprint from_task(const Task&);

    /// SSE 事件驱动的增量更新。任何携带信息的事件都推进 event_seq。
    void bump_from_event(const ParsedStreamEvent&);

    bool operator==(const ProgressFingerprint& o) const;
    bool operator!=(const ProgressFingerprint& o) const { return !(*this == o); }
};

// ── working 的四种细分 ──────────────────────────────────────────────────────

enum class WorkingSubState {
    /// 距上次事件 < idle_timeout → 继续等
    Fresh,
    /// 超 idle_timeout，但探测发现 fp 变了 → 重置计时继续等
    Stale,
    /// 超 idle_timeout 且 fp 未变 → 进入退避探测
    Suspect,
    /// 连续 max_no_progress 次 fp 未变 → 判死
    Zombie,
};

const char* working_sub_state_text(WorkingSubState);

// ── 终态原因 ────────────────────────────────────────────────────────────────

/// 归一化后的终态原因。
/// ⚠ 枚举顺序即优先级，与 classify() 的短路顺序严格一致。
///   対标 run_wait_pure.cc:150-164 的 error 取值链写法。
///   不要改成 if-else 平铺或调整顺序 —— 优先级错了会把硬超时降级成普通失败。
enum class TerminalReason {
    HardTimeout = 0,  // 总时长超限 —— 粘性
    Zombie,           // 连续探测无推进
    RemoteFailed,     // 远端明确 failed
    Canceled,         // 取消 —— 粘性
    ConnLost,         // 传输不可恢复
    Completed,        // 正常
};

const char* terminal_reason_text(TerminalReason);

/// TerminalReason → 对外暴露的 TaskState
TaskState terminal_reason_to_task_state(TerminalReason);

/// 対标 AgentWaitResult::is_sticky_terminal()（run_wait.h:105-108）：
/// hard_timeout / canceled 是粘性终态，不应被后到的普通状态覆盖。
bool is_sticky(TerminalReason);

// ── 追踪快照 ────────────────────────────────────────────────────────────────

/// classify() 的全部输入。刻意做成纯值类型，便于单测穷举。
struct TaskTracking {
    std::string task_id;
    std::string context_id;

    /// 远端最近一次报告的状态
    TaskState remote_state = TaskState::Submitted;

    ProgressFingerprint fp;
    int consecutive_no_progress = 0;
    int max_no_progress = 6;

    int64_t elapsed_ms = 0;
    int64_t max_task_ms = 1800000;

    int64_t idle_ms = 0;
    int64_t idle_timeout_ms = 120000;

    /// 传输层已判定断开（配合 last_err 决定是否可恢复）
    bool transport_dead = false;
    std::string last_err;
};

/// ⚠ 顺序即优先级，逐条短路。不要重排。
TerminalReason classify(const TaskTracking&);

/// working 细分判定。fp_changed 由调用方比较前后两次 fingerprint 得出。
WorkingSubState classify_working(const TaskTracking&, bool fp_changed);

/// 是否应当继续等待（即尚未到任何终态）
bool should_keep_waiting(const TaskTracking&);

// ── 传输错误可恢复性 ────────────────────────────────────────────────────────

/// 传输层抖动 vs 真死。
///
/// pattern 表参考 agents/run_wait_pure.cc:217-226，但有两处刻意差异：
///   · 去掉 gateway 专属项（"gateway closed (1006" / "gateway not connected"）
///     —— 那是 WebSocket gateway 的错误，HTTP 路径不会出现
///   · run_wait 把 "gateway timeout" 明确【排除】在可恢复之外，
///     但 HTTP 504 Gateway Time-out 是典型的上游抖动，应视为【可恢复】
bool is_recoverable_transport_error(const std::string& error);

}  // namespace remote_a2a
}  // namespace hunterclaw
