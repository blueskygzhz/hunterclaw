#pragma once

// ============================================================================
// system_events.h
// Aligned with openclaw: src/infra/system-events.ts
//
// Lightweight in-memory queue for human-readable system events that should be
// prefixed to the next prompt. Events are session-scoped and ephemeral.
//
// Provides:
//   - SystemEvent                  (aligned with SystemEvent)
//   - enqueue_system_event         (aligned with enqueueSystemEvent)
//   - drain_system_event_entries   (aligned with drainSystemEventEntries)
//   - drain_system_events          (aligned with drainSystemEvents)
//   - peek_system_event_entries    (aligned with peekSystemEventEntries)
//   - peek_system_events           (aligned with peekSystemEvents)
//   - has_system_events            (aligned with hasSystemEvents)
//   - is_system_event_context_changed (aligned with isSystemEventContextChanged)
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace hunterclaw {
namespace infra {

// ── SystemEvent ──────────────────────────────────────────────────────────────
// Aligned with openclaw: SystemEvent

struct SystemEvent {
    std::string text;
    int64_t ts = 0;
    std::optional<std::string> context_key;  // aligned with contextKey
};

// ── enqueue_system_event ─────────────────────────────────────────────────────
// Aligned with openclaw: enqueueSystemEvent
// Returns true if the event was added (false if duplicate or empty).

bool enqueue_system_event(
    const std::string& text,
    const std::string& session_key,
    const std::optional<std::string>& context_key = std::nullopt
);

// ── drain_system_event_entries ───────────────────────────────────────────────
// Aligned with openclaw: drainSystemEventEntries
// Returns all events and clears the queue.

std::vector<SystemEvent> drain_system_event_entries(const std::string& session_key);

// ── drain_system_events ──────────────────────────────────────────────────────
// Aligned with openclaw: drainSystemEvents
// Returns event texts and clears the queue.

std::vector<std::string> drain_system_events(const std::string& session_key);

// ── peek_system_event_entries ────────────────────────────────────────────────
// Aligned with openclaw: peekSystemEventEntries
// Returns a copy of all events without clearing.

std::vector<SystemEvent> peek_system_event_entries(const std::string& session_key);

// ── peek_system_events ───────────────────────────────────────────────────────
// Aligned with openclaw: peekSystemEvents

std::vector<std::string> peek_system_events(const std::string& session_key);

// ── has_system_events ────────────────────────────────────────────────────────
// Aligned with openclaw: hasSystemEvents

bool has_system_events(const std::string& session_key);

// ── is_system_event_context_changed ─────────────────────────────────────────
// Aligned with openclaw: isSystemEventContextChanged

bool is_system_event_context_changed(
    const std::string& session_key,
    const std::optional<std::string>& context_key = std::nullopt
);

} // namespace infra
} // namespace hunterclaw
