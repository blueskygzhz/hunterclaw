#pragma once

// ============================================================================
// heartbeat_events_filter.h
// Aligned with openclaw: src/infra/heartbeat-events-filter.ts
//
// Provides:
//   - build_cron_event_prompt    (aligned with buildCronEventPrompt)
//   - build_exec_event_prompt    (aligned with buildExecEventPrompt)
//   - is_exec_completion_event   (aligned with isExecCompletionEvent)
//   - is_cron_system_event       (aligned with isCronSystemEvent)
// ============================================================================

#include <string>
#include <vector>

namespace hunterclaw {
namespace infra {

// ── build_cron_event_prompt ──────────────────────────────────────────────────
// Aligned with openclaw: buildCronEventPrompt

std::string build_cron_event_prompt(
    const std::vector<std::string>& pending_events,
    bool deliver_to_user = true
);

// ── build_exec_event_prompt ──────────────────────────────────────────────────
// Aligned with openclaw: buildExecEventPrompt

std::string build_exec_event_prompt(bool deliver_to_user = true);

// ── is_exec_completion_event ─────────────────────────────────────────────────
// Aligned with openclaw: isExecCompletionEvent
// Returns true if the event text contains "exec finished" (case-insensitive).

bool is_exec_completion_event(const std::string& evt);

// ── is_cron_system_event ─────────────────────────────────────────────────────
// Aligned with openclaw: isCronSystemEvent
// Returns true when a system event should be treated as real cron reminder content.
// Excludes heartbeat noise events and exec completion events.

bool is_cron_system_event(const std::string& evt);

} // namespace infra
} // namespace hunterclaw
