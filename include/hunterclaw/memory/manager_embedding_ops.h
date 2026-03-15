#pragma once

#include "hunterclaw/memory/manager_sync_ops.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"
#include <vector>
#include <string>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <future>

// 严格对齐 OpenClaw src/memory/manager-embedding-ops.ts
// 提供完整的批量 embedding、缓存和索引管理能力

namespace hunterclaw {
namespace memory {

// 前向声明
struct MemoryChunk;
struct MemoryFileEntry;
struct SessionFileEntry;
class EmbeddingProvider;

// ============================================================================
// Embedding Cache Entry
// ============================================================================

struct EmbeddingCacheEntry {
    std::string hash;
    std::vector<float> embedding;
};

// ============================================================================
// Batch Request Types (对齐 batch-*.ts)
// ============================================================================

struct BatchRequestBase {
    std::string custom_id;
};

struct OpenAiBatchRequest : BatchRequestBase {
    std::string method;  // "POST"
    std::string url;     // "/v1/embeddings"
    struct Body {
        std::string model;
        std::string input;
    } body;
};

struct GeminiBatchRequest : BatchRequestBase {
    struct Request {
        std::string model_path;
        struct Content {
            struct Part {
                std::string text;
            };
            std::vector<Part> parts;
        } content;
        std::string task_type;  // "RETRIEVAL_DOCUMENT"
        std::optional<int> output_dimensionality;
    } request;
};

struct VoyageBatchRequest : BatchRequestBase {
    struct Body {
        std::string input;
    } body;
};

// ============================================================================
// Manager Embedding Operations
// (继承自 ManagerSyncOps,添加 embedding 批处理能力)
// ============================================================================

class ManagerEmbeddingOps : public ManagerSyncOps {
protected:
    // Batch failure tracking (对齐 manager-embedding-ops.ts:50-53)
    int batch_failure_count_ = 0;
    std::string batch_failure_last_error_;
    std::string batch_failure_last_provider_;
    std::mutex batch_failure_mutex_;
    std::shared_future<void> batch_failure_lock_;

public:
    explicit ManagerEmbeddingOps(
        SQLiteStorage* storage,
        const MemoryConfig& config
    );
    virtual ~ManagerEmbeddingOps() = default;

    // ========================================================================
    // Embedding Batching (对齐 manager-embedding-ops.ts:55-83)
    // ========================================================================

    // 将chunks分批，每批不超过EMBEDDING_BATCH_MAX_TOKENS
    std::vector<std::vector<MemoryChunk>> 
    build_embedding_batches(const std::vector<MemoryChunk>& chunks);

    // ========================================================================
    // Embedding Cache (对齐 manager-embedding-ops.ts:85-183)
    // ========================================================================

    // 加载缓存的embeddings (对齐:85-125)
    std::map<std::string, std::vector<float>>
    load_embedding_cache(const std::vector<std::string>& hashes);

    // 更新缓存 (对齐:127-155)
    void upsert_embedding_cache(
        const std::vector<EmbeddingCacheEntry>& entries
    );

    // 清理缓存 (对齐:157-183)
    void prune_embedding_cache_if_needed();

    // ========================================================================
    // Core Embedding Logic (对齐 manager-embedding-ops.ts:185-286)
    // ========================================================================

    // 批量embedding (优先使用缓存) (对齐:185-226)
    Result<std::vector<std::vector<float>>>
    embed_chunks_in_batches(const std::vector<MemoryChunk>& chunks);

    // 收集缓存的embeddings (对齐:288-307)
    struct CachedEmbeddingsResult {
        std::vector<std::vector<float>> embeddings;
        struct MissingEntry {
            int index;
            MemoryChunk chunk;
        };
        std::vector<MissingEntry> missing;
    };
    CachedEmbeddingsResult 
    collect_cached_embeddings(const std::vector<MemoryChunk>& chunks);

    // ========================================================================
    // Provider Batch API (对齐 manager-embedding-ops.ts:442-522)
    // ========================================================================

    // OpenAI batch (对齐:465-491)
    Result<std::vector<std::vector<float>>>
    embed_chunks_with_openai_batch(
        const std::vector<MemoryChunk>& chunks,
        const MemoryFileEntry& entry,
        MemorySource source
    );

    // Gemini batch (对齐:493-522)
    Result<std::vector<std::vector<float>>>
    embed_chunks_with_gemini_batch(
        const std::vector<MemoryChunk>& chunks,
        const MemoryFileEntry& entry,
        MemorySource source
    );

    // Voyage batch (对齐:442-463)
    Result<std::vector<std::vector<float>>>
    embed_chunks_with_voyage_batch(
        const std::vector<MemoryChunk>& chunks,
        const MemoryFileEntry& entry,
        MemorySource source
    );

    // 通用batch入口 (对齐:268-286)
    Result<std::vector<std::vector<float>>>
    embed_chunks_with_batch(
        const std::vector<MemoryChunk>& chunks,
        const MemoryFileEntry& entry,
        MemorySource source
    );

    // ========================================================================
    // Retry & Timeout (对齐 manager-embedding-ops.ts:524-647)
    // ========================================================================

    // Batch retry (对齐:524-556)
    Result<std::vector<std::vector<float>>>
    embed_batch_with_retry(const std::vector<std::string>& texts);

    // Query timeout (对齐:615-626)
    Result<std::vector<float>>
    embed_query_with_timeout(const std::string& text);

    // Timeout wrapper (对齐:628-647)
    template<typename T>
    Result<T> with_timeout(
        std::function<Result<T>()> func,
        int timeout_ms,
        const std::string& message
    );

    // ========================================================================
    // Batch Failure Management (对齐 manager-embedding-ops.ts:649-754)
    // ========================================================================

    // 重置失败计数 (对齐:663-672)
    void reset_batch_failure_count();

    // 记录失败 (对齐:674-696)
    struct BatchFailureResult {
        bool disabled;
        int count;
    };
    BatchFailureResult record_batch_failure(
        const std::string& provider,
        const std::string& message,
        int attempts = 1,
        bool force_disable = false
    );

    // Batch fallback (对齐:723-754)
    template<typename T>
    Result<T> run_batch_with_fallback(
        const std::string& provider,
        std::function<Result<T>()> run_func,
        std::function<Result<std::vector<std::vector<float>>>()> fallback_func
    );

    // ========================================================================
    // File Indexing (对齐 manager-embedding-ops.ts:803-924)
    // ========================================================================

    // 索引单个文件 (核心方法) (对齐:803-924)
    Result<void> index_file(
        const MemoryFileEntry& entry,
        MemorySource source,
        const std::optional<std::string>& content = std::nullopt
    );

    // 清理索引数据 (对齐:760-778)
    void clear_indexed_file_data(
        const std::string& pathname,
        MemorySource source
    );

    // 更新文件记录 (对齐:780-791)
    void upsert_file_record(
        const MemoryFileEntry& entry,
        MemorySource source
    );

    // 删除文件记录 (对齐:793-795)
    void delete_file_record(
        const std::string& pathname,
        MemorySource source
    );

    // ========================================================================
    // Provider Key (对齐 manager-embedding-ops.ts:228-266)
    // ========================================================================

    // 计算 provider key (用于缓存隔离) (对齐:228-266)
    std::string compute_provider_key() const;

    // ========================================================================
    // Utilities
    // ========================================================================

    // 获取索引并发度 (对齐:756-758)
    int get_index_concurrency() const;

    // 判断是否为重试错误 (对齐:601-605)
    bool is_retryable_embedding_error(const std::string& message) const;

    // 判断是否为超时错误 (对齐:698-700)
    bool is_batch_timeout_error(const std::string& message) const;

    // 判断structured input过大错误 (对齐:797-801)
    bool is_structured_input_too_large_error(const std::string& message) const;

    // 解析embedding timeout (对齐:607-613)
    int resolve_embedding_timeout(const std::string& kind) const;  // "query" or "batch"

protected:
    // ========================================================================
    // Internal Helpers
    // ========================================================================

    // 等待重试 (对齐:592-599)
    void wait_for_embedding_retry(int delay_ms, const std::string& action);

    // Batch failure lock (对齐:649-661)
    template<typename T>
    T with_batch_failure_lock(std::function<T()> func);

    // Build batch custom ID (对齐:309-318)
    std::string build_batch_custom_id(
        MemorySource source,
        const MemoryFileEntry& entry,
        const MemoryChunk& chunk,
        int index
    );

    // Apply batch embeddings (对齐:345-360)
    void apply_batch_embeddings(
        const std::map<std::string, std::vector<float>>& by_custom_id,
        const std::map<std::string, std::pair<int, std::string>>& mapping,
        std::vector<std::vector<float>>& embeddings
    );

private:
    // Constants (对齐 manager-embedding-ops.ts:30-42)
    static constexpr int EMBEDDING_BATCH_MAX_TOKENS = 8000;
    static constexpr int EMBEDDING_INDEX_CONCURRENCY = 4;
    static constexpr int EMBEDDING_RETRY_MAX_ATTEMPTS = 3;
    static constexpr int EMBEDDING_RETRY_BASE_DELAY_MS = 500;
    static constexpr int EMBEDDING_RETRY_MAX_DELAY_MS = 8000;
    static constexpr int BATCH_FAILURE_LIMIT = 2;
    static constexpr int EMBEDDING_QUERY_TIMEOUT_REMOTE_MS = 60000;
    static constexpr int EMBEDDING_QUERY_TIMEOUT_LOCAL_MS = 300000;  // 5 min
    static constexpr int EMBEDDING_BATCH_TIMEOUT_REMOTE_MS = 120000;  // 2 min
    static constexpr int EMBEDDING_BATCH_TIMEOUT_LOCAL_MS = 600000;  // 10 min
};

}  // namespace memory
}  // namespace hunterclaw
