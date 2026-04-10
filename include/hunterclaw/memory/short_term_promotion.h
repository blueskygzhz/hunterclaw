#pragma once

// ============================================================================
// Short-Term Promotion — 严格对齐 OpenClaw short-term-promotion.ts (1879 行)
//
// 短期记忆晋升系统。管理 recall store（JSON 文件锁机制），
// 计算多维度加权评分（frequency/relevance/diversity/recency/consolidation/conceptual），
// 以及将候选记忆提升到 MEMORY.md。
//
// 核心功能:
//   - recordShortTermRecalls: 记录搜索召回信号
//   - recordGroundedShortTermCandidates: 记录 grounded 候选
//   - recordDreamingPhaseSignals: 记录梦境阶段信号
//   - rankShortTermPromotionCandidates: 排名提升候选
//   - applyShortTermPromotions: 应用提升到 MEMORY.md
//   - auditShortTermPromotionArtifacts: 审计工件
//   - repairShortTermPromotionArtifacts: 修复工件
// ============================================================================

#include "hunterclaw/memory/types.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Constants (对齐 short-term-promotion.ts:20-37)
// ============================================================================
constexpr double DEFAULT_PROMOTION_MIN_SCORE = 0.75;
constexpr int DEFAULT_PROMOTION_MIN_RECALL_COUNT = 3;
constexpr int DEFAULT_PROMOTION_MIN_UNIQUE_QUERIES = 2;
constexpr int DEFAULT_RECENCY_HALF_LIFE_DAYS = 14;
constexpr int MAX_QUERY_HASHES = 32;
constexpr int MAX_RECALL_DAYS = 16;

// ============================================================================
// Types (对齐 short-term-promotion.ts:40-207)
// ============================================================================

struct PromotionWeights {
    double frequency = 0.24;
    double relevance = 0.30;
    double diversity = 0.15;
    double recency = 0.15;
    double consolidation = 0.10;
    double conceptual = 0.06;
};

extern const PromotionWeights DEFAULT_PROMOTION_WEIGHTS;

struct ShortTermRecallEntry {
    std::string key;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    std::string source;  // "memory"
    std::string snippet;
    int recall_count = 0;
    int daily_count = 0;
    int grounded_count = 0;
    double total_score = 0.0;
    double max_score = 0.0;
    std::string first_recalled_at;
    std::string last_recalled_at;
    std::vector<std::string> query_hashes;
    std::vector<std::string> recall_days;
    std::vector<std::string> concept_tags;
    std::optional<std::string> claim_hash;
    std::optional<std::string> promoted_at;
};

struct PromotionComponents {
    double frequency = 0.0;
    double relevance = 0.0;
    double diversity = 0.0;
    double recency = 0.0;
    double consolidation = 0.0;
    double conceptual = 0.0;
};

struct PromotionCandidate {
    std::string key;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    std::string source;  // "memory"
    std::string snippet;
    int recall_count = 0;
    int daily_count = 0;
    int grounded_count = 0;
    int signal_count = 0;
    double avg_score = 0.0;
    double max_score = 0.0;
    int unique_queries = 0;
    std::optional<std::string> claim_hash;
    std::optional<std::string> promoted_at;
    std::string first_recalled_at;
    std::string last_recalled_at;
    double age_days = 0.0;
    double score = 0.0;
    std::vector<std::string> recall_days;
    std::vector<std::string> concept_tags;
    PromotionComponents components;
};

// ============================================================================
// Rank Options (对齐 short-term-promotion.ts:176-187)
// ============================================================================
struct RankShortTermPromotionOptions {
    std::string workspace_dir;
    std::optional<int> limit;
    std::optional<double> min_score;
    std::optional<int> min_recall_count;
    std::optional<int> min_unique_queries;
    std::optional<int> max_age_days;
    bool include_promoted = false;
    std::optional<double> recency_half_life_days;
    std::optional<PromotionWeights> weights;
    int64_t now_ms = 0;  // 0 = use current time
};

// ============================================================================
// Apply Options (对齐 short-term-promotion.ts:189-199)
// ============================================================================
struct ApplyShortTermPromotionsOptions {
    std::string workspace_dir;
    std::vector<PromotionCandidate> candidates;
    std::optional<int> limit;
    std::optional<double> min_score;
    std::optional<int> min_recall_count;
    std::optional<int> min_unique_queries;
    std::optional<int> max_age_days;
    int64_t now_ms = 0;
    std::optional<std::string> timezone;
};

struct ApplyShortTermPromotionsResult {
    std::string memory_path;
    int applied = 0;
    int appended = 0;
    int reconciled_existing = 0;
    std::vector<PromotionCandidate> applied_candidates;
};

// ============================================================================
// Core API
// ============================================================================

/**
 * Record short-term recall signals from memory search results.
 * (对齐 short-term-promotion.ts:809-917)
 */
void record_short_term_recalls(
    const std::string& workspace_dir,
    const std::string& query,
    const std::vector<MemorySearchResult>& results,
    const std::string& signal_type = "recall",  // "recall" | "daily"
    bool dedupe_by_query_per_day = false,
    const std::string& day_bucket = "",
    int64_t now_ms = 0,
    const std::string& timezone = ""
);

/**
 * Record dreaming phase signals.
 * (对齐 short-term-promotion.ts:1042-1094)
 */
void record_dreaming_phase_signals(
    const std::string& workspace_dir,
    const std::string& phase,  // "light" | "rem"
    const std::vector<std::string>& keys,
    int64_t now_ms = 0
);

/**
 * Rank short-term promotion candidates.
 * (对齐 short-term-promotion.ts:1096-1232)
 */
std::vector<PromotionCandidate> rank_short_term_promotion_candidates(
    const RankShortTermPromotionOptions& options
);

/**
 * Apply short-term promotions to MEMORY.md.
 * (对齐 short-term-promotion.ts:1443-1575)
 */
ApplyShortTermPromotionsResult apply_short_term_promotions(
    const ApplyShortTermPromotionsOptions& options
);

/**
 * Read all short-term recall entries.
 * (对齐 short-term-promotion.ts:1234-1249)
 */
std::vector<ShortTermRecallEntry> read_short_term_recall_entries(
    const std::string& workspace_dir,
    int64_t now_ms = 0
);

/**
 * Check if a file path is a short-term memory path.
 * (对齐 short-term-promotion.ts:798-807)
 */
bool is_short_term_memory_path(const std::string& file_path);

/**
 * Repair short-term promotion artifacts.
 * (对齐 short-term-promotion.ts:1743-1823)
 */
struct RepairResult {
    bool changed = false;
    int removed_invalid_entries = 0;
    bool rewrote_store = false;
    bool removed_stale_lock = false;
};
RepairResult repair_short_term_promotion_artifacts(const std::string& workspace_dir);

// ============================================================================
// Path resolution helpers
// ============================================================================

std::string resolve_short_term_recall_store_path(const std::string& workspace_dir);
std::string resolve_short_term_phase_signal_store_path(const std::string& workspace_dir);
std::string resolve_short_term_recall_lock_path(const std::string& workspace_dir);

}  // namespace memory
}  // namespace hunterclaw
