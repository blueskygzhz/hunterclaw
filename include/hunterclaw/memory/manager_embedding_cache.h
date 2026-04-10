#pragma once

// ============================================================================
// Manager Embedding Cache — 严格对齐 OpenClaw manager-embedding-cache.ts (117 行)
//
// Embedding 缓存的加载/更新/收集操作，按 content hash 缓存嵌入向量。
// ============================================================================

#include <string>
#include <vector>
#include <map>

namespace hunterclaw {
namespace memory {

struct EmbeddingCacheEntry {
    std::string hash;
    std::vector<double> embedding;
};

/**
 * Load cached embeddings by content hashes.
 * (对齐 manager-embedding-cache.ts:14-57 loadMemoryEmbeddingCache)
 */
std::map<std::string, std::vector<double>> load_memory_embedding_cache(
    const std::string& db_path,
    bool enabled,
    const std::string& provider_id,
    const std::string& provider_model,
    const std::string& provider_key,
    const std::vector<std::string>& hashes
);

/**
 * Upsert embedding cache entries.
 * (对齐 manager-embedding-cache.ts:59-94 upsertMemoryEmbeddingCache)
 */
void upsert_memory_embedding_cache(
    const std::string& db_path,
    bool enabled,
    const std::string& provider_id,
    const std::string& provider_model,
    const std::string& provider_key,
    const std::vector<EmbeddingCacheEntry>& entries,
    int64_t now_ms = 0
);

/**
 * Collect cached embeddings for chunks, returning missing indices.
 * (对齐 manager-embedding-cache.ts:96-117 collectMemoryCachedEmbeddings)
 */
struct CollectCacheResult {
    std::vector<std::vector<double>> embeddings;  // per-chunk, empty if miss
    std::vector<int> missing_indices;              // indices of cache misses
};

CollectCacheResult collect_memory_cached_embeddings(
    const std::vector<std::string>& chunk_hashes,
    const std::map<std::string, std::vector<double>>& cached
);

}  // namespace memory
}  // namespace hunterclaw
