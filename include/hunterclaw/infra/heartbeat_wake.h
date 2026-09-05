#pragma once

// ============================================================================
// heartbeat_wake.h
// Aligned with openclaw: src/infra/heartbeat-wake.ts
//                      + src/infra/heartbeat-wake-target.ts
//                      + src/infra/heartbeat-wake-contracts.ts
//
// Provides:
//   - HeartbeatRunResult         (aligned with HeartbeatRunResult)
//   - HeartbeatWakeIntent        (aligned with HeartbeatWakeIntent)
//   - HeartbeatWakeSource        (aligned with HeartbeatWakeSource)
//   - are_heartbeats_enabled     (aligned with areHeartbeatsEnabled)
//   - set_heartbeats_enabled     (aligned with setHeartbeatsEnabled)
//   - request_heartbeat          (aligned with requestHeartbeat)
//   - request_heartbeat_now      (legacy shim → request_heartbeat)
//   - HeartbeatWakeHandler       (aligned with HeartbeatWakeHandler)
//   - set_heartbeat_wake_handler (aligned with setHeartbeatWakeHandler)
//
// 调度语义（严格对齐真源 schedule/takePendingWakeBatch）：
//   1. 合并窗口 DEFAULT_COALESCE_MS=250，多次请求塌陷为一次 handler 执行
//   2. 单 timer + 抢占：已有 timer 更早或同时到期则复用，更晚则抢占重排
//   3. 按 wake target 分组（session 优先于 agent），全局 target = "::"
//   4. 每个 target 内三槽（task / scheduled / event）按 intent 归位
//   5. 同槽合并按 priority，再按 requested_at
//   6. 每个 target 串行；跨 target 并发上限 MAX_CONCURRENT_WAKE_TARGETS=4
//   7. 全局 target 与定向 target 互斥（不与任何定向 turn 并行）
// ============================================================================

#include <string>
#include <functional>
#include <optional>
#include <cstdint>

namespace hunterclaw {
namespace infra {

// ============================================================================
// HeartbeatRunResult
// Aligned with openclaw: HeartbeatRunResult
// ============================================================================

struct HeartbeatRunResult {
    enum class Status { Ran, Skipped, Failed };
    Status status = Status::Skipped;
    std::optional<int64_t> duration_ms;  // set when status == Ran
    std::optional<std::string> reason;   // set when status == Skipped or Failed
};

// ============================================================================
// HeartbeatWakeIntent / HeartbeatWakeSource
// Aligned with openclaw: heartbeat-wake-contracts.ts
//   type HeartbeatWakeIntent = "scheduled" | "task" | "event" | "immediate" | "manual"
// ============================================================================

enum class HeartbeatWakeIntent {
    Scheduled,
    Task,
    Event,
    Immediate,
    Manual,
};

// Aligned with openclaw: HeartbeatWakeSource（仅保留影响优先级判定的取值）。
enum class HeartbeatWakeSource {
    Interval,
    Manual,
    Retry,
    Cron,
    Hook,
    BackgroundTask,
    AcpSpawn,
    SessionState,
    Other,
};

// ============================================================================
// HeartbeatWakeHandler
// Aligned with openclaw: HeartbeatWakeRequest / HeartbeatWakeHandler
// ============================================================================

struct HeartbeatWakeParams {
    std::optional<std::string> reason;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    // Aligned with openclaw: HeartbeatWakeRequest.intent / .source
    HeartbeatWakeIntent intent = HeartbeatWakeIntent::Event;
    HeartbeatWakeSource source = HeartbeatWakeSource::Other;
};

using HeartbeatWakeHandler = std::function<HeartbeatRunResult(const HeartbeatWakeParams&)>;

// ============================================================================
// Global heartbeat enable/disable switch
// Aligned with openclaw: areHeartbeatsEnabled / setHeartbeatsEnabled
// ============================================================================

bool are_heartbeats_enabled();
void set_heartbeats_enabled(bool enabled);

// ============================================================================
// Wake handler registration
// Aligned with openclaw: setHeartbeatWakeHandler
//
// Returns a disposer function that clears this specific registration.
// ============================================================================

std::function<void()> set_heartbeat_wake_handler(HeartbeatWakeHandler handler);

// ============================================================================
// Request a heartbeat run
// Aligned with openclaw: requestHeartbeat (heartbeat-wake.ts:716)
//
// 请求会先进入 pending 队列，再按合并窗口调度；调用方立即返回。
// ============================================================================

struct HeartbeatWakeRequest {
    std::optional<std::string> reason;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    HeartbeatWakeIntent intent = HeartbeatWakeIntent::Event;
    HeartbeatWakeSource source = HeartbeatWakeSource::Other;
    // Aligned with openclaw: opts.coalesceMs ?? DEFAULT_COALESCE_MS
    std::optional<int64_t> coalesce_ms;
};

void request_heartbeat(const HeartbeatWakeRequest& request);

// ----------------------------------------------------------------------------
// Legacy positional shim.
// 真源已把 requestHeartbeatNow 降级为 plugin-SDK 兼容接口（deprecated），
// 内部一律走 requestHeartbeat。此处保留位置参数形态以免改动既有调用点。
// ----------------------------------------------------------------------------

void request_heartbeat_now(
    const std::optional<std::string>& reason = std::nullopt,
    const std::optional<std::string>& agent_id = std::nullopt,
    const std::optional<std::string>& session_key = std::nullopt,
    HeartbeatWakeIntent intent = HeartbeatWakeIntent::Event,
    HeartbeatWakeSource source = HeartbeatWakeSource::Other
);

// ============================================================================
// Wake target key helpers
// Aligned with openclaw: src/infra/heartbeat-wake-target.ts
// ============================================================================

// Aligned with openclaw: GLOBAL_HEARTBEAT_WAKE_TARGET_KEY = "::"
extern const char* const GLOBAL_HEARTBEAT_WAKE_TARGET_KEY;

// Aligned with openclaw: resolveHeartbeatWakeTargetKey
//   sessionKey ? `::${sessionKey}` : `${agentId ?? ""}::`
std::string resolve_heartbeat_wake_target_key(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& session_key
);

// ============================================================================
// Test hooks（仅供单测断言调度状态，生产代码不应依赖）
// ============================================================================

namespace testing {
// 当前 pending 分组数量
size_t pending_wake_group_count();
// 当前正在执行的 target 数量
size_t active_wake_target_count();
// 阻塞直至所有 pending 与 active 清空，或超时。返回是否已排空。
bool drain_wakes_for_test(int64_t timeout_ms);
// 清空全部调度状态（不触发 handler）
void reset_wake_state_for_test();
}  // namespace testing

} // namespace infra
} // namespace hunterclaw
