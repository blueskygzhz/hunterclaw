#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/memory/multimodal.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <map>
#include <algorithm>
#include <functional>
#include <cmath>

namespace hunterclaw {
namespace memory {

// ============================================================================
// EmbeddingInput (对齐 openclaw embedding-inputs.ts)
// ============================================================================
struct EmbeddingInputPart {
    std::string type;       // "text" | "inline-data"
    std::string text;       // for type="text"
    std::string mime_type;  // for type="inline-data"
    std::string data;       // base64 for type="inline-data"
};

struct EmbeddingInput {
    std::string text;                        // plain text representation
    std::vector<EmbeddingInputPart> parts;   // structured parts (multimodal)
};

// ============================================================================
// MemoryChunk (对齐 openclaw internal.ts:22-29)
// ============================================================================
struct MemoryChunk {
    int start_line = 0;
    int end_line = 0;
    std::string text;
    std::string hash;
    std::optional<EmbeddingInput> embedding_input;  // optional multimodal input
};

// ============================================================================
// MemoryFileEntry (对齐 openclaw internal.ts:14-21)
// ============================================================================
struct MemoryFileEntry {
    std::string path;       // relative path from workspaceDir
    std::string abs_path;   // absolute path
    int64_t mtime_ms = 0;
    int64_t size = 0;
    std::string hash;
    std::optional<std::string> data_hash;   // for multimodal files
    std::string kind;                        // "markdown" | "multimodal"
    std::optional<std::string> content_text; // for multimodal label
    std::optional<MemoryMultimodalModality> modality;  // for multimodal files
    std::optional<std::string> mime_type;
};

// ============================================================================
// MultimodalMemoryChunk (对齐 openclaw internal.ts:31-34)
// ============================================================================
struct MultimodalMemoryChunk {
    MemoryChunk chunk;
    int structured_input_bytes = 0;
};

// ============================================================================
// ChunkingConfig (对齐 openclaw internal.ts chunkMarkdown 参数)
// ============================================================================
struct ChunkingConfig {
    int tokens = 400;    // max tokens per chunk
    int overlap = 80;    // overlap tokens between chunks
};

// ============================================================================
// Core Chunking Functions (对齐 openclaw internal.ts)
// ============================================================================

/**
 * Chunk markdown content into MemoryChunks (对齐 openclaw internal.ts:chunkMarkdown)
 * 
 * - Splits by character count (tokens * 4 chars per token)
 * - Carries overlap between chunks
 * - Returns chunks with 1-indexed start/end line numbers
 */
std::vector<MemoryChunk> chunk_markdown(
    const std::string& content,
    const ChunkingConfig& chunking = ChunkingConfig{}
);

/**
 * Remap chunk line numbers using a lineMap (对齐 openclaw internal.ts:remapChunkLines)
 * Used for session JSONL files where content is flattened before chunking.
 */
void remap_chunk_lines(
    std::vector<MemoryChunk>& chunks,
    const std::vector<int>& line_map
);

/**
 * Hash text using SHA-256 (对齐 openclaw internal.ts:hashText)
 */
std::string hash_text(const std::string& text);

/**
 * Parse embedding from JSON string (对齐 openclaw internal.ts:parseEmbedding)
 */
std::vector<float> parse_embedding(const std::string& raw);

/**
 * Cosine similarity between two vectors (对齐 openclaw internal.ts:cosineSimilarity)
 */
float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);

/**
 * Normalize relative path (对齐 openclaw internal.ts:normalizeRelPath)
 * Strips leading "./" and normalizes separators
 */
std::string normalize_rel_path(const std::string& value);

/**
 * Normalize extra memory paths (对齐 openclaw internal.ts:normalizeExtraMemoryPaths)
 */
std::vector<std::string> normalize_extra_memory_paths(
    const std::string& workspace_dir,
    const std::vector<std::string>& extra_paths
);

/**
 * Check if a relative path is a memory path (对齐 openclaw internal.ts:isMemoryPath)
 * Returns true for "MEMORY.md", "memory.md", or paths starting with "memory/"
 */
bool is_memory_path(const std::string& rel_path);

/**
 * List all memory files in workspace (对齐 openclaw internal.ts:listMemoryFiles)
 * Scans MEMORY.md, memory.md, memory/ directory, and extra_paths
 * multimodal: optional multimodal settings for non-markdown file support
 */
core::Result<std::vector<std::string>> list_memory_files(
    const std::string& workspace_dir,
    const std::vector<std::string>& extra_paths = {},
    const MemoryMultimodalSettings* multimodal = nullptr
);

/**
 * Build a MemoryFileEntry for a given file (对齐 openclaw internal.ts:buildFileEntry)
 * Returns nullopt if file is missing or invalid
 * multimodal: optional multimodal settings for non-markdown file support
 */
std::optional<MemoryFileEntry> build_file_entry(
    const std::string& abs_path,
    const std::string& workspace_dir,
    const MemoryMultimodalSettings* multimodal = nullptr
);

/**
 * Ensure directory exists (对齐 openclaw internal.ts:ensureDir)
 */
std::string ensure_dir(const std::string& dir);

// ============================================================================
// Embedding Input Helpers (对齐 openclaw embedding-inputs.ts)
// ============================================================================

/**
 * Build a text-only EmbeddingInput (对齐 buildTextEmbeddingInput)
 */
EmbeddingInput build_text_embedding_input(const std::string& text);

/**
 * Sanitize and normalize embedding vector
 * (对齐 openclaw embedding-vectors.ts:sanitizeAndNormalizeEmbedding)
 */
std::vector<float> sanitize_and_normalize_embedding(const std::vector<float>& embedding);

// ============================================================================
// Embedding Input Limits (对齐 openclaw embedding-input-limits.ts)
// ============================================================================

/**
 * Estimate UTF-8 byte length of a string
 * (对齐 openclaw embedding-input-limits.ts:estimateUtf8Bytes)
 */
int estimate_utf8_bytes(const std::string& text);

/**
 * Split text to UTF-8 byte limit using binary search
 * (对齐 openclaw embedding-input-limits.ts:splitTextToUtf8ByteLimit)
 */
std::vector<std::string> split_text_to_byte_limit(const std::string& text, int max_bytes);

/**
 * Enforce embedding max input tokens by splitting large chunks
 * (对齐 openclaw embedding-chunk-limits.ts:enforceEmbeddingMaxInputTokens)
 */
std::vector<MemoryChunk> enforce_embedding_max_input_tokens(
    const std::vector<MemoryChunk>& chunks,
    int max_input_tokens
);

/**
 * Resolve embedding max input tokens for a provider/model
 * (对齐 openclaw embedding-model-limits.ts:resolveEmbeddingMaxInputTokens)
 */
int resolve_embedding_max_input_tokens(
    const std::string& provider_id,
    const std::string& model,
    std::optional<int> provider_max_input_tokens = std::nullopt
);

// ============================================================================
// Concurrency Helper (对齐 openclaw internal.ts:runWithConcurrency)
// ============================================================================

/**
 * Run tasks with limited concurrency
 * In C++ this is implemented with a thread pool
 */
template<typename T>
std::vector<T> run_with_concurrency(
    std::vector<std::function<T()>> tasks,
    int limit
);

}  // namespace memory
}  // namespace hunterclaw
