#pragma once

// ============================================================================
// Dreaming System — 严格对齐 OpenClaw dreaming*.ts (~3000+ 行)
//
// 梦境记忆巩固系统。包含:
//   - dreaming.ts: 统一的 dreaming 控制器
//   - dreaming-phases.ts: Light/REM 两个阶段
//   - dreaming-narrative.ts: 梦境日记生成器
//   - dreaming-markdown.ts: 结果写入 markdown
//
// 核心流程:
//   heartbeat 触发 → sweep phases → rank → promote → 生成报告 → 叙事日记
// ============================================================================

#include "hunterclaw/memory/short_term_promotion.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Dreaming Config (对齐 dreaming.ts:95-110)
// ============================================================================
struct ShortTermPromotionDreamingConfig {
    bool enabled = true;
    std::string cron = "0 2 * * *";  // default: 2 AM daily
    std::optional<std::string> timezone;
    int limit = 5;
    double min_score = DEFAULT_PROMOTION_MIN_SCORE;
    int min_recall_count = DEFAULT_PROMOTION_MIN_RECALL_COUNT;
    int min_unique_queries = DEFAULT_PROMOTION_MIN_UNIQUE_QUERIES;
    std::optional<double> recency_half_life_days;
    std::optional<int> max_age_days;
    bool verbose_logging = false;
    struct Storage {
        std::string mode = "inline";  // "inline" | "separate" | "both"
        bool separate_reports = false;
    } storage;
};

// ============================================================================
// Light Dreaming Config (对齐 dreaming-phases.ts)
// ============================================================================
struct MemoryLightDreamingConfig {
    bool enabled = true;
    int lookback_days = 7;
    int limit = 10;
    double dedupe_similarity = 0.88;
    std::optional<std::string> timezone;
    struct Storage {
        std::string mode = "inline";
        bool separate_reports = false;
    } storage;
};

// ============================================================================
// REM Dreaming Config (对齐 dreaming-phases.ts)
// ============================================================================
struct MemoryRemDreamingConfig {
    bool enabled = true;
    int lookback_days = 14;
    int limit = 5;
    double min_pattern_strength = 0.15;
    std::optional<std::string> timezone;
    struct Storage {
        std::string mode = "inline";
        bool separate_reports = false;
    } storage;
};

// ============================================================================
// REM Dreaming Preview (对齐 dreaming-phases.ts:1308-1314)
// ============================================================================
struct RemDreamingPreview {
    int source_entry_count = 0;
    std::vector<std::string> reflections;
    struct CandidateTruth {
        std::string snippet;
        double confidence = 0.0;
        std::string evidence;
    };
    std::vector<CandidateTruth> candidate_truths;
    std::vector<std::string> candidate_keys;
    std::vector<std::string> body_lines;
};

// ============================================================================
// Narrative Phase Data (对齐 dreaming-narrative.ts:26-34)
// ============================================================================
struct NarrativePhaseData {
    std::string phase;  // "light" | "deep" | "rem"
    std::vector<std::string> snippets;
    std::vector<std::string> themes;
    std::vector<std::string> promotions;
};

// ============================================================================
// Logger interface
// ============================================================================
struct DreamingLogger {
    std::function<void(const std::string&)> info;
    std::function<void(const std::string&)> warn;
    std::function<void(const std::string&)> error;
};

// ============================================================================
// Core Dreaming API
// ============================================================================

/**
 * Run dreaming sweep phases (Light + REM).
 * (对齐 dreaming-phases.ts:1595-1632 runDreamingSweepPhases)
 */
void run_dreaming_sweep_phases(
    const std::string& workspace_dir,
    const DreamingLogger& logger,
    int64_t now_ms = 0
);

/**
 * Run the full short-term dreaming promotion cycle.
 * (对齐 dreaming.ts:452-617 runShortTermDreamingPromotionIfTriggered)
 *
 * Flow:
 * 1. Run sweep phases (light + REM)
 * 2. Repair recall artifacts
 * 3. Rank candidates
 * 4. Apply promotions to MEMORY.md
 * 5. Write dreaming report
 */
struct DreamingPromotionResult {
    int total_candidates = 0;
    int total_applied = 0;
    int failed_workspaces = 0;
};

DreamingPromotionResult run_short_term_dreaming_promotion(
    const std::vector<std::string>& workspace_dirs,
    const ShortTermPromotionDreamingConfig& config,
    const DreamingLogger& logger
);

/**
 * Preview REM dreaming results without applying.
 * (对齐 dreaming-phases.ts:1396-1431 previewRemDreaming)
 */
RemDreamingPreview preview_rem_dreaming(
    const std::vector<ShortTermRecallEntry>& entries,
    int limit,
    double min_pattern_strength
);

// ============================================================================
// Dreaming Markdown API (对齐 dreaming-markdown.ts)
// ============================================================================

/**
 * Write a daily dreaming phase block (Light Sleep / REM Sleep).
 * (对齐 dreaming-markdown.ts:58-122 writeDailyDreamingPhaseBlock)
 */
struct DreamingPhaseBlockResult {
    std::optional<std::string> inline_path;
    std::optional<std::string> report_path;
};

DreamingPhaseBlockResult write_daily_dreaming_phase_block(
    const std::string& workspace_dir,
    const std::string& phase,  // "light" | "rem"
    const std::vector<std::string>& body_lines,
    int64_t now_ms = 0,
    const std::optional<std::string>& timezone = std::nullopt,
    const std::string& storage_mode = "inline",
    bool separate_reports = false
);

/**
 * Write a deep dreaming report.
 * (对齐 dreaming-markdown.ts:124-149 writeDeepDreamingReport)
 */
std::optional<std::string> write_deep_dreaming_report(
    const std::string& workspace_dir,
    const std::vector<std::string>& body_lines,
    int64_t now_ms = 0,
    const std::optional<std::string>& timezone = std::nullopt,
    const std::string& storage_mode = "inline",
    bool separate_reports = false
);

// ============================================================================
// Narrative API (对齐 dreaming-narrative.ts)
// ============================================================================

/**
 * Build narrative prompt from phase data.
 * (对齐 dreaming-narrative.ts:78-101 buildNarrativePrompt)
 */
std::string build_narrative_prompt(const NarrativePhaseData& data);

/**
 * Append a narrative diary entry to DREAMS.md.
 * (对齐 dreaming-narrative.ts:381-430 appendNarrativeEntry)
 */
std::string append_narrative_entry(
    const std::string& workspace_dir,
    const std::string& narrative,
    int64_t now_ms = 0,
    const std::optional<std::string>& timezone = std::nullopt
);

// ============================================================================
// Flush Plan (对齐 flush-plan.ts:95-139)
// ============================================================================
struct MemoryFlushPlan {
    int soft_threshold_tokens = 4000;
    int64_t force_flush_transcript_bytes = 2 * 1024 * 1024;
    int reserve_tokens_floor = 20000;
    std::string prompt;
    std::string system_prompt;
    std::string relative_path;
};

// 对齐 flush-plan.ts:95 FlushPlan config params
struct MemoryFlushConfig {
    bool enabled = true;
    std::optional<int> soft_threshold_tokens;
    std::optional<int64_t> force_flush_transcript_bytes;
    std::optional<int> reserve_tokens_floor;
    std::optional<std::string> prompt;
    std::optional<std::string> system_prompt;
    std::optional<std::string> timezone;
};

/**
 * Build a memory flush plan for pre-compaction memory persistence.
 * (对齐 flush-plan.ts:95-139 buildMemoryFlushPlan)
 */
std::optional<MemoryFlushPlan> build_memory_flush_plan(
    int64_t now_ms = 0,
    const std::optional<MemoryFlushConfig>& config = std::nullopt
);

// ============================================================================
// Date helpers
// ============================================================================

/**
 * Format epoch ms to "YYYY-MM-DD" day string.
 * (对齐 memory-core-host-status.ts:formatMemoryDreamingDay)
 */
std::string format_memory_dreaming_day(int64_t epoch_ms, const std::string& timezone = "");

}  // namespace memory
}  // namespace hunterclaw
