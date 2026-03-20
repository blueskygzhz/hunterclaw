#pragma once

#include <optional>
#include "hunterclaw/memory/manager_sync_ops.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/chunking.h"
#include "hunterclaw/core/types.h"
#include <vector>
#include <string>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <future>

// 严格对齐 OpenClaw src/memory/manager-embedding-ops.ts
// MemoryManagerEmbeddingOps extends MemoryManagerSyncOps (对齐 openclaw 继承链)

namespace hunterclaw {
namespace memory {

// 前向声明
struct SessionFileEntry;
class EmbeddingProvider;

// ============================================================================
// Constants (对齐 manager-embedding-ops.ts:30-42)
// ============================================================================
constexpr int EMBEDDING_BATCH_MAX_TOKENS = 8000;
constexpr int EMBEDDING_INDEX_CONCURRENCY = 4;
constexpr int EMBEDDING_RETRY_MAX_ATTEMPTS = 3;
constexpr int EMBEDDING_RETRY_BASE_DELAY_MS = 500;
constexpr int EMBEDDING_RETRY_MAX_DELAY_MS = 8000;
constexpr int BATCH_FAILURE_LIMIT = 2;
constexpr int EMBEDDING_QUERY_TIMEOUT_REMOTE_MS = 60000;
constexpr int EMBEDDING_QUERY_TIMEOUT_LOCAL_MS = 300000;   // 5 min
constexpr int EMBEDDING_BATCH_TIMEOUT_REMOTE_MS = 120000;  // 2 min
constexpr int EMBEDDING_BATCH_TIMEOUT_LOCAL_MS = 600000;   // 10 min

// ============================================================================
// Batch Request Types (对齐 batch-*.ts)
// ============================================================================

struct OpenAiBatchRequest {
    std::string custom_id;
    std::string method;  // "POST"
    std::string url;     // "/v1/embeddings"
    struct Body {
        std::string model;
        std::string input;
    } body;
};

struct GeminiBatchRequest {
    std::string custom_id;
    struct Request {
        std::string model_path;
        struct Content {
            struct Part { std::string text; };
            std::vector<Part> parts;
        } content;
        std::string task_type;  // "RETRIEVAL_DOCUMENT"
        std::optional<int> output_dimensionality;
    } request;
};

struct VoyageBatchRequest {
    std::string custom_id;
    struct Body { std::string input; } body;
};

// ============================================================================
// MemoryManagerEmbeddingOps
// 严格对齐 openclaw manager-embedding-ops.ts:
//   export abstract class MemoryManagerEmbeddingOps extends MemoryManagerSyncOps
// ============================================================================
class MemoryManagerEmbeddingOps : public MemoryManagerSyncOps {
protected:
    // Batch failure tracking (对齐 manager-embedding-ops.ts:50-53)
    int batch_failure_count_ = 0;
    std::optional<std::string> batch_failure_last_error_;
    std::optional<std::string> batch_failure_last_provider_;
    std::mutex batch_failure_mutex_;

public:
    virtual ~MemoryManagerEmbeddingOps() = default;

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
        const std::string& hash,
        const std::vector<float>& embedding
    );

    // 清理缓存 (对齐:157-183)
    void prune_embedding_cache_if_needed() override;

    // ========================================================================
    // Core Embedding Logic (对齐 manager-embedding-ops.ts:185-286)
    // ========================================================================

    // 批量embedding (优先使用缓存) (对齐:185-226)
    core::Result<std::vector<std::vector<float>>>
    embed_chunks_in_batches(const std::vector<MemoryChunk>& chunks);

    // ========================================================================
    // Provider Batch API (对齐 manager-embedding-ops.ts:442-522)
    // ========================================================================

    // OpenAI batch (对齐:465-491)
    core::Result<std::vector<std::vector<float>>>
    embed_chunks_with_openai_batch(
        const std::vector<MemoryChunk>& chunks,
        const MemoryFileEntry& entry,
        MemorySource source
    );

    // Gemini batch (对齐:493-522)
    core::Result<std::vector<std::vector<float>>>
    embed_chunks_with_gemini_batch(
        const std::vector<MemoryChunk>& chunks,
        const MemoryFileEntry& entry,
        MemorySource source
    );

    // Voyage batch (对齐:442-463)
    core::Result<std::vector<std::vector<float>>>
    embed_chunks_with_voyage_batch(
        const std::vector<MemoryChunk>& chunks,
        const MemoryFileEntry& entry,
        MemorySource source
    );

    // 通用batch入口 (对齐:268-286)
    core::Result<std::vector<std::vector<float>>>
    embed_chunks_with_batch(
        const std::vector<MemoryChunk>& chunks,
        const MemoryFileEntry& entry,
        MemorySource source
    );

    // ========================================================================
    // Retry & Timeout (对齐 manager-embedding-ops.ts:524-647)
    // ========================================================================

    // Batch retry (对齐:524-556)
    core::Result<std::vector<std::vector<float>>>
    embed_batch_with_retry(const std::vector<std::string>& texts);

    // Query embedding with timeout (对齐:615-626)
    core::Result<std::vector<float>>
    embed_query_with_timeout(const std::string& text);

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

    // ========================================================================
    // File Indexing (对齐 manager-embedding-ops.ts:803-924)
    // ========================================================================

    // 索引单个文件 (核心方法) (对齐:803-924)
    core::Result<void> index_file(
        const MemoryFileEntry& entry,
        MemorySource source,
        const std::optional<std::string>& content = std::nullopt
    ) override;

    // 索引session文件 (对齐 manager-sync-ops.ts)
    core::Result<void> index_session_file(
        const SessionFileEntry& entry,
        MemorySource source
    ) override;

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
    std::string compute_provider_key() override;

    // ========================================================================
    // Utilities
    // ========================================================================

    // 获取索引并发度 (对齐:756-758)
    int get_index_concurrency() override;

    // 判断是否为重试错误 (对齐:601-605)
    bool is_retryable_embedding_error(const std::string& message) const;

    // 判断是否为超时错误 (对齐:698-700)
    bool is_batch_timeout_error(const std::string& message) const;

    // 解析embedding timeout (对齐:607-613)
    int resolve_embedding_timeout(const std::string& kind) const;  // "query" or "batch"

    // 确保向量表准备就绪
    core::Result<bool> ensure_vector_ready(std::optional<int> dimensions = std::nullopt);

protected:
    // ========================================================================
    // Internal Helpers
    // ========================================================================

    // 等待重试 (对齐:592-599)
    void wait_for_embedding_retry(int delay_ms, const std::string& action);

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
};

// Backward compatibility alias
using ManagerEmbeddingOps = MemoryManagerEmbeddingOps;

}  // namespace memory
}  // namespace hunterclaw
