#pragma once

// ============================================================================
// heartbeat_wake.h
// Aligned with openclaw: src/infra/heartbeat-wake.ts
//
// Provides:
//   - HeartbeatRunResult         (aligned with HeartbeatRunResult)
//   - are_heartbeats_enabled     (aligned with areHeartbeatsEnabled)
//   - set_heartbeats_enabled     (aligned with setHeartbeatsEnabled)
//   - request_heartbeat_now      (aligned with requestHeartbeatNow)
//   - HeartbeatWakeHandler       (aligned with HeartbeatWakeHandler)
//   - set_heartbeat_wake_handler (aligned with setHeartbeatWakeHandler)
// ============================================================================

#include <string>
#include <functional>
#include <optional>

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
// HeartbeatWakeHandler
// Aligned with openclaw: HeartbeatWakeHandler
// ============================================================================

struct HeartbeatWakeParams {
    std::optional<std::string> reason;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
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
// Request an immediate heartbeat run
// Aligned with openclaw: requestHeartbeatNow
// ============================================================================

void request_heartbeat_now(
    const std::optional<std::string>& reason = std::nullopt,
    const std::optional<std::string>& agent_id = std::nullopt,
    const std::optional<std::string>& session_key = std::nullopt
);

} // namespace infra
} // namespace hunterclaw
