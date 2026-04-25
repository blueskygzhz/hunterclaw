#pragma once

// ============================================================================
// agent_events.h
// Aligned with openclaw: src/infra/agent-events.ts
//
// Provides:
//   - AgentEvent              (aligned with AgentEvent)
//   - emit_agent_event        (aligned with emitAgentEvent)
//   - on_agent_event          (aligned with onAgentEvent)
//
// Agent events are emitted during agent runs (text deltas, tool calls,
// lifecycle events). Subscribers can listen to all events or filter by runId.
//
// Used by:
//   - acp_spawn_parent_stream.cc: subscribe to child run events for relay
//   - gateway/methods/agent.cc: emit events during run
// ============================================================================

#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace infra {

// ============================================================================
// AgentEvent
// Aligned with openclaw: AgentEvent
// ============================================================================

struct AgentEvent {
    std::string run_id;       // run ID (idempotency key)
    std::string stream;       // "assistant" | "lifecycle" | "tool"
    nlohmann::json data;      // event payload

    // Convenience accessors
    std::string phase() const {
        if (data.contains("phase") && data["phase"].is_string())
            return data["phase"].get<std::string>();
        return "";
    }
    std::string delta() const {
        if (data.contains("delta") && data["delta"].is_string())
            return data["delta"].get<std::string>();
        if (data.contains("text") && data["text"].is_string())
            return data["text"].get<std::string>();
        return "";
    }
    // Aligned with openclaw: assistantPhase — "commentary" | "main" | "" (empty = main)
    // Used by parent stream relay to skip commentary-phase deltas from relay
    std::string assistant_phase() const {
        if (data.contains("assistantPhase") && data["assistantPhase"].is_string())
            return data["assistantPhase"].get<std::string>();
        if (data.contains("assistant_phase") && data["assistant_phase"].is_string())
            return data["assistant_phase"].get<std::string>();
        return "";
    }
};

// ============================================================================
// emit_agent_event
// Aligned with openclaw: emitAgentEvent
//
// Emits an agent event to all registered listeners.
// Thread-safe.
// ============================================================================

void emit_agent_event(const AgentEvent& event);

// ============================================================================
// on_agent_event
// Aligned with openclaw: onAgentEvent
//
// Registers a listener for agent events.
// Returns a disposer function that removes the listener.
// Thread-safe.
// ============================================================================

using AgentEventListener = std::function<void(const AgentEvent&)>;

std::function<void()> on_agent_event(AgentEventListener listener);

} // namespace infra
} // namespace hunterclaw
