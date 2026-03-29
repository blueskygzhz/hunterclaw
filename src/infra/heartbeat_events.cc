// ============================================================================
// heartbeat_events.cc
// Aligned with openclaw: src/infra/heartbeat-events.ts
// ============================================================================

#include "hunterclaw/infra/heartbeat_events.h"
#include <chrono>
#include <mutex>
#include <set>
#include <functional>
#include <optional>

namespace hunterclaw {
namespace infra {

// ── Module-level state (aligned with openclaw module-level variables) ────────

static std::mutex g_heartbeat_mutex;
static std::optional<HeartbeatEventPayload> g_last_heartbeat;

// Listener registry: use a set of shared_ptr<Listener> for safe removal
struct ListenerEntry {
    uint64_t id;
    HeartbeatEventListener fn;
};
static std::vector<ListenerEntry> g_listeners;
static uint64_t g_next_listener_id = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ── emit_heartbeat_event ─────────────────────────────────────────────────────
// Aligned with openclaw: emitHeartbeatEvent
//
// export function emitHeartbeatEvent(evt: Omit<HeartbeatEventPayload, "ts">) {
//   const enriched: HeartbeatEventPayload = { ts: Date.now(), ...evt };
//   lastHeartbeat = enriched;
//   for (const listener of listeners) { try { listener(enriched); } catch {} }
// }

void emit_heartbeat_event(const HeartbeatEventPayload& evt) {
    HeartbeatEventPayload enriched = evt;
    if (enriched.ts == 0) {
        enriched.ts = now_ms();
    }

    // Snapshot listeners under lock, then call outside lock to avoid deadlock
    std::vector<HeartbeatEventListener> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
        g_last_heartbeat = enriched;
        snapshot.reserve(g_listeners.size());
        for (const auto& entry : g_listeners) {
            snapshot.push_back(entry.fn);
        }
    }

    for (const auto& fn : snapshot) {
        try {
            fn(enriched);
        } catch (...) {
            // Aligned with openclaw: ignore listener errors
        }
    }
}

// ── on_heartbeat_event ───────────────────────────────────────────────────────
// Aligned with openclaw: onHeartbeatEvent
//
// export function onHeartbeatEvent(listener): () => void {
//   listeners.add(listener);
//   return () => listeners.delete(listener);
// }

std::function<void()> on_heartbeat_event(HeartbeatEventListener listener) {
    std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
    uint64_t id = g_next_listener_id++;
    g_listeners.push_back({id, std::move(listener)});
    return [id]() {
        std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
        g_listeners.erase(
            std::remove_if(g_listeners.begin(), g_listeners.end(),
                [id](const ListenerEntry& e) { return e.id == id; }),
            g_listeners.end()
        );
    };
}

// ── get_last_heartbeat_event ─────────────────────────────────────────────────
// Aligned with openclaw: getLastHeartbeatEvent
//
// export function getLastHeartbeatEvent(): HeartbeatEventPayload | null {
//   return lastHeartbeat;
// }

std::optional<HeartbeatEventPayload> get_last_heartbeat_event() {
    std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
    return g_last_heartbeat;
}

} // namespace infra
} // namespace hunterclaw
