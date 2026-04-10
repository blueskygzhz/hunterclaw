#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstddef>

// 严格对齐 OpenClaw src/memory/manager-embedding-policy.ts (132行)
// 独立的 embedding 策略函数：批次构建、重试判断、延迟计算

namespace hunterclaw {
namespace memory {

// ============================================================================
// EmbeddingInput types (对齐 manager-embedding-policy.ts:3-22)
// ============================================================================

struct MemoryEmbeddingTextPart {
    std::string type;  // "text"
    std::string text;
};

struct MemoryEmbeddingInlineDataPart {
    std::string type;  // "inline-data"
    std::string mime_type;
    std::string data;
};

struct MemoryEmbeddingInput {
    std::string text;
    // simplified: parts stored as text-only for C++
    bool has_structured_parts = false;
    size_t structured_bytes = 0;
};

struct MemoryEmbeddingChunk {
    std::string text;
    std::string hash;
    MemoryEmbeddingInput embedding_input;
    bool has_embedding_input = false;
};

// ============================================================================
// estimateUtf8Bytes (对齐 manager-embedding-policy.ts:24-29)
// ============================================================================
size_t estimate_utf8_bytes(const std::string& text);

// ============================================================================
// estimateStructuredEmbeddingInputBytes (对齐 manager-embedding-policy.ts:31-45)
// ============================================================================
size_t estimate_structured_embedding_input_bytes(const MemoryEmbeddingInput& input);

// ============================================================================
// filterNonEmptyMemoryChunks (对齐 manager-embedding-policy.ts:47-49)
// ============================================================================
template<typename T>
std::vector<T> filter_non_empty_memory_chunks(const std::vector<T>& chunks);

// ============================================================================
// buildMemoryEmbeddingBatches (对齐 manager-embedding-policy.ts:51-81)
// ============================================================================
template<typename T>
std::vector<std::vector<T>> build_memory_embedding_batches(
    const std::vector<T>& chunks,
    size_t max_tokens
);

// ============================================================================
// isRetryableMemoryEmbeddingError (对齐 manager-embedding-policy.ts:83-87)
// ============================================================================
bool is_retryable_memory_embedding_error(const std::string& message);

// ============================================================================
// isStructuredInputTooLargeMemoryEmbeddingError (对齐 manager-embedding-policy.ts:89-93)
// ============================================================================
bool is_structured_input_too_large_memory_embedding_error(const std::string& message);

// ============================================================================
// resolveMemoryEmbeddingRetryDelay (对齐 manager-embedding-policy.ts:95-101)
// ============================================================================
int resolve_memory_embedding_retry_delay(int delay_ms, double random_value, int max_delay_ms);

// ============================================================================
// runMemoryEmbeddingRetryLoop (对齐 manager-embedding-policy.ts:103-125)
// ============================================================================
template<typename T>
T run_memory_embedding_retry_loop(
    std::function<T()> run,
    std::function<bool(const std::string&)> is_retryable,
    std::function<void(int)> wait_for_retry,
    int max_attempts,
    int base_delay_ms
);

// ============================================================================
// buildTextEmbeddingInputs (对齐 manager-embedding-policy.ts:127-131)
// ============================================================================
std::vector<MemoryEmbeddingInput> build_text_embedding_inputs(
    const std::vector<MemoryEmbeddingChunk>& chunks
);

} // namespace memory
} // namespace hunterclaw
