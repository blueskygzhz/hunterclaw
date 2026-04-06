#pragma once
// Aligned with openclaw: src/auto-reply/reply/block-streaming.ts
//
// Block streaming configuration resolution:
//   - BlockStreamingChunking: min/max chars, break preference, flushOnParagraph
//   - BlockStreamingCoalescing: min/max chars, idle timer, joiner, flushOnEnqueue
//   - resolveEffectiveBlockStreamingConfig: merge chunking + coalescing from config

#include <string>
#include <optional>
#include <algorithm>
#include <cmath>

namespace hunterclaw {
namespace outbound {

// Aligned with openclaw: DEFAULT_BLOCK_STREAM_MIN/MAX/COALESCE_IDLE_MS
static constexpr int DEFAULT_BLOCK_STREAM_MIN = 800;
static constexpr int DEFAULT_BLOCK_STREAM_MAX = 1200;
static constexpr int DEFAULT_BLOCK_STREAM_COALESCE_IDLE_MS = 1000;

// Aligned with openclaw: BlockStreamingChunking
struct BlockStreamingChunking {
    int min_chars = DEFAULT_BLOCK_STREAM_MIN;
    int max_chars = DEFAULT_BLOCK_STREAM_MAX;
    std::string break_preference = "paragraph";  // "paragraph" | "newline" | "sentence"
    bool flush_on_paragraph = false;
};

// Aligned with openclaw: BlockStreamingCoalescing
struct BlockStreamingCoalescing {
    int min_chars = DEFAULT_BLOCK_STREAM_MIN;
    int max_chars = DEFAULT_BLOCK_STREAM_MAX;
    int idle_ms = DEFAULT_BLOCK_STREAM_COALESCE_IDLE_MS;
    std::string joiner = "\n\n";
    bool flush_on_enqueue = false;
};

// Aligned with openclaw: clampPositiveInteger
inline int clamp_positive_integer(int value, int fallback, int min_val, int max_val) {
    if (value <= 0) return fallback;
    int rounded = value;
    if (rounded < min_val) return min_val;
    if (rounded > max_val) return max_val;
    return rounded;
}

// Aligned with openclaw: resolveEffectiveBlockStreamingConfig
struct EffectiveBlockStreamingConfig {
    BlockStreamingChunking chunking;
    BlockStreamingCoalescing coalescing;
};

inline EffectiveBlockStreamingConfig resolve_effective_block_streaming_config(
    const std::optional<BlockStreamingChunking>& caller_chunking = std::nullopt,
    int text_limit = 4096,
    int max_chunk_chars_override = 0,
    int coalesce_idle_ms_override = -1
) {
    // Resolve chunking
    BlockStreamingChunking chunking;
    if (caller_chunking.has_value()) {
        chunking = *caller_chunking;
    } else {
        chunking.min_chars = DEFAULT_BLOCK_STREAM_MIN;
        chunking.max_chars = DEFAULT_BLOCK_STREAM_MAX;
        chunking.break_preference = "paragraph";
    }

    // Apply maxChunkChars override (ACP-style)
    if (max_chunk_chars_override > 0) {
        int clamped = clamp_positive_integer(max_chunk_chars_override, chunking.max_chars, 1, std::max(1, text_limit));
        chunking.max_chars = clamped;
        chunking.min_chars = std::min(chunking.min_chars, chunking.max_chars);
    }

    // Resolve coalescing
    BlockStreamingCoalescing coalescing;
    coalescing.max_chars = std::max(1, std::min(chunking.max_chars, chunking.max_chars));
    coalescing.min_chars = std::min(chunking.min_chars, coalescing.max_chars);

    if (coalesce_idle_ms_override >= 0) {
        coalescing.idle_ms = clamp_positive_integer(coalesce_idle_ms_override, DEFAULT_BLOCK_STREAM_COALESCE_IDLE_MS, 0, 5000);
    } else {
        coalescing.idle_ms = DEFAULT_BLOCK_STREAM_COALESCE_IDLE_MS;
    }

    // Resolve joiner based on break preference
    if (chunking.break_preference == "sentence") {
        coalescing.joiner = " ";
    } else if (chunking.break_preference == "newline") {
        coalescing.joiner = "\n";
    } else {
        coalescing.joiner = "\n\n";
    }

    coalescing.flush_on_enqueue = chunking.flush_on_paragraph;

    return {chunking, coalescing};
}

} // namespace outbound
} // namespace hunterclaw
