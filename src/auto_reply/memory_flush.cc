// Aligned with openclaw: src/auto-reply/reply/memory-flush.ts (132行)
// Memory flush gating: token-based threshold + compaction cycle dedup

#include "hunterclaw/auto_reply/memory_flush.h"
#include "hunterclaw/config/config_system.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>
#include <string>

// For SHA-256
#include <openssl/sha.h>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Constants (对齐 OC: DEFAULT_CONTEXT_TOKENS = 128000)
// ============================================================================

static constexpr int DEFAULT_CONTEXT_TOKENS = 128000;

// ============================================================================
// resolve_memory_flush_context_window_tokens
// 对齐 OC: resolveMemoryFlushContextWindowTokens (memory-flush.ts:7-22)
// ============================================================================

int resolve_memory_flush_context_window_tokens(const MemoryFlushContextParams& params) {
    // Try to resolve from model-specific config
    if (params.agent_cfg_context_tokens && *params.agent_cfg_context_tokens > 0) {
        return *params.agent_cfg_context_tokens;
    }
    // Fall back to default
    return DEFAULT_CONTEXT_TOKENS;
}

// ============================================================================
// Internal: resolve gate state
// 对齐 OC: resolveMemoryFlushGateState (memory-flush.ts:30-58)
// ============================================================================

struct MemoryFlushGateState {
    int total_tokens;
    int threshold;
    bool valid;
};

static MemoryFlushGateState resolve_memory_flush_gate_state(
    const MemoryFlushGateParams& params) {
    // Resolve total tokens
    int total_tokens = 0;
    if (params.token_count && *params.token_count > 0) {
        total_tokens = *params.token_count;
    } else if (params.total_tokens_fresh && *params.total_tokens_fresh > 0) {
        total_tokens = *params.total_tokens_fresh;
    } else if (params.total_tokens && *params.total_tokens > 0) {
        total_tokens = *params.total_tokens;
    }

    if (total_tokens <= 0) {
        return {0, 0, false};
    }

    int context_window = std::max(1, params.context_window_tokens);
    int reserve_tokens = std::max(0, params.reserve_tokens_floor);
    int soft_threshold = std::max(0, params.soft_threshold_tokens);
    int threshold = std::max(0, context_window - reserve_tokens - soft_threshold);

    if (threshold <= 0) {
        return {0, 0, false};
    }

    return {total_tokens, threshold, true};
}

// ============================================================================
// has_already_flushed_for_current_compaction
// 对齐 OC: hasAlreadyFlushedForCurrentCompaction (memory-flush.ts:108-114)
// ============================================================================

bool has_already_flushed_for_current_compaction(
    int compaction_count,
    std::optional<int> memory_flush_compaction_count) {
    if (!memory_flush_compaction_count) return false;
    return *memory_flush_compaction_count == compaction_count;
}

// ============================================================================
// should_run_memory_flush
// 对齐 OC: shouldRunMemoryFlush (memory-flush.ts:60-85)
// ============================================================================

bool should_run_memory_flush(const MemoryFlushGateParams& params) {
    auto state = resolve_memory_flush_gate_state(params);
    if (!state.valid || state.total_tokens < state.threshold) {
        return false;
    }

    // Check if already flushed for this compaction cycle
    int compaction_count = params.compaction_count.value_or(0);
    if (has_already_flushed_for_current_compaction(
            compaction_count, params.memory_flush_compaction_count)) {
        return false;
    }

    return true;
}

// ============================================================================
// should_run_preflight_compaction
// 对齐 OC: shouldRunPreflightCompaction (memory-flush.ts:87-101)
// ============================================================================

bool should_run_preflight_compaction(const MemoryFlushGateParams& params) {
    auto state = resolve_memory_flush_gate_state(params);
    return state.valid && state.total_tokens >= state.threshold;
}

// ============================================================================
// compute_context_hash
// 对齐 OC: computeContextHash (memory-flush.ts:125-131)
// Hash input: messages.length + content of last 3 user/assistant messages
// Algorithm: SHA-256 truncated to 16 hex chars
// ============================================================================

std::string compute_context_hash(const std::vector<SimpleMessageForHash>& messages) {
    // Filter user/assistant messages
    std::vector<const SimpleMessageForHash*> user_assistant;
    for (const auto& m : messages) {
        if (m.role == "user" || m.role == "assistant") {
            user_assistant.push_back(&m);
        }
    }

    // Take last 3
    size_t start = user_assistant.size() > 3 ? user_assistant.size() - 3 : 0;
    std::vector<const SimpleMessageForHash*> tail(
        user_assistant.begin() + start, user_assistant.end());

    // Build payload: "messages.length:[0:role]content\0[1:role]content..."
    std::ostringstream ss;
    ss << messages.size() << ":";
    for (size_t i = 0; i < tail.size(); ++i) {
        ss << "[" << i << ":" << tail[i]->role << "]" << tail[i]->content;
        if (i + 1 < tail.size()) ss << '\0';
    }

    std::string payload = ss.str();

    // SHA-256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
           payload.size(), hash);

    // Hex encode, take first 16 chars
    std::ostringstream hex_ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH && hex_ss.str().size() < 16; ++i) {
        hex_ss << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(hash[i]);
    }
    return hex_ss.str().substr(0, 16);
}

}  // namespace auto_reply
}  // namespace hunterclaw
