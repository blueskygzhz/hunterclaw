#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

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
    std::optional<std::string> modality;     // "image" | "audio" | "video"
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
 */
core::Result<std::vector<std::string>> list_memory_files(
    const std::string& workspace_dir,
    const std::vector<std::string>& extra_paths = {}
);

/**
 * Build a MemoryFileEntry for a given file (对齐 openclaw internal.ts:buildFileEntry)
 * Returns nullopt if file is missing or invalid
 */
std::optional<MemoryFileEntry> build_file_entry(
    const std::string& abs_path,
    const std::string& workspace_dir
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
