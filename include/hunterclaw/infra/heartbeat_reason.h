#pragma once

// ============================================================================
// heartbeat_reason.h
// Aligned with openclaw: src/infra/heartbeat-reason.ts
// ============================================================================

#include <string>

namespace hunterclaw {
namespace infra {

// ── HeartbeatReasonKind ──────────────────────────────────────────────────────
// Aligned with openclaw: HeartbeatReasonKind

enum class HeartbeatReasonKind {
    Retry,      // "retry"
    Interval,   // "interval"
    Manual,     // "manual"
    ExecEvent,  // "exec-event"
    Wake,       // "wake"
    Cron,       // "cron"
    Hook,       // "hook"
    Other,      // (default)
};

// ── normalizeHeartbeatWakeReason ─────────────────────────────────────────────
// Aligned with openclaw: normalizeHeartbeatWakeReason
// Returns trimmed reason, or "requested" if empty.

std::string normalize_heartbeat_wake_reason(const std::string& reason = "");

// ── resolveHeartbeatReasonKind ───────────────────────────────────────────────
// Aligned with openclaw: resolveHeartbeatReasonKind

HeartbeatReasonKind resolve_heartbeat_reason_kind(const std::string& reason = "");

// ── isHeartbeatEventDrivenReason ─────────────────────────────────────────────
// Aligned with openclaw: isHeartbeatEventDrivenReason
// Returns true for exec-event, cron, wake, hook.

bool is_heartbeat_event_driven_reason(const std::string& reason = "");

// ── isHeartbeatActionWakeReason ──────────────────────────────────────────────
// Aligned with openclaw: isHeartbeatActionWakeReason
// Returns true for manual, exec-event, hook.

bool is_heartbeat_action_wake_reason(const std::string& reason = "");

} // namespace infra
} // namespace hunterclaw
