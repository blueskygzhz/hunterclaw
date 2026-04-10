#include "hunterclaw/sessions/session_lifecycle_events.h"
#include <iostream>
#include <vector>

namespace hunterclaw {
namespace sessions {

namespace {
// 对齐 OpenClaw: module-level Set<SessionLifecycleListener>
struct ListenerRegistry {
    std::mutex mutex;
    std::vector<SessionLifecycleListener> listeners;
    std::vector<bool> active;  // Soft-delete flags for unsubscribe
};

ListenerRegistry& get_listener_registry() {
    static ListenerRegistry registry;
    return registry;
}
}  // namespace

// 对齐 OpenClaw session-lifecycle-events.ts:13-18
std::function<void()> on_session_lifecycle_event(SessionLifecycleListener listener) {
    auto& registry = get_listener_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    
    size_t index = registry.listeners.size();
    registry.listeners.push_back(std::move(listener));
    registry.active.push_back(true);
    
    return [index]() {
        auto& reg = get_listener_registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        if (index < reg.active.size()) {
            reg.active[index] = false;
        }
    };
}

// 对齐 OpenClaw session-lifecycle-events.ts:20-28
void emit_session_lifecycle_event(const SessionLifecycleEvent& event) {
    auto& registry = get_listener_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    
    for (size_t i = 0; i < registry.listeners.size(); ++i) {
        if (!registry.active[i]) continue;
        try {
            registry.listeners[i](event);
        } catch (...) {
            // Best-effort, do not propagate listener errors.
        }
    }
}

}  // namespace sessions
}  // namespace hunterclaw
