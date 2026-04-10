#pragma once

#include <optional>
#include <cstdint>

// 严格对齐 OpenClaw src/auto-reply/reply/memory-flush.ts (132行)
// Memory flush 门控逻辑

namespace hunterclaw {
namespace react {

// ============================================================================
// resolveMemoryFlushContextWindowTokens (对齐 memory-flush.ts:7-22)
// ============================================================================
int resolve_memory_flush_context_window_tokens(
    int agent_cfg_context_tokens = 0,
    int default_context_tokens = 128000
);

// ============================================================================
// shouldRunMemoryFlush (对齐 memory-flush.ts:60-96)
// ============================================================================
struct MemoryFlushGateParams {
    std::optional<int64_t> total_tokens;       // session entry totalTokens
    bool total_tokens_fresh = false;           // whether totalTokens is from current turn
    std::optional<int> token_count_override;   // fresh override
    int context_window_tokens = 128000;
    int reserve_tokens_floor = 20000;
    int soft_threshold_tokens = 4000;
    std::optional<int> compaction_count;
    std::optional<int> memory_flush_compaction_count;
};

bool should_run_memory_flush(const MemoryFlushGateParams& params);

// ============================================================================
// hasAlreadyFlushedForCurrentCompaction (对齐 memory-flush.ts:80-95)
// ============================================================================
bool has_already_flushed_for_current_compaction(
    std::optional<int> compaction_count,
    std::optional<int> memory_flush_compaction_count
);

// ============================================================================
// shouldRunPreflightCompaction (对齐 memory-flush.ts:97-132)
// ============================================================================
struct PreflightCompactionParams {
    std::optional<int64_t> total_tokens;
    bool total_tokens_fresh = false;
    std::optional<int> token_count_override;
    int context_window_tokens = 128000;
    int reserve_tokens_floor = 20000;
    int soft_threshold_tokens = 4000;
    int64_t force_flush_transcript_bytes = 2 * 1024 * 1024;
    int64_t transcript_bytes = 0;
};

bool should_run_preflight_compaction(const PreflightCompactionParams& params);

}  // namespace react
}  // namespace hunterclaw
