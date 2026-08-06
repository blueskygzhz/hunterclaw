#pragma once

#include <optional>
#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/chunking.h"   // MemoryChunk, MemoryFileEntry (对齐 openclaw internal.ts)
#include "hunterclaw/memory/session_files.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>
#include <thread>
#include <mutex>
#include <future>
#include <memory>

// 严格对齐 OpenClaw src/memory/manager-sync-ops.ts

namespace hunterclaw {
namespace memory {

// Forward declarations
class SQLiteVectorStorage;
class EmbeddingProvider;

// ============================================================================
// MemoryIndexMeta (对齐 manager-sync-ops.ts:54-63)
// ============================================================================
struct MemoryIndexMeta {
    std::string model;
    std::string provider;
    std::optional<std::string> provider_key;
    std::optional<std::vector<MemorySource>> sources;
    std::optional<std::string> scope_hash;
    int chunk_tokens = 0;
    int chunk_overlap = 0;
    std::optional<int> vector_dims;
    std::string fts_tokenizer = "unicode61";  // 对齐 OC: meta.ftsTokenizer
};

// ============================================================================
// MemorySyncProgressState (对齐 manager-sync-ops.ts:65-70)
// ============================================================================
struct MemorySyncProgressState {
    int completed = 0;
    int total = 0;
    std::optional<std::string> label;
    std::function<void(const MemorySyncProgressUpdate&)> report;
};

// MemoryFileEntry is defined in chunking.h (对齐 openclaw internal.ts)

// ============================================================================
// MemoryManagerSyncOps (对齐 manager-sync-ops.ts:97-168)
// ============================================================================
class MemoryManagerSyncOps {
public:
    virtual ~MemoryManagerSyncOps() = default;

protected:
    // ========================================================================
    // 抽象成员变量 (对齐 manager-sync-ops.ts:98-148)
    // ========================================================================

    // 核心配置 (对齐 openclaw: protected abstract readonly cfg)
    // 在 C++ 中通过 settings_ 访问配置，不需要单独的 cfg 字段
    std::string agent_id_;
    std::string workspace_dir_;
    ResolvedMemorySearchConfig settings_;

    // Embedding provider (对齐 openclaw: protected provider)
    std::shared_ptr<EmbeddingProvider> provider_;
    std::optional<std::string> fallback_from_;   // "openai" | "local" | "gemini" | "voyage" | "mistral" | "ollama"
    std::optional<std::string> fallback_reason_;
    std::optional<std::string> requested_provider_;
    std::optional<std::string> provider_unavailable_reason_;

    // Batch configuration (对齐 openclaw: protected abstract batch)
    struct BatchState {
        bool enabled = false;
        bool wait = true;
        int concurrency = 2;
        int poll_interval_ms = 2000;
        int timeout_ms = 3600000;  // 60 min
    } batch_;

    std::set<MemorySource, std::less<MemorySource>> sources_;
    std::string provider_key_;

    // Cache configuration (对齐 openclaw: protected abstract cache)
    struct CacheState {
        bool enabled = false;
        std::optional<int> max_entries;
    } cache_;

    // Vector configuration (对齐 openclaw: protected abstract vector)
    struct VectorState {
        bool enabled = false;
        std::optional<bool> available;
        std::optional<std::string> extension_path;
        std::optional<std::string> load_error;
        std::optional<int> dims;
    } vector_;

    // FTS configuration (对齐 openclaw: protected fts)
    struct FtsState {
        bool enabled = false;
        bool available = false;
        std::optional<std::string> load_error;
    } fts_;

    // State flags (对齐 openclaw: protected closed/dirty/sessionsDirty)
    bool closed_ = false;
    bool dirty_ = false;
    bool sessions_dirty_ = false;

    std::set<std::string> sessions_dirty_files_;
    std::set<std::string> session_pending_files_;

    // Session delta tracking (对齐 openclaw: protected sessionDeltas)
    struct SessionDelta {
        size_t last_size = 0;
        size_t pending_bytes = 0;
        int pending_messages = 0;
    };
    std::map<std::string, SessionDelta> session_deltas_;

    // Meta serialization cache (对齐 openclaw: private lastMetaSerialized)
    std::optional<std::string> last_meta_serialized_;

    // Database (对齐 openclaw: protected abstract db: DatabaseSync)
    // 使用 SQLiteVectorStorage 封装 SQLite 连接
    std::shared_ptr<SQLiteVectorStorage> db_;

    std::mutex mutex_;

    // vectorReady promise (对齐 openclaw: protected vectorReady)
    std::shared_ptr<std::future<bool>> vector_ready_future_;
    std::shared_ptr<std::promise<bool>> vector_ready_promise_;

    // Watch timers (对齐 openclaw: protected watcher/watchTimer/sessionWatchTimer/intervalTimer)
    std::unique_ptr<std::thread> watch_timer_thread_;
    std::unique_ptr<std::thread> session_watch_timer_thread_;
    std::unique_ptr<std::thread> interval_timer_thread_;
    bool watch_timer_active_ = false;
    bool session_watch_timer_active_ = false;
    bool interval_timer_active_ = false;

    // ========================================================================
    // 抽象方法 (需要子类实现，对齐 openclaw abstract methods)
    // ========================================================================

    // 计算 provider key (对齐 openclaw: protected abstract computeProviderKey())
    virtual std::string compute_provider_key() = 0;

    // 同步 (对齐 openclaw: protected abstract sync())
    // 子类实现具体的 sync 逻辑
    virtual void do_sync(const SyncParams& params) = 0;

    // 获取索引并发度 (对齐 openclaw: protected abstract getIndexConcurrency())
    virtual int get_index_concurrency() = 0;

    // 清理 embedding 缓存 (对齐 openclaw: protected abstract pruneEmbeddingCacheIfNeeded())
    virtual void prune_embedding_cache_if_needed() = 0;

    // 索引文件 (对齐 openclaw: protected abstract indexFile())
    virtual core::Result<void> index_file(
        const MemoryFileEntry& entry,
        MemorySource source,
        const std::optional<std::string>& content = std::nullopt
    ) = 0;

    // 索引 session 文件 (对齐 openclaw: indexFile with SessionFileEntry)
    virtual core::Result<void> index_session_file(
        const SessionFileEntry& entry,
        MemorySource source
    ) = 0;

    // withTimeout (对齐 openclaw: protected abstract withTimeout())
    // C++ 中通过 std::future::wait_for 实现
    template<typename T>
    T with_timeout(std::future<T>& fut, int timeout_ms, const std::string& message);

    // ========================================================================
    // 核心同步方法 (对齐 manager-sync-ops.ts)
    // ========================================================================

    /**
     * Main sync entry point (对齐 manager-sync-ops.ts:runSync)
     * 包含完整的 reindex 决策逻辑
     */
    core::Result<void> run_sync(const SyncParams& params = {});

    /**
     * Safe atomic reindex (对齐 manager-sync-ops.ts:runSafeReindex)
     * 使用临时数据库，成功后原子替换
     */
    core::Result<void> run_safe_reindex(const SyncParams& params);

    /**
     * Unsafe fast reindex (对齐 manager-sync-ops.ts:runUnsafeReindex)
     * 直接清空并重建，用于测试
     */
    core::Result<void> run_unsafe_reindex(const SyncParams& params);

    /**
     * Reset index (对齐 manager-sync-ops.ts:resetIndex)
     */
    void reset_index();

    /**
     * Ensure vector extension is loaded (对齐 manager-sync-ops.ts:ensureVectorReady)
     */
    bool ensure_vector_ready(std::optional<int> dimensions = std::nullopt);

    /**
     * Load vector extension (对齐 manager-sync-ops.ts:loadVectorExtension)
     */
    bool load_vector_extension();

    /**
     * Ensure vector table exists (对齐 manager-sync-ops.ts:ensureVectorTable)
     */
    void ensure_vector_table(int dimensions);

    /**
     * Drop vector table (对齐 manager-sync-ops.ts:dropVectorTable)
     */
    void drop_vector_table();

    /**
     * Build source filter SQL (对齐 manager-sync-ops.ts:buildSourceFilter)
     */
    struct SourceFilterResult {
        std::string sql;
        std::vector<MemorySource> params;
    };
    SourceFilterResult build_source_filter(const std::optional<std::string>& alias = std::nullopt);

    /**
     * Open database (对齐 manager-sync-ops.ts:openDatabase)
     */
    std::shared_ptr<SQLiteVectorStorage> open_database();

    /**
     * Open database at specific path (对齐 manager-sync-ops.ts:openDatabaseAtPath)
     */
    std::shared_ptr<SQLiteVectorStorage> open_database_at_path(const std::string& db_path);

    /**
     * Seed embedding cache from source db (对齐 manager-sync-ops.ts:seedEmbeddingCache)
     */
    void seed_embedding_cache(SQLiteVectorStorage& source_db);

    /**
     * Swap index files atomically (对齐 manager-sync-ops.ts:swapIndexFiles)
     */
    core::Result<void> swap_index_files(const std::string& target_path, const std::string& temp_path);

    /**
     * Ensure schema is up to date (对齐 manager-sync-ops.ts:ensureSchema)
     */
    void ensure_schema();

    /**
     * Start memory file watcher (对齐 manager-sync-ops.ts:ensureWatcher)
     */
    void start_memory_watcher();

    /**
     * Stop memory file watcher
     */
    void stop_memory_watcher();

    /**
     * Start session file watcher (对齐 manager-sync-ops.ts:ensureSessionListener)
     */
    void start_session_watcher();

    /**
     * Stop session watcher
     */
    void stop_session_watcher();

    /**
     * Start interval sync (对齐 manager-sync-ops.ts:ensureIntervalSync)
     */
    void ensure_interval_sync();

    /**
     * Schedule watch sync (对齐 manager-sync-ops.ts:scheduleWatchSync)
     */
    void schedule_watch_sync();

    /**
     * Schedule session dirty (对齐 manager-sync-ops.ts:scheduleSessionDirty)
     */
    void schedule_session_dirty(const std::string& session_file);

    /**
     * Process session delta batch (对齐 manager-sync-ops.ts:processSessionDeltaBatch)
     */
    void process_session_delta_batch();

    /**
     * Update session delta (对齐 manager-sync-ops.ts:updateSessionDelta)
     */
    struct SessionDeltaResult {
        int delta_bytes;
        int delta_messages;
        size_t pending_bytes;
        int pending_messages;
    };
    std::optional<SessionDeltaResult> update_session_delta(const std::string& session_file);

    /**
     * Reset session delta (对齐 manager-sync-ops.ts:resetSessionDelta)
     */
    void reset_session_delta(const std::string& abs_path, size_t size);

    /**
     * Check if session file belongs to this agent (对齐 manager-sync-ops.ts:isSessionFileForAgent)
     */
    bool is_session_file_for_agent(const std::string& session_file) const;

    /**
     * Normalize target session files (对齐 manager-sync-ops.ts:normalizeTargetSessionFiles)
     */
    std::optional<std::set<std::string>> normalize_target_session_files(
        const std::optional<std::vector<std::string>>& session_files
    ) const;

    /**
     * Clear synced session files (对齐 manager-sync-ops.ts:clearSyncedSessionFiles)
     */
    void clear_synced_session_files(const std::optional<std::set<std::string>>& target = std::nullopt);

    /**
     * Should sync sessions (对齐 manager-sync-ops.ts:shouldSyncSessions)
     */
    bool should_sync_sessions(const SyncParams& params, bool needs_full_reindex = false) const;

    /**
     * Should fallback on error (对齐 manager-sync-ops.ts:shouldFallbackOnError)
     */
    bool should_fallback_on_error(const std::string& message) const;

    /**
     * Activate fallback provider (对齐 manager-sync-ops.ts:activateFallbackProvider)
     */
    bool activate_fallback_provider(const std::string& reason);

    /**
     * Resolve batch config (对齐 manager-sync-ops.ts:resolveBatchConfig)
     */
    BatchState resolve_batch_config() const;

    /**
     * Sync memory files (对齐 manager-sync-ops.ts:syncMemoryFiles)
     */
    core::Result<void> sync_memory_files(
        bool needs_full_reindex,
        MemorySyncProgressState* progress = nullptr
    );

    /**
     * Sync session files (对齐 manager-sync-ops.ts:syncSessionFiles)
     */
    core::Result<void> sync_session_files(
        bool needs_full_reindex,
        const std::optional<std::vector<std::string>>& target_session_files = std::nullopt,
        MemorySyncProgressState* progress = nullptr
    );

    /**
     * Read meta from database (对齐 manager-sync-ops.ts:readMeta)
     */
    std::optional<MemoryIndexMeta> read_meta();

    /**
     * Write meta to database (对齐 manager-sync-ops.ts:writeMeta)
     */
    void write_meta(const MemoryIndexMeta& meta);

    /**
     * Resolve configured sources for meta (对齐 manager-sync-ops.ts:resolveConfiguredSourcesForMeta)
     */
    std::vector<MemorySource> resolve_configured_sources_for_meta() const;

    /**
     * Resolve configured scope hash (对齐 manager-sync-ops.ts:resolveConfiguredScopeHash)
     */
    std::string resolve_configured_scope_hash() const;

    /**
     * Check if meta sources differ (对齐 manager-sync-ops.ts:metaSourcesDiffer)
     */
    bool meta_sources_differ(const MemoryIndexMeta& meta,
                              const std::vector<MemorySource>& configured_sources) const;

    /**
     * Normalize meta sources (对齐 manager-sync-ops.ts:normalizeMetaSources)
     */
    std::vector<MemorySource> normalize_meta_sources(const MemoryIndexMeta& meta) const;

    // ========================================================================
    // Deprecated / backward compat aliases
    // ========================================================================

    // 旧接口别名，保持向后兼容
    inline std::optional<MemoryIndexMeta> load_index_meta() { return read_meta(); }
    inline void save_index_meta() { /* meta is written explicitly via write_meta */ }
};

// ============================================================================
// 常量定义 (对齐 manager-sync-ops.ts:72-87)
// ============================================================================
constexpr const char* META_KEY = "memory_index_meta_v1";
constexpr const char* VECTOR_TABLE = "chunks_vec";
constexpr const char* FTS_TABLE = "chunks_fts";
constexpr const char* EMBEDDING_CACHE_TABLE = "embedding_cache";
constexpr int SESSION_DIRTY_DEBOUNCE_MS = 5000;
constexpr int SESSION_DELTA_READ_CHUNK_BYTES = 64 * 1024;
constexpr int VECTOR_LOAD_TIMEOUT_MS = 30000;

// Ignored directory names for file watching (对齐 manager-sync-ops.ts:78-87)
const std::set<std::string> IGNORED_MEMORY_WATCH_DIR_NAMES = {
    ".git", "node_modules", ".pnpm-store", ".venv", "venv",
    ".tox", "__pycache__"
};

/**
 * Check if path should be ignored for watching (对齐 manager-sync-ops.ts:91-95)
 */
bool should_ignore_memory_watch_path(const std::string& watch_path);

} // namespace memory
} // namespace hunterclaw
