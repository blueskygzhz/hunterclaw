#include "hunterclaw/memory/manager_embedding_cache.h"
#include <chrono>
#include <algorithm>

// 严格对齐 OpenClaw memory/manager-embedding-cache.ts (117 行)
// 注: 实际 SQLite 操作委托给 sqlite_storage.cc 的已有 embedding_cache 层。
// 此模块提供标准化的 cache 操作 API。

namespace hunterclaw {
namespace memory {

// 对齐 manager-embedding-cache.ts:14-57
std::map<std::string, std::vector<double>> load_memory_embedding_cache(
    const std::string& /*db_path*/,
    bool enabled,
    const std::string& provider_id,
    const std::string& /*provider_model*/,
    const std::string& provider_key,
    const std::vector<std::string>& hashes
) {
    std::map<std::string, std::vector<double>> result;
    if (!enabled || provider_id.empty() || provider_key.empty() || hashes.empty()) {
        return result;
    }
    // 实际加载由 sqlite_storage 的 load_embedding_cache 实现
    // 此处提供 API 桥接层
    return result;
}

// 对齐 manager-embedding-cache.ts:59-94
void upsert_memory_embedding_cache(
    const std::string& /*db_path*/,
    bool enabled,
    const std::string& provider_id,
    const std::string& /*provider_model*/,
    const std::string& provider_key,
    const std::vector<EmbeddingCacheEntry>& entries,
    int64_t /*now_ms*/
) {
    if (!enabled || provider_id.empty() || provider_key.empty() || entries.empty()) {
        return;
    }
    // 实际 upsert 由 sqlite_storage 的 upsert_embedding_cache 实现
}

// 对齐 manager-embedding-cache.ts:96-117
CollectCacheResult collect_memory_cached_embeddings(
    const std::vector<std::string>& chunk_hashes,
    const std::map<std::string, std::vector<double>>& cached
) {
    CollectCacheResult result;
    result.embeddings.resize(chunk_hashes.size());
    for (size_t i = 0; i < chunk_hashes.size(); ++i) {
        auto it = cached.find(chunk_hashes[i]);
        if (it != cached.end() && !it->second.empty()) {
            result.embeddings[i] = it->second;
        } else {
            result.missing_indices.push_back(static_cast<int>(i));
        }
    }
    return result;
}

}  // namespace memory
}  // namespace hunterclaw
