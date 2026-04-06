// ============================================================================
// heartbeat_wake.cc
// Aligned with openclaw: src/infra/heartbeat-wake.ts
// ============================================================================

#include "hunterclaw/infra/heartbeat_wake.h"
#include <mutex>
#include <atomic>
#include <thread>

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
// ✅ 严格对齐 OpenClaw: requestHeartbeatNow (heartbeat-wake.ts)
//
// OpenClaw 实现:
//   export function requestHeartbeatNow(opts?) {
//     queuePendingWakeReason({ reason: opts?.reason, ... });
//     schedule(opts?.coalesceMs ?? DEFAULT_COALESCE_MS, "normal");
//   }
//   function schedule(ms, priority) {
//     if (pendingTimer) return;                // coalesce: 已有 timer 就跳过
//     pendingTimer = setTimeout(() => {
//       handler(drainPendingReasons());        // 异步执行
//     }, ms);
//   }
//
// 关键行为: requestHeartbeatNow 是异步的 (setTimeout)
//   调用方立即返回，handler 在 event loop 的下一个 tick 执行
//   这对 cron wakeMode=next-heartbeat 路径至关重要:
//     cron 线程不应该被 LLM 调用阻塞 (对齐 OpenClaw: cron 立即返回 OK)
//
// HunterClaw 对齐:
//   使用 detached thread 模拟 setTimeout 的异步语义
//   handler 在独立线程中执行，调用方立即返回

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

    // ✅ 对齐 OpenClaw: 异步执行 handler (模拟 setTimeout)
    // 参数按值捕获，确保 detached thread 的生命周期安全
    HeartbeatWakeParams params;
    params.reason = reason;
    params.agent_id = agent_id;
    params.session_key = session_key;

    std::thread([handler_copy, params]() {
        try {
            handler_copy(params);
        } catch (...) {
            // Aligned with openclaw: errors are logged by the runner, not here
        }
    }).detach();
}

} // namespace infra
} // namespace hunterclaw
