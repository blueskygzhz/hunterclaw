#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/session_files.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>
#include <thread>
#include <mutex>

// 严格对齐 OpenClaw src/memory/manager-sync-ops.ts

namespace hunterclaw {
namespace memory {

// Forward declaration
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
    int chunk_tokens;
    int chunk_overlap;
    std::optional<int> vector_dims;
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

// ============================================================================
// MemoryFileEntry (简化版)
// ============================================================================
struct MemoryFileEntry {
    std::string path;
    std::string abs_path;
    int64_t mtime_ms;
    size_t size;
    std::string hash;
};

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
    std::string agent_id_;
    std::string workspace_dir_;
    ResolvedMemorySearchConfig settings_;
    
    std::shared_ptr<EmbeddingProvider> provider_;
    std::optional<std::string> fallback_from_;
    std::optional<std::string> fallback_reason_;
    
    // Batch configuration
    struct {
        bool enabled;
        bool wait;
        int concurrency;
        int poll_interval_ms;
        int timeout_ms;
    } batch_;
    
    std::set<MemorySource> sources_;
    std::string provider_key_;
    
    // Cache configuration
    struct {
        bool enabled;
        std::optional<int> max_entries;
    } cache_;
    
    // Vector configuration
    struct {
        bool enabled;
        std::optional<bool> available;
        std::optional<std::string> extension_path;
        std::optional<std::string> load_error;
        std::optional<int> dims;
    } vector_;
    
    // FTS configuration
    struct {
        bool enabled;
        bool available;
        std::optional<std::string> load_error;
    } fts_;
    
    // State flags
    std::shared_ptr<std::promise<bool>> vector_ready_;
    bool closed_ = false;
    bool dirty_ = false;
    bool sessions_dirty_ = false;
    
    std::set<std::string> sessions_dirty_files_;
    std::set<std::string> session_pending_files_;
    
    struct SessionDelta {
        size_t last_size;
        size_t pending_bytes;
        int pending_messages;
    };
    std::map<std::string, SessionDelta> session_deltas_;
    
    std::optional<std::string> last_meta_serialized_;
    
    std::shared_ptr<SQLiteVectorStorage> db_;
    std::mutex mutex_;
    
    // Watch timers
    std::unique_ptr<std::thread> watch_timer_;
    std::unique_ptr<std::thread> session_watch_timer_;
    std::unique_ptr<std::thread> interval_timer_;
    
    // ========================================================================
    // 抽象方法 (需要子类实现)
    // ========================================================================
    virtual std::string compute_provider_key() = 0;
    
    virtual core::Result<void> sync_impl(
        const std::optional<std::string>& reason,
        bool force,
        bool force_sessions,
        const std::optional<std::string>& session_file,
        const std::optional<std::function<void(const MemorySyncProgressUpdate&)>>& progress
    ) = 0;
    
    virtual int get_index_concurrency() = 0;
    
    virtual void prune_embedding_cache_if_needed() = 0;
    
    virtual core::Result<void> index_file(
        const MemoryFileEntry& entry,
        MemorySource source,
        const std::optional<std::string>& content
    ) = 0;
    
    virtual core::Result<void> index_session_file(
        const SessionFileEntry& entry,
        MemorySource source
    ) = 0;
    
    // ========================================================================
    // 核心同步方法 (对齐 manager-sync-ops.ts:170-1391)
    // ========================================================================
    
    /**
     * Ensure vector extension is loaded (对齐 manager-sync-ops.ts:170-196)
     */
    core::Result<bool> ensure_vector_ready(std::optional<int> dimensions = std::nullopt);
    
    /**
     * Load vector extension (对齐 manager-sync-ops.ts:198-224)
     */
    core::Result<bool> load_vector_extension();
    
    /**
     * Ensure vector table exists (对齐 manager-sync-ops.ts:226-240)
     */
    void ensure_vector_table(int dimensions);
    
    /**
     * Drop vector table (对齐 manager-sync-ops.ts:242-249)
     */
    void drop_vector_table();
    
    /**
     * Build source filter SQL (对齐 manager-sync-ops.ts:251-259)
     */
    struct SourceFilterResult {
        std::string sql;
        std::vector<MemorySource> params;
    };
    SourceFilterResult build_source_filter(const std::optional<std::string>& alias = std::nullopt);
    
    /**
     * Open database (对齐 manager-sync-ops.ts:261-264)
     */
    core::Result<std::shared_ptr<SQLiteVectorStorage>> open_database();
    
    /**
     * Seed embedding cache (对齐 manager-sync-ops.ts:278-311)
     */
    void seed_embedding_cache(SQLiteVectorStorage& source_db);
    
    /**
     * Start memory file watcher (对齐 manager-sync-ops.ts:313-389)
     */
    void start_memory_watcher();
    
    /**
     * Stop memory file watcher
     */
    void stop_memory_watcher();
    
    /**
     * Start session file watcher (对齐 manager-sync-ops.ts:391-485)
     */
    void start_session_watcher();
    
    /**
     * Stop session watcher
     */
    void stop_session_watcher();
    
    /**
     * Check if should reindex (对齐 manager-sync-ops.ts:487-598)
     */
    struct ReindexDecision {
        bool should_reindex;
        std::optional<std::string> reason;
        std::optional<MemoryIndexMeta> old_meta;
    };
    ReindexDecision check_should_reindex();
    
    /**
     * List memory files (对齐 manager-sync-ops.ts:600-687)
     */
    core::Result<std::vector<MemoryFileEntry>> list_memory_files();
    
    /**
     * Sync memory files (对齐 manager-sync-ops.ts:689-832)
     */
    core::Result<void> sync_memory_files(
        const std::vector<MemoryFileEntry>& entries,
        MemorySyncProgressState& progress_state
    );
    
    /**
     * Sync session files (对齐 manager-sync-ops.ts:834-1031)
     */
    core::Result<void> sync_session_files(
        bool force_sessions,
        const std::optional<std::string>& session_file,
        MemorySyncProgressState& progress_state
    );
    
    /**
     * Save index metadata (对齐 manager-sync-ops.ts:1033-1074)
     */
    void save_index_meta();
    
    /**
     * Load index metadata (对齐 manager-sync-ops.ts:1076-1107)
     */
    std::optional<MemoryIndexMeta> load_index_meta();
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

// Ignored directory names for file watching
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
