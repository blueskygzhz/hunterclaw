#pragma once

// ============================================================================
// Dreaming Narrative — 严格对齐 OpenClaw dreaming-narrative.ts (509 行)
//
// LLM 驱动的梦境日记叙事生成器。
// 通过 subagent 调用 LLM 生成诗意的日记条目，追加到 DREAMS.md。
//
// 核心功能:
//   - buildNarrativePrompt: 构建 narrative prompt
//   - extractNarrativeText: 从 messages 提取叙事文本
//   - appendNarrativeEntry: 追加日记条目到 DREAMS.md
//   - buildDiaryEntry: 格式化单条日记
//   - generateAndAppendDreamNarrative: 完整编排（subagent 调用）
//   - writeBackfillDiaryEntries: 批量回填日记
//   - removeBackfillDiaryEntries: 清理回填日记
// ============================================================================

#include "hunterclaw/memory/dreaming.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Constants (对齐 dreaming-narrative.ts:44-73)
// ============================================================================

extern const std::string NARRATIVE_SYSTEM_PROMPT;
constexpr int NARRATIVE_TIMEOUT_MS = 60000;
extern const std::string DIARY_START_MARKER;
extern const std::string DIARY_END_MARKER;
extern const std::string BACKFILL_ENTRY_MARKER;

// ============================================================================
// Subagent surface (对齐 dreaming-narrative.ts:7-24)
// ============================================================================

struct SubagentRunResult {
    std::string run_id;
};

struct SubagentWaitResult {
    std::string status;
    std::optional<std::string> error;
};

struct SubagentMessage {
    std::string role;
    std::string content;
};

struct SubagentSurface {
    std::function<SubagentRunResult(
        const std::string& idempotency_key,
        const std::string& session_key,
        const std::string& message,
        const std::optional<std::string>& extra_system_prompt,
        bool deliver
    )> run;
    std::function<SubagentWaitResult(
        const std::string& run_id,
        int timeout_ms
    )> wait_for_run;
    std::function<std::vector<SubagentMessage>(
        const std::string& session_key,
        int limit
    )> get_session_messages;
    std::function<void(const std::string& session_key)> delete_session;
};

// ============================================================================
// Core API
// ============================================================================

/**
 * Extract narrative text from assistant messages.
 * (对齐 dreaming-narrative.ts:105-138 extractNarrativeText)
 */
std::optional<std::string> extract_narrative_text(
    const std::vector<SubagentMessage>& messages
);

/**
 * Format narrative date for diary entries.
 * (对齐 dreaming-narrative.ts:142-153 formatNarrativeDate)
 */
std::string format_narrative_date(int64_t epoch_ms, const std::string& timezone = "");

/**
 * Build a diary entry string.
 * (对齐 dreaming-narrative.ts:377-379 buildDiaryEntry)
 */
std::string build_diary_entry(const std::string& narrative, const std::string& date_str);

/**
 * Ensure DREAMS.md has diary section markers.
 * (对齐 dreaming-narrative.ts:183-192 ensureDiarySection)
 */
std::string ensure_diary_section(const std::string& existing);

/**
 * Replace diary content between markers.
 * (对齐 dreaming-narrative.ts:194-205 replaceDiaryContent)
 */
std::string replace_diary_content(const std::string& existing, const std::string& diary_content);

/**
 * Build a backfill diary entry.
 * (对齐 dreaming-narrative.ts:306-319 buildBackfillDiaryEntry)
 */
struct BackfillDiaryEntryParams {
    std::string iso_day;
    std::vector<std::string> body_lines;
    std::optional<std::string> source_path;
    std::optional<std::string> timezone;
};

std::string build_backfill_diary_entry(const BackfillDiaryEntryParams& params);

/**
 * Write backfill diary entries to DREAMS.md.
 * (对齐 dreaming-narrative.ts:321-359 writeBackfillDiaryEntries)
 */
struct WriteBackfillResult {
    std::string dreams_path;
    int written = 0;
    int replaced = 0;
};

WriteBackfillResult write_backfill_diary_entries(
    const std::string& workspace_dir,
    const std::vector<BackfillDiaryEntryParams>& entries,
    const std::string& timezone = ""
);

/**
 * Remove all backfill diary entries from DREAMS.md.
 * (对齐 dreaming-narrative.ts:361-375 removeBackfillDiaryEntries)
 */
struct RemoveBackfillResult {
    std::string dreams_path;
    int removed = 0;
};

RemoveBackfillResult remove_backfill_diary_entries(const std::string& workspace_dir);

/**
 * Full orchestrator: generate + append dream narrative via subagent.
 * (对齐 dreaming-narrative.ts:434-509 generateAndAppendDreamNarrative)
 */
void generate_and_append_dream_narrative(
    const SubagentSurface& subagent,
    const std::string& workspace_dir,
    const NarrativePhaseData& data,
    const DreamingLogger& logger,
    int64_t now_ms = 0,
    const std::string& timezone = ""
);

}  // namespace memory
}  // namespace hunterclaw
