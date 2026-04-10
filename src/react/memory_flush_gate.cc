#include "hunterclaw/react/memory_flush_gate.h"
#include <algorithm>
#include <cmath>

namespace hunterclaw {
namespace react {

// ============================================================================
// resolveMemoryFlushContextWindowTokens (对齐 memory-flush.ts:7-22)
// ============================================================================

int resolve_memory_flush_context_window_tokens(
    int agent_cfg_context_tokens,
    int default_context_tokens
) {
    // 对齐: resolveContextTokensForModel({ contextTokensOverride }) ?? DEFAULT_CONTEXT_TOKENS
    if (agent_cfg_context_tokens > 0) {
        return agent_cfg_context_tokens;
    }
    return default_context_tokens;
}

// ============================================================================
// hasAlreadyFlushedForCurrentCompaction (对齐 memory-flush.ts:80-95)
// ============================================================================

bool has_already_flushed_for_current_compaction(
    std::optional<int> compaction_count,
    std::optional<int> memory_flush_compaction_count
) {
    // 对齐: entry.compactionCount === entry.memoryFlushCompactionCount
    if (!compaction_count.has_value() || !memory_flush_compaction_count.has_value()) {
        return false;
    }
    return *compaction_count == *memory_flush_compaction_count;
}

// ============================================================================
// shouldRunMemoryFlush (对齐 memory-flush.ts:60-96)
// ============================================================================

bool should_run_memory_flush(const MemoryFlushGateParams& params) {
    // 对齐: resolveMemoryFlushGateState
    int64_t total_tokens = 0;

    if (params.token_count_override.has_value() && *params.token_count_override > 0) {
        total_tokens = *params.token_count_override;
    } else if (params.total_tokens.has_value() && params.total_tokens_fresh && *params.total_tokens > 0) {
        total_tokens = *params.total_tokens;
    } else {
        return false;
    }

    int context_window = std::max(1, params.context_window_tokens);
    int reserve = std::max(0, params.reserve_tokens_floor);
    int soft = std::max(0, params.soft_threshold_tokens);
    int threshold = std::max(0, context_window - reserve - soft);

    if (threshold <= 0) {
        return false;
    }

    if (total_tokens < threshold) {
        return false;
    }

    // 对齐: hasAlreadyFlushedForCurrentCompaction
    if (has_already_flushed_for_current_compaction(
            params.compaction_count, params.memory_flush_compaction_count)) {
        return false;
    }

    return true;
}

// ============================================================================
// shouldRunPreflightCompaction (对齐 memory-flush.ts:97-132)
// ============================================================================

bool should_run_preflight_compaction(const PreflightCompactionParams& params) {
    // 对齐: if (forceFlushTranscriptBytes > 0 && transcriptBytes >= forceFlushTranscriptBytes) return true
    if (params.force_flush_transcript_bytes > 0 &&
        params.transcript_bytes >= params.force_flush_transcript_bytes) {
        return true;
    }

    // 对齐: token-based gate
    int64_t total_tokens = 0;
    if (params.token_count_override.has_value() && *params.token_count_override > 0) {
        total_tokens = *params.token_count_override;
    } else if (params.total_tokens.has_value() && params.total_tokens_fresh && *params.total_tokens > 0) {
        total_tokens = *params.total_tokens;
    } else {
        return false;
    }

    int context_window = std::max(1, params.context_window_tokens);
    int reserve = std::max(0, params.reserve_tokens_floor);
    int soft = std::max(0, params.soft_threshold_tokens);
    int threshold = std::max(0, context_window - reserve - soft);

    return threshold > 0 && total_tokens >= threshold;
}

}  // namespace react
}  // namespace hunterclaw
