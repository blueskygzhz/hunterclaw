#include "hunterclaw/memory/manager_embedding_policy.h"
#include <regex>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hunterclaw {
namespace memory {

// ============================================================================
// estimateUtf8Bytes — 使用 chunking.cc 中已有的实现
// 此处仅声明 policy-specific 的 estimate 函数作为包装
// ============================================================================
static size_t policy_estimate_utf8_bytes(const std::string& text) {
    return text.size();
}

// ============================================================================
// estimateStructuredEmbeddingInputBytes (对齐 manager-embedding-policy.ts:31-45)
// ============================================================================
size_t estimate_structured_embedding_input_bytes(const MemoryEmbeddingInput& input) {
    if (!input.has_structured_parts) {
        return policy_estimate_utf8_bytes(input.text);
    }
    return input.structured_bytes > 0 ? input.structured_bytes : policy_estimate_utf8_bytes(input.text);
}

// ============================================================================
// isRetryableMemoryEmbeddingError (对齐 manager-embedding-policy.ts:83-87)
// ============================================================================
bool is_retryable_memory_embedding_error(const std::string& message) {
    // /(rate[_ ]limit|too many requests|429|resource has been exhausted|5\d\d|cloudflare|tokens per day)/i
    static const std::regex pattern(
        "rate[_ ]limit|too many requests|429|resource has been exhausted|5\\d\\d|cloudflare|tokens per day",
        std::regex_constants::icase);
    return std::regex_search(message, pattern);
}

// ============================================================================
// isStructuredInputTooLargeMemoryEmbeddingError (对齐 manager-embedding-policy.ts:89-93)
// ============================================================================
bool is_structured_input_too_large_memory_embedding_error(const std::string& message) {
    // /(413|payload too large|request too large|input too large|too many tokens|input limit|request size)/i
    static const std::regex pattern(
        "413|payload too large|request too large|input too large|too many tokens|input limit|request size",
        std::regex_constants::icase);
    return std::regex_search(message, pattern);
}

// ============================================================================
// resolveMemoryEmbeddingRetryDelay (对齐 manager-embedding-policy.ts:95-101)
// ============================================================================
int resolve_memory_embedding_retry_delay(int delay_ms, double random_value, int max_delay_ms) {
    return std::min(max_delay_ms, static_cast<int>(std::round(delay_ms * (1.0 + random_value * 0.2))));
}

// ============================================================================
// buildTextEmbeddingInputs (对齐 manager-embedding-policy.ts:127-131)
// ============================================================================
std::vector<MemoryEmbeddingInput> build_text_embedding_inputs(
    const std::vector<MemoryEmbeddingChunk>& chunks
) {
    std::vector<MemoryEmbeddingInput> result;
    result.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        if (chunk.has_embedding_input) {
            result.push_back(chunk.embedding_input);
        } else {
            MemoryEmbeddingInput input;
            input.text = chunk.text;
            result.push_back(input);
        }
    }
    return result;
}

} // namespace memory
} // namespace hunterclaw
