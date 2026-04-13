// ============================================================================
// format_time.cc
// ✅ 严格对齐 OpenClaw: src/infra/format-time/ (6个文件, ~315行核心)
//
// 统一的时间格式化模块：
//   - formatTimeAgo: "just now" / "5m ago" / "3h ago" / "2d ago"
//   - formatDurationSeconds: "1.2s" / "1.23 seconds"
//   - formatDurationPrecise: "500ms" / "1.23s"
//   - formatDurationCompact: "500ms" / "45s" / "2m5s" / "1h30m" / "2d5h"
//   - formatDurationHuman: "500ms" / "5s" / "3m" / "2h" / "5d"
//   - formatUtcTimestamp: "2024-01-15T14:30Z"
//   - formatRelativeTimestamp: epoch→relative + date fallback
// ============================================================================

#include <string>
#include <optional>
#include <cmath>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace hunterclaw {
namespace infra {

// ============================================================================
// 对齐 format-relative.ts:26-52: formatTimeAgo
// ============================================================================
std::string format_time_ago(
    int64_t duration_ms,
    bool suffix,
    const std::string& fallback
) {
    if (duration_ms < 0) return fallback;

    int64_t total_seconds = std::llround(duration_ms / 1000.0);
    int64_t minutes = std::llround(total_seconds / 60.0);

    if (minutes < 1) {
        return suffix ? "just now" : std::to_string(total_seconds) + "s";
    }
    if (minutes < 60) {
        return suffix ? std::to_string(minutes) + "m ago" : std::to_string(minutes) + "m";
    }
    int64_t hours = std::llround(minutes / 60.0);
    if (hours < 48) {
        return suffix ? std::to_string(hours) + "h ago" : std::to_string(hours) + "h";
    }
    int64_t days = std::llround(hours / 24.0);
    return suffix ? std::to_string(days) + "d ago" : std::to_string(days) + "d";
}

// ============================================================================
// 对齐 format-duration.ts:11-23: formatDurationSeconds
// ============================================================================
std::string format_duration_seconds(int64_t ms, int decimals, const std::string& unit) {
    double seconds = std::max(0.0, static_cast<double>(ms)) / 1000.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(std::max(0, decimals)) << seconds;
    std::string fixed = oss.str();

    // Trim trailing zeros after decimal
    if (fixed.find('.') != std::string::npos) {
        size_t last_non_zero = fixed.find_last_not_of('0');
        if (fixed[last_non_zero] == '.') last_non_zero--;
        fixed = fixed.substr(0, last_non_zero + 1);
    }

    return (unit == "seconds") ? fixed + " seconds" : fixed + "s";
}

// ============================================================================
// 对齐 format-duration.ts:27-41: formatDurationPrecise
// ============================================================================
std::string format_duration_precise(int64_t ms, int decimals) {
    if (ms < 1000) {
        return std::to_string(std::max(int64_t(0), ms)) + "ms";
    }
    return format_duration_seconds(ms, (decimals > 0) ? decimals : 2, "s");
}

// ============================================================================
// 对齐 format-duration.ts:49-76: formatDurationCompact
// ============================================================================
std::optional<std::string> format_duration_compact(int64_t ms, bool spaced) {
    if (ms <= 0) return std::nullopt;
    if (ms < 1000) return std::to_string(ms) + "ms";

    std::string sep = spaced ? " " : "";
    int64_t total_seconds = std::llround(ms / 1000.0);
    int64_t hours = total_seconds / 3600;
    int64_t minutes = (total_seconds % 3600) / 60;
    int64_t seconds = total_seconds % 60;

    if (hours >= 24) {
        int64_t days = hours / 24;
        int64_t rem_hours = hours % 24;
        return (rem_hours > 0)
            ? std::to_string(days) + "d" + sep + std::to_string(rem_hours) + "h"
            : std::to_string(days) + "d";
    }
    if (hours > 0) {
        return (minutes > 0)
            ? std::to_string(hours) + "h" + sep + std::to_string(minutes) + "m"
            : std::to_string(hours) + "h";
    }
    if (minutes > 0) {
        return (seconds > 0)
            ? std::to_string(minutes) + "m" + sep + std::to_string(seconds) + "s"
            : std::to_string(minutes) + "m";
    }
    return std::to_string(seconds) + "s";
}

// ============================================================================
// 对齐 format-duration.ts:82-103: formatDurationHuman
// ============================================================================
std::string format_duration_human(int64_t ms, const std::string& fallback) {
    if (ms < 0) return fallback;
    if (ms < 1000) return std::to_string(ms) + "ms";
    int64_t sec = std::llround(ms / 1000.0);
    if (sec < 60) return std::to_string(sec) + "s";
    int64_t min = std::llround(sec / 60.0);
    if (min < 60) return std::to_string(min) + "m";
    int64_t hr = std::llround(min / 60.0);
    if (hr < 24) return std::to_string(hr) + "h";
    int64_t day = std::llround(hr / 24.0);
    return std::to_string(day) + "d";
}

// ============================================================================
// 对齐 format-datetime.ts:36-47: formatUtcTimestamp
// ============================================================================
std::string format_utc_timestamp(int64_t epoch_ms, bool display_seconds) {
    time_t seconds = epoch_ms / 1000;
    struct tm utc_tm;
    gmtime_r(&seconds, &utc_tm);

    char buf[32];
    if (display_seconds) {
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc_tm);
        return std::string(buf) + "Z";
    }
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", &utc_tm);
    return std::string(buf) + "Z";
}

// ============================================================================
// 对齐 format-relative.ts:54-113: formatRelativeTimestamp
// ============================================================================
std::string format_relative_timestamp(
    int64_t epoch_ms,
    bool date_fallback,
    const std::string& fallback
) {
    if (epoch_ms <= 0) return fallback;

    auto now = std::chrono::system_clock::now();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    int64_t diff = now_ms - epoch_ms;

    // Future timestamps
    if (diff < -60000) {
        return format_utc_timestamp(epoch_ms, false);
    }

    // Recent (< 7 days)
    if (diff >= 0 && diff < 7LL * 24 * 60 * 60 * 1000) {
        return format_time_ago(diff, true, fallback);
    }

    // > 7 days: use date fallback or full timestamp
    if (date_fallback) {
        time_t seconds = epoch_ms / 1000;
        struct tm local_tm;
        localtime_r(&seconds, &local_tm);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%b %d", &local_tm);
        return buf;
    }

    return format_utc_timestamp(epoch_ms, false);
}

} // namespace infra
} // namespace hunterclaw
