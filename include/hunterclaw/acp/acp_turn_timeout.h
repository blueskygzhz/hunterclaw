#pragma once

// 严格对齐 OpenClaw: src/acp/control-plane/manager.turn-timeout.ts (209行)
// Timeout and cleanup helpers for long-running ACP turns.
//
// C++ 与真源的结构性差异：
//   真源 runTurn 返回 AsyncIterable，可用 Promise.race 竞速；
//   HunterClaw 的 AcpRuntime::run_turn 是同步阻塞回调式接口，
//   因此超时必须靠「turn 跑在独立线程 + 主线程 future.wait_for」实现。
//   由此带来一个真源没有的约束：C++ 无法强杀线程，超时后 turn 线程仍在跑，
//   所有跨线程共享状态必须 shared_ptr 保活（见 AcpTurnEventGate）。

#include "hunterclaw/acp/runtime_errors.h"
#include "hunterclaw/acp/runtime_options.h"
#include "hunterclaw/acp/runtime_types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace hunterclaw {
namespace acp {

// 对齐 OC: ACP_TURN_TIMEOUT_CLEANUP_GRACE_MS = 2_000
constexpr int64_t ACP_TURN_TIMEOUT_CLEANUP_GRACE_MS = 2000;
// 对齐 OC: ACP_TURN_TIMEOUT_REASON = "turn-timeout"
inline constexpr const char* ACP_TURN_TIMEOUT_REASON = "turn-timeout";
// 对齐 OC: ACP_TURN_TIMEOUT_GRACE_MS (manager.turn-runner.ts 中 timeoutMs 的附加宽限)
constexpr int64_t ACP_TURN_TIMEOUT_GRACE_MS = 5000;
// 对齐 OC: resolveTurnTimeoutMs 的下限 minMs = 1_000
constexpr int64_t ACP_TURN_TIMEOUT_MIN_MS = 1000;
// 对齐 OC: DEFAULT_AGENT_TIMEOUT_SECONDS（agents/timeout.ts）
constexpr int64_t ACP_DEFAULT_AGENT_TIMEOUT_SECONDS = 300;
// 对齐 OC: MAX_SAFE_TIMEOUT_MS —— setTimeout 32 位上限
constexpr int64_t ACP_MAX_SAFE_TIMEOUT_MS = 2147483647;

/// Mutable gate used to suppress late events after timeout/cancel races.
/// 对齐 OC: type AcpTurnEventGate = { open: boolean }
/// C++ 差异：必须 shared_ptr + atomic —— 超时后 turn 线程仍活着并继续投事件，
/// 裸 bool 会造成 data race，栈对象会 UAF。
struct AcpTurnEventGate {
    std::atomic<bool> open{true};
};
using AcpTurnEventGatePtr = std::shared_ptr<AcpTurnEventGate>;

/// Summary of whether a turn stream emitted user-visible output or terminal events.
/// 对齐 OC: type AcpTurnStreamOutcome
struct AcpTurnStreamOutcome {
    bool saw_output = false;
    /// nullopt 表示流结束时没收到 done 事件 —— 调用方须据此报
    /// "ACP turn ended without a terminal done event."
    std::optional<std::string> terminal_status; // "completed" | "cancelled"
};

/// 对齐 OC: isCancellationStopReason
bool is_cancellation_stop_reason(const std::optional<std::string>& stop_reason);

/// Resolves legacy and current done events to the manager's canonical terminal status.
/// 对齐 OC: resolveAcpTurnTerminalStatus
std::string resolve_acp_turn_terminal_status(const AcpRuntimeEvent& done_event);

/// Resolves the effective ACP turn timeout from session runtime options or agent defaults.
/// 对齐 OC: resolveTurnTimeoutMs
int64_t resolve_turn_timeout_ms(const AcpSessionRuntimeOptions& options);

/// Result of a bounded turn wait. 对齐 OC: awaitTurnWithTimeout 的三种归宿
struct AwaitTurnResult {
    enum class Kind { Completed, Failed, TimedOut };
    Kind kind = Kind::Completed;
    AcpTurnStreamOutcome outcome;
    std::optional<AcpRuntimeError> error;
};

/// Awaits a turn with bounded timeout handling and late-error logging.
/// 对齐 OC: awaitTurnWithTimeout
///
/// timeout_ms <= 0 时退化为无限等待（对齐真源）。
/// 超时后 **不会** 返回错误对象，而是返回 TimedOut，由调用方执行 on_timeout
/// 后再构造带 detailCode 的 AcpRuntimeError（保持与真源相同的错误文案来源）。
AwaitTurnResult await_turn_with_timeout(
    const std::string& session_key,
    int64_t timeout_ms,
    int64_t timeout_label_ms,
    const std::function<AcpTurnStreamOutcome()>& run_turn_body);

/// Builds the timeout error carrying ACP_TURN_TIMEOUT_DETAIL_CODE.
/// 对齐 OC: awaitTurnWithTimeout 抛出的 AcpRuntimeError
AcpRuntimeError make_turn_timeout_error(int64_t timeout_label_ms);

/// Cancels a timed-out turn and clears non-persistent cached runtime state.
/// 对齐 OC: cleanupTimedOutTurn
///
/// cancel 与 close（仅 oneshot）各自享有 2s 宽限；两者都在宽限内完成才
/// 同步清缓存，否则交给后台线程在真正完成后再清（对齐真源 allSettled 分支）。
void cleanup_timed_out_turn(
    const std::string& session_key,
    const std::shared_ptr<AcpRuntime>& runtime,
    const AcpRuntimeHandle& handle,
    AcpRuntimeSessionMode mode,
    const std::function<void()>& clear_cached_runtime_state);

} // namespace acp
} // namespace hunterclaw
