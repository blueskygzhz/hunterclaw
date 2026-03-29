// ============================================================================
// heartbeat_reason.cc
// Aligned with openclaw: src/infra/heartbeat-reason.ts
// ============================================================================

#include "hunterclaw/infra/heartbeat_reason.h"
#include <algorithm>

namespace hunterclaw {
namespace infra {

static std::string trim_reason(const std::string& reason) {
    const std::string& s = reason;
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// ── normalize_heartbeat_wake_reason ─────────────────────────────────────────
// Aligned with openclaw: normalizeHeartbeatWakeReason

std::string normalize_heartbeat_wake_reason(const std::string& reason) {
    const std::string trimmed = trim_reason(reason);
    return trimmed.empty() ? "requested" : trimmed;
}

// ── resolve_heartbeat_reason_kind ────────────────────────────────────────────
// Aligned with openclaw: resolveHeartbeatReasonKind

HeartbeatReasonKind resolve_heartbeat_reason_kind(const std::string& reason) {
    const std::string trimmed = trim_reason(reason);
    if (trimmed == "retry")      return HeartbeatReasonKind::Retry;
    if (trimmed == "interval")   return HeartbeatReasonKind::Interval;
    if (trimmed == "manual")     return HeartbeatReasonKind::Manual;
    if (trimmed == "exec-event") return HeartbeatReasonKind::ExecEvent;
    if (trimmed == "wake")       return HeartbeatReasonKind::Wake;
    // Aligned with openclaw: trimmed.startsWith("acp:spawn:") → Wake
    if (trimmed.rfind("acp:spawn:", 0) == 0) return HeartbeatReasonKind::Wake;
    // Aligned with openclaw: trimmed.startsWith("cron:") → Cron
    if (trimmed.rfind("cron:", 0) == 0) return HeartbeatReasonKind::Cron;
    // Aligned with openclaw: trimmed.startsWith("hook:") → Hook
    if (trimmed.rfind("hook:", 0) == 0) return HeartbeatReasonKind::Hook;
    return HeartbeatReasonKind::Other;
}

// ── is_heartbeat_event_driven_reason ────────────────────────────────────────
// Aligned with openclaw: isHeartbeatEventDrivenReason

bool is_heartbeat_event_driven_reason(const std::string& reason) {
    const auto kind = resolve_heartbeat_reason_kind(reason);
    return kind == HeartbeatReasonKind::ExecEvent
        || kind == HeartbeatReasonKind::Cron
        || kind == HeartbeatReasonKind::Wake
        || kind == HeartbeatReasonKind::Hook;
}

// ── is_heartbeat_action_wake_reason ─────────────────────────────────────────
// Aligned with openclaw: isHeartbeatActionWakeReason

bool is_heartbeat_action_wake_reason(const std::string& reason) {
    const auto kind = resolve_heartbeat_reason_kind(reason);
    return kind == HeartbeatReasonKind::Manual
        || kind == HeartbeatReasonKind::ExecEvent
        || kind == HeartbeatReasonKind::Hook;
}

} // namespace infra
} // namespace hunterclaw
