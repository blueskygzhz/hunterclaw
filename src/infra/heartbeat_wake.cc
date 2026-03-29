// ============================================================================
// heartbeat_wake.cc
// Aligned with openclaw: src/infra/heartbeat-wake.ts
// ============================================================================

#include "hunterclaw/infra/heartbeat_wake.h"
#include <mutex>
#include <atomic>

namespace hunterclaw {
namespace infra {

// ── Module-level state (aligned with openclaw module-level variables) ────────

static std::mutex g_wake_mutex;
static std::atomic<bool> g_heartbeats_enabled{true};
static HeartbeatWakeHandler g_handler;
static uint64_t g_handler_generation = 0;

// ── are_heartbeats_enabled ───────────────────────────────────────────────────
// Aligned with openclaw: areHeartbeatsEnabled
//
// let heartbeatsEnabled = true;
// export function areHeartbeatsEnabled(): boolean { return heartbeatsEnabled; }

bool are_heartbeats_enabled() {
    return g_heartbeats_enabled.load();
}

// ── set_heartbeats_enabled ───────────────────────────────────────────────────
// Aligned with openclaw: setHeartbeatsEnabled
//
// export function setHeartbeatsEnabled(enabled: boolean) {
//   heartbeatsEnabled = enabled;
// }

void set_heartbeats_enabled(bool enabled) {
    g_heartbeats_enabled.store(enabled);
}

// ── set_heartbeat_wake_handler ───────────────────────────────────────────────
// Aligned with openclaw: setHeartbeatWakeHandler
//
// export function setHeartbeatWakeHandler(next): () => void {
//   handlerGeneration += 1;
//   const generation = handlerGeneration;
//   handler = next;
//   return () => { if (handlerGeneration !== generation) return; handler = null; };
// }

std::function<void()> set_heartbeat_wake_handler(HeartbeatWakeHandler handler) {
    std::lock_guard<std::mutex> lock(g_wake_mutex);
    g_handler_generation++;
    uint64_t generation = g_handler_generation;
    g_handler = std::move(handler);
    return [generation]() {
        std::lock_guard<std::mutex> lock(g_wake_mutex);
        if (g_handler_generation != generation) return;
        g_handler_generation++;
        g_handler = nullptr;
    };
}

// ── request_heartbeat_now ────────────────────────────────────────────────────
// Aligned with openclaw: requestHeartbeatNow
//
// export function requestHeartbeatNow(opts?) {
//   queuePendingWakeReason({ reason: opts?.reason, ... });
//   schedule(opts?.coalesceMs ?? DEFAULT_COALESCE_MS, "normal");
// }
//
// HunterClaw: simplified synchronous dispatch (no coalesce timer needed
// since C++ heartbeat runner uses its own thread-based scheduling).

void request_heartbeat_now(
    const std::optional<std::string>& reason,
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& session_key
) {
    HeartbeatWakeHandler handler_copy;
    {
        std::lock_guard<std::mutex> lock(g_wake_mutex);
        handler_copy = g_handler;
    }
    if (!handler_copy) return;
    HeartbeatWakeParams params;
    params.reason = reason;
    params.agent_id = agent_id;
    params.session_key = session_key;
    try {
        handler_copy(params);
    } catch (...) {
        // Aligned with openclaw: errors are logged by the runner, not here
    }
}

} // namespace infra
} // namespace hunterclaw
