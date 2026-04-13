// Aligned with openclaw: src/auto-reply/reply/memory-flush.ts (132行)
// Memory flush gating — decides when to flush session context to long-term memory
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Memory flush context window resolution
// 对齐 OC: resolveMemoryFlushContextWindowTokens (memory-flush.ts:7-22)
// ============================================================================

struct MemoryFlushContextParams {
    std::optional<std::string> model_id;
    std::optional<int> agent_cfg_context_tokens;
    std::string config_json;
    std::optional<std::string> provider;
};

/// Resolve the context window size for memory flush gating.
/// Falls back to DEFAULT_CONTEXT_TOKENS (128000) when model-specific value is unavailable.
int resolve_memory_flush_context_window_tokens(const MemoryFlushContextParams& params);

// ============================================================================
// shouldRunMemoryFlush (对齐 OC: shouldRunMemoryFlush, memory-flush.ts:60-85)
// ============================================================================

struct MemoryFlushGateParams {
    std::optional<int> total_tokens;         // session entry total tokens
    std::optional<int> total_tokens_fresh;   // fresh total tokens
    std::optional<int> token_count;          // override token count
    int context_window_tokens;
    int reserve_tokens_floor;
    int soft_threshold_tokens;
    std::optional<int> compaction_count;
    std::optional<int> memory_flush_compaction_count;
};

/// Determine if a memory flush should run based on token usage thresholds.
/// Returns true when total tokens exceed (contextWindow - reserve - softThreshold)
/// and no flush has been performed for the current compaction cycle.
/// 对齐 OC: shouldRunMemoryFlush (memory-flush.ts:60-85)
bool should_run_memory_flush(const MemoryFlushGateParams& params);

// ============================================================================
// shouldRunPreflightCompaction (对齐 OC: shouldRunPreflightCompaction)
// ============================================================================

/// Determine if a pre-run compaction should be triggered.
/// 对齐 OC: shouldRunPreflightCompaction (memory-flush.ts:87-101)
bool should_run_preflight_compaction(const MemoryFlushGateParams& params);

// ============================================================================
// hasAlreadyFlushedForCurrentCompaction
// 对齐 OC: hasAlreadyFlushedForCurrentCompaction (memory-flush.ts:108-114)
// ============================================================================

/// Returns true when a memory flush has already been performed for the
/// current compaction cycle. Prevents repeated flush runs.
bool has_already_flushed_for_current_compaction(
    int compaction_count,
    std::optional<int> memory_flush_compaction_count);

// ============================================================================
// computeContextHash (对齐 OC: computeContextHash, memory-flush.ts:125-131)
// ============================================================================

struct SimpleMessageForHash {
    std::string role;
    std::string content;
};

/// Compute a lightweight content hash from the tail of a session transcript.
/// Used for state-based flush deduplication.
/// Hash input: messages.length + content of last 3 user/assistant messages.
/// Algorithm: SHA-256 truncated to 16 hex chars.
std::string compute_context_hash(const std::vector<SimpleMessageForHash>& messages);

}  // namespace auto_reply
}  // namespace hunterclaw
