#include "hunterclaw/memory/temporal_decay.h"
#include <cmath>
#include <regex>
#include <sys/stat.h>

namespace hunterclaw {
namespace memory {

constexpr int64_t DAY_MS = 24LL * 60 * 60 * 1000;
const std::regex DATED_MEMORY_PATH_RE(R"((?:^|\/)memory\/(\d{4})-(\d{2})-(\d{2})\.md$)");

double to_decay_lambda(int half_life_days) {
    if (half_life_days <= 0 || !std::isfinite(static_cast<double>(half_life_days))) {
        return 0.0;
    }
    return M_LN2 / half_life_days;
}

double calculate_temporal_decay_multiplier(int age_in_days, int half_life_days) {
    double lambda = to_decay_lambda(half_life_days);
    double clamped_age = std::max(0, age_in_days);
    
    if (lambda <= 0.0 || !std::isfinite(clamped_age)) {
        return 1.0;
    }
    
    return std::exp(-lambda * clamped_age);
}

double apply_temporal_decay_to_score(double score, int age_in_days, int half_life_days) {
    return score * calculate_temporal_decay_multiplier(age_in_days, half_life_days);
}

std::chrono::system_clock::time_point parse_memory_date_from_path(const std::string& file_path) {
    // Normalize path separators
    std::string normalized = file_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    
    // Remove leading "./"
    if (normalized.size() >= 2 && normalized[0] == '.' && normalized[1] == '/') {
        normalized = normalized.substr(2);
    }
    
    // Match pattern like "memory/2024-03-15.md"
    std::smatch match;
    if (!std::regex_search(normalized, match, DATED_MEMORY_PATH_RE)) {
        return std::chrono::system_clock::time_point();
    }
    
    try {
        int year = std::stoi(match[1].str());
        int month = std::stoi(match[2].str());
        int day = std::stoi(match[3].str());
        
        // Basic validation
        if (year < 1900 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
            return std::chrono::system_clock::time_point();
        }
        
        // Create time_t for the date (UTC midnight)
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        
        time_t time = timegm(&tm);  // UTC conversion
        if (time == -1) {
            return std::chrono::system_clock::time_point();
        }
        
        return std::chrono::system_clock::from_time_t(time);
    } catch (...) {
        return std::chrono::system_clock::time_point();
    }
}

bool is_evergreen_memory_path(const std::string& file_path) {
    // Normalize path
    std::string normalized = file_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    
    // Remove leading "./"
    if (normalized.size() >= 2 && normalized[0] == '.' && normalized[1] == '/') {
        normalized = normalized.substr(2);
    }
    
    // Check for root memory files
    if (normalized == "MEMORY.md" || normalized == "memory.md") {
        return true;
    }
    
    // Check if in memory/ directory but not a dated file
    if (normalized.rfind("memory/", 0) != 0) {
        return false;
    }
    
    // If it's a dated file, it's not evergreen
    std::smatch match;
    return !std::regex_search(normalized, match, DATED_MEMORY_PATH_RE);
}

std::chrono::system_clock::time_point extract_timestamp(
    const std::string& file_path,
    const std::string& source,
    const std::string& workspace_dir
) {
    // Try parsing date from path first
    auto from_path = parse_memory_date_from_path(file_path);
    if (from_path != std::chrono::system_clock::time_point()) {
        return from_path;
    }
    
    // Memory root/topic files are evergreen
    if (source == "memory" && is_evergreen_memory_path(file_path)) {
        return std::chrono::system_clock::time_point();
    }
    
    // If no workspace dir, can't check mtime
    if (workspace_dir.empty()) {
        return std::chrono::system_clock::time_point();
    }
    
    // Build absolute path
    std::string absolute_path = file_path;
    if (file_path[0] != '/') {
        absolute_path = workspace_dir + "/" + file_path;
    }
    
    // Get file mtime
    struct stat st;
    if (stat(absolute_path.c_str(), &st) != 0) {
        return std::chrono::system_clock::time_point();
    }
    
    return std::chrono::system_clock::from_time_t(st.st_mtime);
}

double age_in_days_from_timestamp(
    const std::chrono::system_clock::time_point& timestamp,
    const std::chrono::system_clock::time_point& now
) {
    if (timestamp == std::chrono::system_clock::time_point()) {
        return 0.0;
    }
    
    auto age_duration = now - timestamp;
    auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(age_duration).count();
    age_ms = std::max<int64_t>(0, age_ms);
    
    return static_cast<double>(age_ms) / DAY_MS;
}

// Note: Template implementation would typically be in header,
// but for demonstration we'll provide explicit instantiation if needed

} // namespace memory
} // namespace hunterclaw
