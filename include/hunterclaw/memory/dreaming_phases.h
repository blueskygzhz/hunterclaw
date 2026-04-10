#pragma once

// ============================================================================
// Dreaming Phases — 严格对齐 OpenClaw dreaming-phases.ts (1701 行)
//
// Light/REM 阶段的核心执行引擎，包含：
//   - Daily memory ingestion (对齐 lines 963-1114)
//   - Session transcript ingestion (对齐 lines 504-955)
//   - Light dreaming (对齐 lines 1433-1511)
//   - REM dreaming (对齐 lines 1513-1593)
//   - Sweep phases orchestrator (对齐 lines 1595-1632)
//   - Daily snippet chunking (对齐 lines 186-269)
//   - Managed block stripping (对齐 lines 294-330)
// ============================================================================

#include "hunterclaw/memory/dreaming.h"
#include "hunterclaw/memory/short_term_promotion.h"
#include "hunterclaw/memory/types.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <cstdint>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Constants (对齐 dreaming-phases.ts:54-76)
// ============================================================================
constexpr double DAILY_INGESTION_SCORE = 0.62;
constexpr int DAILY_INGESTION_MAX_SNIPPET_CHARS = 280;
constexpr int DAILY_INGESTION_MIN_SNIPPET_CHARS = 8;
constexpr int DAILY_INGESTION_MAX_CHUNK_LINES = 4;
constexpr double SESSION_INGESTION_SCORE = 0.58;
constexpr int SESSION_INGESTION_MAX_SNIPPET_CHARS = 280;
constexpr int SESSION_INGESTION_MIN_SNIPPET_CHARS = 12;
constexpr int SESSION_INGESTION_MAX_MESSAGES_PER_SWEEP = 240;
constexpr int SESSION_INGESTION_MAX_MESSAGES_PER_FILE = 80;
constexpr int SESSION_INGESTION_MIN_MESSAGES_PER_FILE = 12;
constexpr int SESSION_INGESTION_MAX_TRACKED_MESSAGES_PER_SESSION = 4096;
constexpr int SESSION_INGESTION_MAX_TRACKED_SCOPES = 2048;

// ============================================================================
// Daily Snippet Chunk (对齐 dreaming-phases.ts:169-173)
// ============================================================================
struct DailySnippetChunk {
    int start_line = 0;
    int end_line = 0;
    std::string snippet;
};

// ============================================================================
// Daily Ingestion State (对齐 dreaming-phases.ts:346-354)
// ============================================================================
struct DailyIngestionFileState {
    int64_t mtime_ms = 0;
    int64_t size = 0;
};

struct DailyIngestionState {
    int version = 1;
    std::map<std::string, DailyIngestionFileState> files;
};

// ============================================================================
// Session Ingestion State (对齐 dreaming-phases.ts:416-428)
// ============================================================================
struct SessionIngestionFileState {
    int64_t mtime_ms = 0;
    int64_t size = 0;
    std::string content_hash;
    int line_count = 0;
    int last_content_line = 0;
};

struct SessionIngestionState {
    int version = 3;
    std::map<std::string, SessionIngestionFileState> files;
    std::map<std::string, std::vector<std::string>> seen_messages;
};

// ============================================================================
// Daily Ingestion Batch (对齐 dreaming-phases.ts:341-344)
// ============================================================================
struct DailyIngestionBatch {
    std::string day;
    std::vector<MemorySearchResult> results;
};

// ============================================================================
// Core API
// ============================================================================

/**
 * Build daily snippet chunks from memory file lines.
 * (对齐 dreaming-phases.ts:186-269 buildDailySnippetChunks)
 */
std::vector<DailySnippetChunk> build_daily_snippet_chunks(
    const std::vector<std::string>& lines,
    int limit
);

/**
 * Strip managed daily dreaming blocks (Light/REM) from lines.
 * (对齐 dreaming-phases.ts:294-330 stripManagedDailyDreamingLines)
 */
std::vector<std::string> strip_managed_daily_dreaming_lines(
    const std::vector<std::string>& lines
);

/**
 * Ingest daily memory signals into short-term recall store.
 * (对齐 dreaming-phases.ts:1084-1114 ingestDailyMemorySignals)
 */
void ingest_daily_memory_signals(
    const std::string& workspace_dir,
    int lookback_days,
    int limit,
    int64_t now_ms = 0,
    const std::string& timezone = ""
);

/**
 * Seed historical daily memory signals from explicit file paths.
 * (对齐 dreaming-phases.ts:1116-1221 seedHistoricalDailyMemorySignals)
 */
struct SeedHistoricalResult {
    int imported_file_count = 0;
    int imported_signal_count = 0;
    std::vector<std::string> skipped_paths;
};

SeedHistoricalResult seed_historical_daily_memory_signals(
    const std::string& workspace_dir,
    const std::vector<std::string>& file_paths,
    int limit,
    int64_t now_ms = 0,
    const std::string& timezone = ""
);

/**
 * Run light dreaming phase.
 * (对齐 dreaming-phases.ts:1433-1511 runLightDreaming)
 */
void run_light_dreaming(
    const std::string& workspace_dir,
    const MemoryLightDreamingConfig& config,
    const DreamingLogger& logger,
    int64_t now_ms = 0
);

/**
 * Run REM dreaming phase.
 * (对齐 dreaming-phases.ts:1513-1593 runRemDreaming)
 */
void run_rem_dreaming(
    const std::string& workspace_dir,
    const MemoryRemDreamingConfig& config,
    const DreamingLogger& logger,
    int64_t now_ms = 0
);

/**
 * Check if a heading is a generic daily heading (date, day-of-week, etc.)
 * (对齐 dreaming-phases.ts:142-155 isGenericDailyHeading)
 */
bool is_generic_daily_heading(const std::string& heading);

// ============================================================================
// I/O helpers
// ============================================================================

std::string resolve_daily_ingestion_state_path(const std::string& workspace_dir);
DailyIngestionState read_daily_ingestion_state(const std::string& workspace_dir);
void write_daily_ingestion_state(const std::string& workspace_dir, const DailyIngestionState& state);

}  // namespace memory
}  // namespace hunterclaw
