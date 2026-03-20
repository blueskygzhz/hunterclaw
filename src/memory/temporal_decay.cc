#include "hunterclaw/memory/temporal_decay.h"
#include <cmath>
#include <regex>
#include <algorithm>
#include <chrono>
#include <sys/stat.h>
#include <map>

// 严格对齐 OpenClaw src/memory/temporal-decay.ts

namespace hunterclaw {
namespace memory {

constexpr int64_t DAY_MS = 24LL * 60 * 60 * 1000;
const std::regex DATED_MEMORY_PATH_RE(R"((?:^|\/)memory\/(\d{4})-(\d{2})-(\d{2})\.md$)");

// ============================================================================
// toDecayLambda (对齐 temporal-decay.ts:toDecayLambda)
// ============================================================================
double to_decay_lambda(double half_life_days) {
    if (!std::isfinite(half_life_days) || half_life_days <= 0.0) {
        return 0.0;
    }
    return M_LN2 / half_life_days;
}

// ============================================================================
// calculateTemporalDecayMultiplier (对齐 temporal-decay.ts:calculateTemporalDecayMultiplier)
// ============================================================================
double calculate_temporal_decay_multiplier(double age_in_days, double half_life_days) {
    double lambda = to_decay_lambda(half_life_days);
    double clamped_age = std::max(0.0, age_in_days);
    if (lambda <= 0.0 || !std::isfinite(clamped_age)) {
        return 1.0;
    }
    return std::exp(-lambda * clamped_age);
}

// ============================================================================
// applyTemporalDecayToScore (对齐 temporal-decay.ts:applyTemporalDecayToScore)
// ============================================================================
double apply_temporal_decay_to_score(double score, double age_in_days, double half_life_days) {
    return score * calculate_temporal_decay_multiplier(age_in_days, half_life_days);
}

// ============================================================================
// parseMemoryDateFromPath (对齐 temporal-decay.ts:parseMemoryDateFromPath)
// ============================================================================
std::chrono::system_clock::time_point parse_memory_date_from_path(const std::string& file_path) {
    std::string normalized = file_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.size() >= 2 && normalized[0] == '.' && normalized[1] == '/') {
        normalized = normalized.substr(2);
    }

    std::smatch match;
    if (!std::regex_search(normalized, match, DATED_MEMORY_PATH_RE)) {
        return std::chrono::system_clock::time_point{};
    }

    try {
        int year  = std::stoi(match[1].str());
        int month = std::stoi(match[2].str());
        int day   = std::stoi(match[3].str());

        // 对齐 openclaw: Date.UTC(year, month-1, day)
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon  = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = 0;
        tm.tm_min  = 0;
        tm.tm_sec  = 0;

        time_t t = timegm(&tm);
        if (t == -1) return std::chrono::system_clock::time_point{};

        // 验证 (对齐 openclaw: parsed.getUTCFullYear() !== year ...)
        std::tm* check = gmtime(&t);
        if (!check ||
            check->tm_year + 1900 != year ||
            check->tm_mon + 1 != month ||
            check->tm_mday != day) {
            return std::chrono::system_clock::time_point{};
        }

        return std::chrono::system_clock::from_time_t(t);
    } catch (...) {
        return std::chrono::system_clock::time_point{};
    }
}

// ============================================================================
// isEvergreenMemoryPath (对齐 temporal-decay.ts:isEvergreenMemoryPath)
// ============================================================================
bool is_evergreen_memory_path(const std::string& file_path) {
    std::string normalized = file_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.size() >= 2 && normalized[0] == '.' && normalized[1] == '/') {
        normalized = normalized.substr(2);
    }

    // 对齐 openclaw: "MEMORY.md" || "memory.md"
    if (normalized == "MEMORY.md" || normalized == "memory.md") {
        return true;
    }

    // 对齐 openclaw: !normalized.startsWith("memory/")
    if (normalized.rfind("memory/", 0) != 0) {
        return false;
    }

    // 对齐 openclaw: !DATED_MEMORY_PATH_RE.test(normalized)
    std::smatch match;
    return !std::regex_search(normalized, match, DATED_MEMORY_PATH_RE);
}

// ============================================================================
// extractTimestamp (对齐 temporal-decay.ts:extractTimestamp)
// ============================================================================
std::chrono::system_clock::time_point extract_timestamp(
    const std::string& file_path,
    const std::string& source,
    const std::string& workspace_dir
) {
    // 1. 尝试从路径解析日期 (对齐 parseMemoryDateFromPath)
    auto from_path = parse_memory_date_from_path(file_path);
    if (from_path != std::chrono::system_clock::time_point{}) {
        return from_path;
    }

    // 2. Memory evergreen 文件不衰减 (对齐 isEvergreenMemoryPath)
    if (source == "memory" && is_evergreen_memory_path(file_path)) {
        return std::chrono::system_clock::time_point{};
    }

    // 3. 无 workspace_dir 则无法获取 mtime
    if (workspace_dir.empty()) {
        return std::chrono::system_clock::time_point{};
    }

    // 4. 构建绝对路径并获取 mtime (对齐 fs.stat)
    std::string absolute_path = file_path;
    if (!file_path.empty() && file_path[0] != '/') {
        absolute_path = workspace_dir + "/" + file_path;
    }

    struct stat st;
    if (stat(absolute_path.c_str(), &st) != 0) {
        return std::chrono::system_clock::time_point{};
    }

    // 对齐 openclaw: !Number.isFinite(stat.mtimeMs)
    int64_t mtime_ms = static_cast<int64_t>(st.st_mtime) * 1000;
    if (mtime_ms <= 0) {
        return std::chrono::system_clock::time_point{};
    }

    return std::chrono::system_clock::from_time_t(st.st_mtime);
}

// ============================================================================
// ageInDaysFromTimestamp (对齐 temporal-decay.ts:ageInDaysFromTimestamp)
// ============================================================================
double age_in_days_from_timestamp(
    const std::chrono::system_clock::time_point& timestamp,
    int64_t now_ms
) {
    if (timestamp == std::chrono::system_clock::time_point{}) {
        return 0.0;
    }
    int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()).count();
    int64_t age_ms = std::max<int64_t>(0, now_ms - ts_ms);
    return static_cast<double>(age_ms) / DAY_MS;
}

// ============================================================================
// applyTemporalDecayToHybridResults (对齐 temporal-decay.ts:applyTemporalDecayToHybridResults)
// ============================================================================
std::vector<TemporalDecayEntry> apply_temporal_decay_to_hybrid_results(
    std::vector<TemporalDecayEntry> results,
    const TemporalDecayConfig& config,
    const std::string& workspace_dir,
    int64_t now_ms
) {
    // 对齐 openclaw: if (!config.enabled) return [...params.results]
    if (!config.enabled) {
        return results;
    }

    if (now_ms == 0) {
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 对齐 openclaw: timestampPromiseCache (按 source:path 缓存)
    std::map<std::string, std::chrono::system_clock::time_point> timestamp_cache;

    for (auto& entry : results) {
        std::string cache_key = entry.source + ":" + entry.path;
        auto it = timestamp_cache.find(cache_key);
        std::chrono::system_clock::time_point timestamp;
        if (it == timestamp_cache.end()) {
            timestamp = extract_timestamp(entry.path, entry.source, workspace_dir);
            timestamp_cache[cache_key] = timestamp;
        } else {
            timestamp = it->second;
        }

        // 对齐 openclaw: if (!timestamp) return entry (不衰减)
        if (timestamp == std::chrono::system_clock::time_point{}) {
            continue;
        }

        double age_in_days = age_in_days_from_timestamp(timestamp, now_ms);
        entry.score = apply_temporal_decay_to_score(entry.score, age_in_days, config.half_life_days);
    }

    // 对齐 openclaw: 直接返回（不排序，排序由调用方 mergeHybridResults 负责）
    return results;
}

} // namespace memory
} // namespace hunterclaw
