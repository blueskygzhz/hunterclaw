// ============================================================================
// heartbeat_active_hours.cc
// Aligned with openclaw: src/infra/heartbeat-active-hours.ts
// ============================================================================

#include "hunterclaw/infra/heartbeat_active_hours.h"
#include <chrono>
#include <regex>
#include <ctime>

namespace hunterclaw {
namespace infra {

// ── Helpers ──────────────────────────────────────────────────────────────────

static const std::regex ACTIVE_HOURS_TIME_PATTERN{
    R"(^(?:([01]\d|2[0-3]):([0-5]\d)|24:00)$)"
};

// Aligned with openclaw: parseActiveHoursTime
// Returns minutes since midnight, or -1 on parse failure.
static int parse_active_hours_time(const std::string& raw, bool allow24) {
    if (raw.empty()) return -1;
    std::smatch m;
    if (!std::regex_match(raw, m, ACTIVE_HOURS_TIME_PATTERN)) return -1;
    int hour   = std::stoi(raw.substr(0, 2));
    int minute = std::stoi(raw.substr(3, 2));
    if (hour == 24) {
        if (!allow24 || minute != 0) return -1;
        return 24 * 60;
    }
    return hour * 60 + minute;
}

// Aligned with openclaw: resolveMinutesInTimeZone
// Returns minutes since midnight in the given timezone, or -1 on failure.
// Uses POSIX TZ trick since C++ stdlib doesn't have IANA tz support natively.
static int resolve_minutes_in_timezone(int64_t now_ms, const std::string& tz) {
    // Use the system's localtime with TZ env var override
    // This is the most portable approach without external tz libraries
    time_t t = static_cast<time_t>(now_ms / 1000);

    // Save and override TZ
    const char* old_tz = getenv("TZ");
    std::string old_tz_str = old_tz ? old_tz : "";
    bool had_tz = (old_tz != nullptr);

    setenv("TZ", tz.c_str(), 1);
    tzset();

    struct tm local_tm{};
    localtime_r(&t, &local_tm);

    // Restore TZ
    if (had_tz) {
        setenv("TZ", old_tz_str.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();

    return local_tm.tm_hour * 60 + local_tm.tm_min;
}

// Aligned with openclaw: resolveActiveHoursTimezone
static std::string resolve_active_hours_timezone(
    const std::string& raw,
    const std::string& user_timezone
) {
    const std::string trimmed = [&]() {
        size_t s = raw.find_first_not_of(" \t\n\r");
        if (s == std::string::npos) return std::string{};
        size_t e = raw.find_last_not_of(" \t\n\r");
        return raw.substr(s, e - s + 1);
    }();

    if (trimmed.empty() || trimmed == "user") {
        return user_timezone.empty() ? "UTC" : user_timezone;
    }
    if (trimmed == "local") {
        // Use system local timezone
        const char* tz = getenv("TZ");
        return (tz && *tz) ? tz : "UTC";
    }
    return trimmed;
}

// ── is_within_active_hours ───────────────────────────────────────────────────
// Aligned with openclaw: isWithinActiveHours

bool is_within_active_hours(
    const std::optional<ActiveHoursConfig>& active_hours,
    const std::string& user_timezone,
    int64_t now_ms
) {
    // Aligned with openclaw: if (!active) return true;
    if (!active_hours.has_value()) return true;

    const auto& active = active_hours.value();

    // Aligned with openclaw: parseActiveHoursTime
    const int start_min = parse_active_hours_time(active.start, /*allow24=*/false);
    const int end_min   = parse_active_hours_time(active.end,   /*allow24=*/true);

    // Aligned with openclaw: if (startMin === null || endMin === null) return true;
    if (start_min < 0 || end_min < 0) return true;

    // Aligned with openclaw: if (startMin === endMin) return false;
    if (start_min == end_min) return false;

    const std::string tz = resolve_active_hours_timezone(active.timezone, user_timezone);

    // Use current time if now_ms == 0
    const int64_t ts = (now_ms > 0) ? now_ms
        : std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()
          ).count();

    const int current_min = resolve_minutes_in_timezone(ts, tz);
    // Aligned with openclaw: if (currentMin === null) return true;
    if (current_min < 0) return true;

    // Aligned with openclaw:
    //   if (endMin > startMin) return currentMin >= startMin && currentMin < endMin;
    //   return currentMin >= startMin || currentMin < endMin;
    if (end_min > start_min) {
        return current_min >= start_min && current_min < end_min;
    }
    return current_min >= start_min || current_min < end_min;
}

} // namespace infra
} // namespace hunterclaw
