#pragma once

#include <optional>
#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/embedding_provider.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/search_algorithms.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <string>
#include <set>
#include <map>
#include <mutex>
#include <future>

namespace hunterclaw {
namespace memory {

// 严格对齐 OpenClaw src/memory/manager.ts

// ============================================================================
// MemoryIndexManager - 核心内存索引管理器
// (对齐 manager.ts:61 - MemoryIndexManager class)
// ============================================================================
class MemoryIndexManager : public MemorySearchManager {
public:
    // ========================================================================
    // 工厂方法 (对齐 manager.ts:135 - static async get())
    // ========================================================================
    
    struct Params {
        std::string agent_id;
        std::string workspace_dir;
        ResolvedMemorySearchConfig settings;
    };
    
    // 获取或创建MemoryIndexManager实例 (带缓存)
    static core::Result<std::shared_ptr<MemoryIndexManager>> get(const Params& params);
    
    // 关闭所有缓存的Manager实例
    static void close_all();
    
    ~MemoryIndexManager() override;
    
    // ========================================================================
    // MemorySearchManager接口实现 (types.ts:61-81)
    // ========================================================================
    
    std::vector<MemorySearchResult> search(
        const std::string& query,
        const SearchOptions& opts = {}
    ) override;
    
    ReadFileResult read_file(const ReadFileParams& params) override;
    
    MemoryProviderStatus status() override;
    
    void sync(const SyncParams& params = {}) override;
    
    MemoryEmbeddingProbeResult probe_embedding_availability() override;
    
    bool probe_vector_availability() override;
    
    void close() override;
    
    // ========================================================================
    // 额外的公共方法
    // ========================================================================
    
    // 获取workspace目录
    std::string get_workspace_dir() const { return workspace_dir_; }
    
    // 获取agent ID
    std::string get_agent_id() const { return agent_id_; }
    
    // 检查是否已关闭
    bool is_closed() const { return closed_; }
    
    // 检查索引是否dirty
    bool is_dirty() const { return dirty_; }

private:
    // 私有构造函数
    explicit MemoryIndexManager(const Params& params);
    
    // ========================================================================
    // 成员变量 (对齐 manager.ts:62-133)
    // ========================================================================
    
    // 缓存key
    std::string cache_key_;
    
    // 配置
    std::string agent_id_;
    std::string workspace_dir_;
    ResolvedMemorySearchConfig settings_;
    
    // Provider管理
    std::shared_ptr<EmbeddingProvider> provider_;
    std::string requested_provider_;  // "openai" | "local" | "gemini" | "voyage" | "mistral" | "ollama" | "auto"
    std::optional<std::string> fallback_from_;
    std::optional<std::string> fallback_reason_;
    std::optional<std::string> provider_unavailable_reason_;
    
    // Provider clients (对应 manager.ts:79-83)
    std::unique_ptr<OpenAiEmbeddingClient> openai_;
    std::unique_ptr<GeminiEmbeddingClient> gemini_;
    std::unique_ptr<VoyageEmbeddingClient> voyage_;
    std::unique_ptr<MistralEmbeddingClient> mistral_;
    std::unique_ptr<OllamaEmbeddingClient> ollama_;
    
    // Batch配置 (对应 manager.ts:84-90)
    struct {
        bool enabled;
        bool wait;
        int concurrency;
        int poll_interval_ms;
        int timeout_ms;
    } batch_;
    
    int batch_failure_count_ = 0;
    std::optional<std::string> batch_failure_last_error_;
    std::optional<std::string> batch_failure_last_provider_;
    std::mutex batch_failure_lock_;
    
    // 数据库 (对应 manager.ts:95)
    std::shared_ptr<SQLiteVectorStorage> db_;
    
    // 源集合 (对应 manager.ts:96)
    std::set<MemorySource> sources_;
    
    // Provider key (对应 manager.ts:97)
    std::string provider_key_;
    
    // 缓存配置 (对应 manager.ts:98)
    struct {
        bool enabled;
        std::optional<int> max_entries;
    } cache_;
    
    // 向量配置 (对应 manager.ts:99-105)
    struct {
        bool enabled;
        std::optional<bool> available;
        std::optional<std::string> extension_path;
        std::optional<std::string> load_error;
        std::optional<int> dims;
    } vector_;
    
    // FTS配置 (对应 manager.ts:106-110)
    struct {
        bool enabled;
        bool available;
        std::optional<std::string> load_error;
    } fts_;
    
    // 异步状态 (对应 manager.ts:111-116)
    std::shared_ptr<std::promise<bool>> vector_ready_;
    // File watcher相关在C++中用inotify/fsevents实现,这里简化处理
    
    // 状态标志 (对应 manager.ts:117-133)
    bool closed_ = false;
    bool dirty_ = false;
    bool sessions_dirty_ = false;
    std::set<std::string> sessions_dirty_files_;
    std::set<std::string> session_pending_files_;
    
    // Session tracking
    struct SessionDelta {
        size_t last_size;
        size_t pending_bytes;
        int pending_messages;
    };
    std::map<std::string, SessionDelta> session_deltas_;
    std::set<std::string> session_warm_;
    
    // 同步状态
    std::shared_ptr<std::future<void>> syncing_;
    std::set<std::string> queued_session_files_;
    std::shared_ptr<std::future<void>> queued_session_sync_;
    
    // Readonly recovery状态
    int readonly_recovery_attempts_ = 0;
    int readonly_recovery_successes_ = 0;
    int readonly_recovery_failures_ = 0;
    std::optional<std::string> readonly_recovery_last_error_;
    
    // 线程安全
    mutable std::mutex mutex_;
    
    // ========================================================================
    // 初始化方法 (对齐 manager.ts 的构造和初始化)
    // ========================================================================
    
    core::Result<bool> initialize();
    core::Result<bool> ensure_provider();
    core::Result<bool> ensure_database();
    core::Result<bool> ensure_vector_ready();
    
    // ========================================================================
    // Provider管理 (对齐 manager-embedding-ops.ts)
    // ========================================================================
    
    core::Result<std::shared_ptr<EmbeddingProvider>> create_provider_by_type(
        const std::string& provider_type
    );
    
    core::Result<std::shared_ptr<EmbeddingProvider>> try_fallback_provider();
    
    // ========================================================================
    // 搜索实现 (对齐 manager-search.ts)
    // ========================================================================
    
    // 向量搜索 (searchVector)
    core::Result<std::vector<MemorySearchResult>> search_vector(
        const std::vector<float>& query_embedding,
        const SearchOptions& opts
    );
    
    // 关键词搜索 (searchKeyword)
    core::Result<std::vector<MemorySearchResult>> search_keyword(
        const std::string& query,
        const SearchOptions& opts
    );
    
    // 混合搜索 (组合vector和keyword)
    core::Result<std::vector<MemorySearchResult>> search_hybrid(
        const std::string& query,
        const std::vector<float>& query_embedding,
        const SearchOptions& opts
    );
    
    // ========================================================================
    // 同步实现 (对齐 manager-sync-ops.ts)
    // ========================================================================
    
    core::Result<bool> sync_memory_files(
        const SyncParams& params,
        std::function<void(const MemorySyncProgressUpdate&)> progress_callback
    );
    
    core::Result<bool> sync_session_files(
        const SyncParams& params,
        std::function<void(const MemorySyncProgressUpdate&)> progress_callback
    );
    
    // ========================================================================
    // Embedding操作 (对齐 manager-embedding-ops.ts)
    // ========================================================================
    
    core::Result<std::vector<float>> embed_query(const std::string& text);
    
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    );
    
    // 带缓存的embedding
    core::Result<std::vector<float>> embed_with_cache(
        const std::string& text,
        const std::string& model
    );
    
    // ========================================================================
    // 文件处理
    // ========================================================================
    
    core::Result<bool> index_file(
        const std::string& file_path,
        MemorySource source
    );
    
    core::Result<bool> remove_file(
        const std::string& file_path,
        MemorySource source
    );
    
    // 文件分块
    std::vector<std::string> chunk_file_content(
        const std::string& content,
        int chunk_tokens,
        int overlap_tokens
    );
    
    // ========================================================================
    // 辅助方法
    // ========================================================================
    
    std::string generate_cache_key() const;
    
    bool should_index_file(const std::string& file_path) const;
    
    int64_t get_current_timestamp() const;
    
    // ========================================================================
    // 静态缓存 (对应 manager.ts:42-43)
    // ========================================================================
    
    static std::map<std::string, std::shared_ptr<MemoryIndexManager>> index_cache_;
    static std::map<std::string, std::shared_ptr<std::future<std::shared_ptr<MemoryIndexManager>>>> index_cache_pending_;
    static std::mutex cache_mutex_;
};

// ============================================================================
// 全局函数 (对齐 manager.ts:45-59)
// ============================================================================

// 关闭所有MemoryIndexManager实例
void close_all_memory_index_managers();

} // namespace memory
} // namespace hunterclaw
