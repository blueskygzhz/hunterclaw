#pragma once
// ============================================================================
// preflight_memory.h — 严格对齐 OpenClaw agent-runner-memory.ts (830行)
//
// Pre-run compaction + memory flush 完整执行流
// 门控逻辑在 memory_flush_gate.h 中，本文件是执行层
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>

namespace hunterclaw {
namespace react {

// ============================================================================
// 对齐 agent-runner-memory.ts:75-86: estimatePromptTokensForMemoryFlush
// ============================================================================
std::optional<int> estimate_prompt_tokens_for_memory_flush(const std::string& prompt);

// ============================================================================
// 对齐 agent-runner-memory.ts:88-99: resolveEffectivePromptTokens
// ============================================================================
int resolve_effective_prompt_tokens(
    int base_prompt_tokens = 0,
    int last_output_tokens = 0,
    int prompt_token_estimate = 0
);

// ============================================================================
// 对齐 agent-runner-memory.ts:101-104: SessionTranscriptUsageSnapshot
// ============================================================================
struct SessionTranscriptUsageSnapshot {
    std::optional<int> prompt_tokens;
    std::optional<int> output_tokens;
};

// ============================================================================
// 对齐 agent-runner-memory.ts:212-252: readSessionLogSnapshot
// ============================================================================
struct SessionLogSnapshot {
    std::optional<int64_t> byte_size;
    std::optional<SessionTranscriptUsageSnapshot> usage;
};

SessionLogSnapshot read_session_log_snapshot(
    const std::string& log_path,
    bool include_byte_size,
    bool include_usage
);

// ============================================================================
// 对齐 agent-runner-memory.ts:254-284: readLastNonzeroUsageFromSessionLog
// ============================================================================
std::optional<SessionTranscriptUsageSnapshot> read_last_nonzero_usage_from_session_log(
    const std::string& log_path
);

// ============================================================================
// 对齐 agent-runner-memory.ts:286-312: estimatePromptTokensFromSessionTranscript
// ============================================================================
std::optional<int> estimate_prompt_tokens_from_session_transcript(
    const std::string& session_id,
    const std::string& store_path = "",
    const std::string& session_file = ""
);

// ============================================================================
// 对齐 agent-runner-memory.ts:331-484: runPreflightCompactionIfNeeded
// ============================================================================

struct PreflightRunParams {
    std::string session_key;
    std::string session_id;
    std::string session_file;
    std::string store_path;
    std::string workspace_dir;
    std::string provider;
    std::string model;
    std::string default_model;
    std::optional<int> agent_cfg_context_tokens;
    bool is_heartbeat = false;
    bool is_cli = false;

    // Token state
    std::optional<int> persisted_total_tokens;
    bool total_tokens_fresh = false;
    std::optional<int> compaction_count;
    std::optional<std::string> prompt_for_estimate;

    // Context window config
    int context_window_tokens = 200000;
    int reserve_tokens_floor = 20000;
    int soft_threshold_tokens = 4000;

    // Callbacks
    std::function<bool(const std::string& session_id, int token_count)> compact_session;
    std::function<void(int new_compaction_count)> on_compaction_complete;
};

struct PreflightCompactionResult {
    bool compacted = false;
    std::optional<int> tokens_after;
    std::optional<std::string> new_session_id;
};

PreflightCompactionResult run_preflight_compaction_if_needed(
    const PreflightRunParams& params
);

// ============================================================================
// 对齐 agent-runner-memory.ts:486-829: runMemoryFlushIfNeeded
// ============================================================================

struct MemoryFlushParams {
    std::string session_key;
    std::string session_id;
    std::string session_file;
    std::string store_path;
    std::string workspace_dir;
    std::string provider;
    std::string model;
    std::string default_model;
    std::optional<int> agent_cfg_context_tokens;
    bool is_heartbeat = false;
    bool is_cli = false;
    bool memory_flush_writable = true;

    // Token state
    std::optional<int> persisted_total_tokens;
    bool total_tokens_fresh = false;
    std::optional<int> compaction_count;
    std::optional<int> memory_flush_compaction_count;
    std::optional<std::string> prompt_for_estimate;

    // Memory flush plan
    int reserve_tokens_floor = 20000;
    int soft_threshold_tokens = 4000;
    int context_window_tokens = 200000;
    int64_t force_flush_transcript_bytes = 0;
    std::string flush_write_path;
    std::string flush_system_prompt;
    std::string flush_prompt;

    // Callbacks
    std::function<void(const std::string& prompt, const std::string& system_prompt)> run_flush_agent;
    std::function<void(int new_compaction_count, const std::string& new_session_id)> on_flush_complete;
};

struct MemoryFlushResult {
    bool flushed = false;
    bool compaction_completed = false;
    std::optional<std::string> new_session_id;
};

MemoryFlushResult run_memory_flush_if_needed(const MemoryFlushParams& params);

// ============================================================================
// 对齐 agent-runner-memory.ts:190-210: appendPostCompactionRefreshPrompt
// ============================================================================
std::optional<std::string> read_post_compaction_context(
    const std::string& workspace_dir
);

} // namespace react
} // namespace hunterclaw
