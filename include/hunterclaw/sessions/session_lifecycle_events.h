#pragma once

// 严格对齐 OpenClaw: sessions/session-lifecycle-events.ts (29行)
// Session lifecycle event system — pub/sub for session create/delete/reset.

#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace hunterclaw {
namespace sessions {

// ============================================================================
// 对齐 OpenClaw session-lifecycle-events.ts:1-7: SessionLifecycleEvent
// ============================================================================

struct SessionLifecycleEvent {
    std::string session_key;
    std::string reason;                         // "created" | "deleted" | "reset" | "compacted" etc.
    std::optional<std::string> parent_session_key;
    std::optional<std::string> label;
    std::optional<std::string> display_name;
};

// ============================================================================
// 对齐 OpenClaw session-lifecycle-events.ts:9-28
// ============================================================================

using SessionLifecycleListener = std::function<void(const SessionLifecycleEvent&)>;

/**
 * Register a listener for session lifecycle events.
 * Returns an unsubscribe function.
 * 对齐: onSessionLifecycleEvent
 */
std::function<void()> on_session_lifecycle_event(SessionLifecycleListener listener);

/**
 * Emit a session lifecycle event to all registered listeners.
 * Best-effort: listener errors are silently caught.
 * 对齐: emitSessionLifecycleEvent
 */
void emit_session_lifecycle_event(const SessionLifecycleEvent& event);

}  // namespace sessions
}  // namespace hunterclaw
