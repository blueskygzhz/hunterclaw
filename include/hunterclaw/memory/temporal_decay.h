#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace hunterclaw {
namespace memory {

/**
 * Temporal decay configuration for memory search results.
 */
struct TemporalDecayConfig {
    bool enabled = false;
    int half_life_days = 30;  // Half-life for exponential decay
};

const TemporalDecayConfig DEFAULT_TEMPORAL_DECAY_CONFIG = {false, 30};

/**
 * Convert half-life days to decay lambda (for exponential decay formula)
 */
double to_decay_lambda(int half_life_days);

/**
 * Calculate temporal decay multiplier for a given age
 */
double calculate_temporal_decay_multiplier(int age_in_days, int half_life_days);

/**
 * Apply temporal decay to a score
 */
double apply_temporal_decay_to_score(double score, int age_in_days, int half_life_days);

/**
 * Parse memory date from path (e.g., "memory/2024-03-15.md" -> Date)
 * Returns epoch timestamp (0) if not a dated memory path
 */
std::chrono::system_clock::time_point parse_memory_date_from_path(const std::string& file_path);

/**
 * Check if a memory path is "evergreen" (not subject to decay)
 * E.g., "MEMORY.md", "memory.md", "memory/topics/**.md"
 */
bool is_evergreen_memory_path(const std::string& file_path);

/**
 * Extract timestamp from file (path date or mtime)
 * Returns time_point() if timestamp cannot be determined
 */
std::chrono::system_clock::time_point extract_timestamp(
    const std::string& file_path,
    const std::string& source = "",
    const std::string& workspace_dir = ""
);

/**
 * Calculate age in days from timestamp
 */
double age_in_days_from_timestamp(
    const std::chrono::system_clock::time_point& timestamp,
    const std::chrono::system_clock::time_point& now
);

/**
 * Result entry with temporal decay support
 */
template<typename T>
struct DecayableResult {
    T result;
    double original_score;
    double decayed_score;
};

/**
 * Apply temporal decay to hybrid search results
 * Template allows flexibility in result structure
 */
template<typename ResultType>
std::vector<ResultType> apply_temporal_decay_to_hybrid_results(
    const std::vector<ResultType>& results,
    const TemporalDecayConfig& config,
    const std::string& workspace_dir = "",
    const std::chrono::system_clock::time_point& now = std::chrono::system_clock::now()
);

} // namespace memory
} // namespace hunterclaw
