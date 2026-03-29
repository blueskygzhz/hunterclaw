#pragma once

// ============================================================================
// heartbeat_active_hours.h
// Aligned with openclaw: src/infra/heartbeat-active-hours.ts
// ============================================================================

#include <string>
#include <optional>
#include <cstdint>

namespace hunterclaw {
namespace infra {

// ── ActiveHoursConfig ────────────────────────────────────────────────────────
// Aligned with openclaw: HeartbeatConfig.activeHours

struct ActiveHoursConfig {
    std::string start;     // "HH:MM" e.g. "09:00"
    std::string end;       // "HH:MM" or "24:00" e.g. "22:00"
    std::string timezone;  // "user" | "local" | IANA tz e.g. "Asia/Shanghai"
};

// ── is_within_active_hours ───────────────────────────────────────────────────
// Aligned with openclaw: isWithinActiveHours
//
// Returns true if the current time (now_ms) is within the configured active hours.
// If active_hours is nullopt, always returns true (no restriction).
// user_timezone is the user's configured timezone (from agents.defaults.userTimezone).

bool is_within_active_hours(
    const std::optional<ActiveHoursConfig>& active_hours,
    const std::string& user_timezone = "UTC",
    int64_t now_ms = 0  // 0 = use current time
);

} // namespace infra
} // namespace hunterclaw
