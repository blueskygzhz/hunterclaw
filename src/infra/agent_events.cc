// agent_events.cc
// Aligned with openclaw: src/infra/agent-events.ts

#include "hunterclaw/infra/agent_events.h"
#include <mutex>
#include <map>
#include <atomic>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace infra {

namespace {

struct AgentEventRegistry {
    std::mutex mutex;
    std::map<uint64_t, AgentEventListener> listeners;
    std::atomic<uint64_t> next_id{1};
};

AgentEventRegistry& registry() {
    static AgentEventRegistry reg;
    return reg;
}

} // namespace

void emit_agent_event(const AgentEvent& event) {
    auto& reg = registry();
    std::vector<AgentEventListener> snapshot;
    {
        std::lock_guard<std::mutex> lock(reg.mutex);
        snapshot.reserve(reg.listeners.size());
        for (const auto& [id, listener] : reg.listeners) {
            snapshot.push_back(listener);
        }
    }
    for (const auto& listener : snapshot) {
        try {
            listener(event);
        } catch (const std::exception& e) {
            auto logger = spdlog::get("default");
            if (logger) logger->warn("[agent_events] listener threw: {}", e.what());
        } catch (...) {}
    }
}

std::function<void()> on_agent_event(AgentEventListener listener) {
    auto& reg = registry();
    uint64_t id = reg.next_id.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(reg.mutex);
        reg.listeners[id] = std::move(listener);
    }
    return [id]() {
        auto& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        reg.listeners.erase(id);
    };
}

} // namespace infra
} // namespace hunterclaw
