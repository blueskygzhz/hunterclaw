#pragma once

// 严格对齐 OpenClaw src/memory/temporal-decay.ts

#include <string>
#include <vector>
#include <chrono>
#include <optional>

namespace hunterclaw {
namespace memory {

// ============================================================================
// TemporalDecayConfig (对齐 temporal-decay.ts:TemporalDecayConfig)
// ============================================================================
struct TemporalDecayConfig {
    bool enabled = false;
    double half_life_days = 30.0;  // 对齐 openclaw: halfLifeDays (number)
};

const TemporalDecayConfig DEFAULT_TEMPORAL_DECAY_CONFIG = {false, 30.0};

// ============================================================================
// 核心计算函数 (对齐 temporal-decay.ts)
// ============================================================================

/**
 * Convert half-life days to decay lambda (对齐 toDecayLambda)
 * lambda = ln(2) / halfLifeDays
 */
double to_decay_lambda(double half_life_days);

/**
 * Calculate temporal decay multiplier (对齐 calculateTemporalDecayMultiplier)
 * multiplier = exp(-lambda * ageInDays)
 */
double calculate_temporal_decay_multiplier(double age_in_days, double half_life_days);

/**
 * Apply temporal decay to a score (对齐 applyTemporalDecayToScore)
 */
double apply_temporal_decay_to_score(double score, double age_in_days, double half_life_days);

// ============================================================================
// Path/timestamp helpers (对齐 temporal-decay.ts)
// ============================================================================

/**
 * Parse memory date from path (对齐 parseMemoryDateFromPath)
 * e.g., "memory/2024-03-15.md" -> Date
 * Returns time_point() (epoch) if not a dated memory path
 */
std::chrono::system_clock::time_point parse_memory_date_from_path(const std::string& file_path);

/**
 * Check if a memory path is "evergreen" (对齐 isEvergreenMemoryPath)
 * e.g., "MEMORY.md", "memory.md", "memory/topics/*.md"
 */
bool is_evergreen_memory_path(const std::string& file_path);

/**
 * Extract timestamp from file (对齐 extractTimestamp)
 * Returns time_point() if timestamp cannot be determined (evergreen or no mtime)
 */
std::chrono::system_clock::time_point extract_timestamp(
    const std::string& file_path,
    const std::string& source = "",
    const std::string& workspace_dir = ""
);

/**
 * Calculate age in days from timestamp (对齐 ageInDaysFromTimestamp)
 */
double age_in_days_from_timestamp(
    const std::chrono::system_clock::time_point& timestamp,
    int64_t now_ms
);

// ============================================================================
// apply_temporal_decay_to_hybrid_results (对齐 applyTemporalDecayToHybridResults)
// ============================================================================

/**
 * Result entry type for temporal decay (对齐 openclaw: T extends { path, score, source })
 */
struct TemporalDecayEntry {
    std::string path;
    double score;
    std::string source;
};

/**
 * Apply temporal decay to hybrid search results (对齐 applyTemporalDecayToHybridResults)
 * Modifies scores in-place and returns sorted results.
 */
std::vector<TemporalDecayEntry> apply_temporal_decay_to_hybrid_results(
    std::vector<TemporalDecayEntry> results,
    const TemporalDecayConfig& config,
    const std::string& workspace_dir = "",
    int64_t now_ms = 0  // 0 = use current time
);

} // namespace memory
} // namespace hunterclaw
