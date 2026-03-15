#include "hunterclaw/memory/manager_sync_ops.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/session_files.h"
#include "hunterclaw/memory/chunking.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <thread>

// 严格对齐 OpenClaw src/memory/manager-sync-ops.ts

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

namespace {

// Constants (对齐 manager-sync-ops.ts:72-86)
constexpr const char* META_KEY = "memory_index_meta_v1";
constexpr const char* VECTOR_TABLE = "chunks_vec";
constexpr const char* FTS_TABLE = "chunks_fts";
constexpr const char* EMBEDDING_CACHE_TABLE = "embedding_cache";
constexpr int SESSION_DIRTY_DEBOUNCE_MS = 5000;
constexpr int SESSION_DELTA_READ_CHUNK_BYTES = 64 * 1024;
constexpr int VECTOR_LOAD_TIMEOUT_MS = 30000;

// Ignored directory names (对齐 manager-sync-ops.ts:79-87)
const std::set<std::string> IGNORED_MEMORY_WATCH_DIR_NAMES = {
    ".git", "node_modules", ".pnpm-store", ".venv", 
    "venv", ".tox", "__pycache__"
};

// 对齐 shouldIgnoreMemoryWatchPath (manager-sync-ops.ts:91-95)
bool should_ignore_memory_watch_path(const std::string& watch_path) {
    fs::path normalized = fs::path(watch_path).lexically_normal();
    for (const auto& segment : normalized) {
        std::string seg_str = segment.string();
        std::transform(seg_str.begin(), seg_str.end(), seg_str.begin(), ::tolower);
        if (IGNORED_MEMORY_WATCH_DIR_NAMES.count(seg_str) > 0) {
            return true;
        }
    }
    return false;
}

// 对齐 listMemoryFiles (internal.ts)
std::vector<std::string> list_memory_files_impl(const std::string& workspace_dir) {
    std::vector<std::string> files;
    
    // MEMORY.md / memory.md at root
    std::vector<std::string> roots = {"MEMORY.md", "memory.md"};
    for (const auto& root : roots) {
        fs::path root_path = fs::path(workspace_dir) / root;
        if (fs::exists(root_path) && fs::is_regular_file(root_path)) {
            files.push_back(root_path.string());
        }
    }
    
    // memory/**/*.md
    fs::path memory_dir = fs::path(workspace_dir) / "memory";
    if (fs::exists(memory_dir) && fs::is_directory(memory_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(memory_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".md") {
                files.push_back(entry.path().string());
            }
        }
    }
    
    return files;
}

// 对齐 buildFileEntry (internal.ts)
struct FileEntry {
    std::string path;
    std::string hash;
    size_t size;
};

FileEntry build_file_entry(const std::string& abs_path) {
    FileEntry entry;
    entry.path = abs_path;
    
    std::ifstream file(abs_path, std::ios::binary);
    if (!file) {
        return entry;
    }
    
    // Read content and compute hash
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // Simple hash (TODO: use proper hash function)
    std::hash<std::string> hasher;
    entry.hash = std::to_string(hasher(content));
    entry.size = content.size();
    
    return entry;
}

}  // anonymous namespace

// ============================================================================
// MemoryManagerSyncOps Implementation
// ============================================================================

// 对齐 ensureVectorReady (manager-sync-ops.ts:170-196)
Result<bool> MemoryManagerSyncOps::ensure_vector_ready(int dimensions) {
    if (!vector_enabled_) {
        return Result<bool>::ok(false);
    }
    
    if (!vector_ready_promise_) {
        // Start loading asynchronously
        // TODO: Implement async loading with timeout
        auto load_result = load_vector_extension();
        if (!load_result.ok()) {
            vector_available_ = false;
            vector_load_error_ = load_result.error();
            return Result<bool>::error(load_result.error());
        }
        vector_available_ = load_result.value();
    }
    
    if (vector_available_ && dimensions > 0) {
        auto table_result = ensure_vector_table(dimensions);
        if (!table_result.ok()) {
            return Result<bool>::error(table_result.error());
        }
    }
    
    return Result<bool>::ok(vector_available_);
}

// 对齐 loadVectorExtension (manager-sync-ops.ts:198-224)
Result<bool> MemoryManagerSyncOps::load_vector_extension() {
    if (vector_available_.has_value()) {
        return Result<bool>::ok(vector_available_.value());
    }
    
    if (!vector_enabled_) {
        vector_available_ = false;
        return Result<bool>::ok(false);
    }
    
    try {
        // TODO: Load sqlite-vec extension
        // For now, assume it's loaded
        vector_available_ = true;
        return Result<bool>::ok(true);
    } catch (const std::exception& e) {
        vector_available_ = false;
        vector_load_error_ = e.what();
        return Result<bool>::error(e.what());
    }
}

// 对齐 ensureVectorTable (manager-sync-ops.ts:226-240)
Result<void> MemoryManagerSyncOps::ensure_vector_table(int dimensions) {
    if (vector_dims_.has_value() && vector_dims_.value() == dimensions) {
        return Result<void>::ok();
    }
    
    if (vector_dims_.has_value() && vector_dims_.value() != dimensions) {
        auto drop_result = drop_vector_table();
        if (!drop_result.ok()) {
            return drop_result;
        }
    }
    
    // Create vector table
    std::stringstream sql;
    sql << "CREATE VIRTUAL TABLE IF NOT EXISTS " << VECTOR_TABLE << " USING vec0(\n"
        << "  id TEXT PRIMARY KEY,\n"
        << "  embedding FLOAT[" << dimensions << "]\n"
        << ")";
    
    auto exec_result = db_->execute(sql.str());
    if (!exec_result.ok()) {
        return exec_result;
    }
    
    vector_dims_ = dimensions;
    return Result<void>::ok();
}

// 对齐 dropVectorTable (manager-sync-ops.ts:242-249)
Result<void> MemoryManagerSyncOps::drop_vector_table() {
    std::string sql = "DROP TABLE IF EXISTS " + std::string(VECTOR_TABLE);
    auto result = db_->execute(sql);
    if (!result.ok()) {
        // Log but don't fail
        // log.debug("Failed to drop vector table: " + result.error());
    }
    vector_dims_.reset();
    return Result<void>::ok();
}

// 对齐 buildSourceFilter (manager-sync-ops.ts:251-259)
SourceFilter MemoryManagerSyncOps::build_source_filter(
    const std::vector<std::string>& sources,
    const std::string& alias
) {
    SourceFilter filter;
    
    if (sources.empty()) {
        filter.sql = "";
        return filter;
    }
    
    std::string column = alias.empty() ? "source" : alias + ".source";
    std::vector<std::string> placeholders(sources.size(), "?");
    
    std::stringstream sql;
    sql << " AND " << column << " IN (";
    for (size_t i = 0; i < placeholders.size(); ++i) {
        if (i > 0) sql << ", ";
        sql << placeholders[i];
    }
    sql << ")";
    
    filter.sql = sql.str();
    filter.params = sources;
    return filter;
}

// 对齐 syncMemoryFiles (manager-sync-ops.ts:696-785)
Result<void> MemoryManagerSyncOps::sync_memory_files(
    bool needs_full_reindex,
    SyncProgressCallback progress_cb
) {
    // FTS-only mode: skip embedding sync (no provider)
    if (!provider_) {
        // log.debug("Skipping memory file sync in FTS-only mode");
        return Result<void>::ok();
    }
    
    // List all memory files
    auto files = list_memory_files_impl(workspace_dir_);
    
    // Build file entries with hash
    std::vector<FileEntry> file_entries;
    for (const auto& file : files) {
        auto entry = build_file_entry(file);
        if (!entry.hash.empty()) {
            file_entries.push_back(entry);
        }
    }
    
    // Track active paths for cleanup
    std::set<std::string> active_paths;
    for (const auto& entry : file_entries) {
        active_paths.insert(entry.path);
    }
    
    // Progress tracking
    int total = static_cast<int>(file_entries.size());
    int completed = 0;
    
    if (progress_cb) {
        progress_cb(completed, total, "Indexing memory files...");
    }
    
    // Index each file
    for (const auto& entry : file_entries) {
        // Check if file needs reindexing
        if (!needs_full_reindex) {
            auto query = db_->prepare(
                "SELECT hash FROM files WHERE path = ? AND source = ?"
            );
            if (query.ok()) {
                // Execute query and check hash
                // TODO: Implement query execution
                // If hash matches, skip
            }
        }
        
        // Index file
        // auto index_result = index_file(entry, "memory");
        // if (!index_result.ok()) {
        //     return index_result;
        // }
        
        completed++;
        if (progress_cb) {
            progress_cb(completed, total, "");
        }
    }
    
    // Clean up stale entries
    auto stale_query = db_->prepare(
        "SELECT path FROM files WHERE source = ?"
    );
    if (stale_query.ok()) {
        // Execute query and get stale paths
        // TODO: Implement query execution
        // For each stale path not in active_paths:
        //   - DELETE FROM files
        //   - DELETE FROM chunks_vec
        //   - DELETE FROM chunks
        //   - DELETE FROM chunks_fts
    }
    
    return Result<void>::ok();
}

// 对齐 syncSessionFiles (manager-sync-ops.ts:787-906)
Result<void> MemoryManagerSyncOps::sync_session_files(
    bool needs_full_reindex,
    const std::vector<std::string>& target_session_files,
    SyncProgressCallback progress_cb
) {
    // FTS-only mode: skip embedding sync (no provider)
    if (!provider_) {
        // log.debug("Skipping session file sync in FTS-only mode");
        return Result<void>::ok();
    }
    
    // List session files
    std::vector<std::string> files;
    if (needs_full_reindex || target_session_files.empty()) {
        files = SessionFiles::list_session_files_for_agent(agent_id_);
    } else {
        files = target_session_files;
    }
    
    // Track active paths
    std::set<std::string> active_paths;
    if (needs_full_reindex) {
        for (const auto& file : files) {
            active_paths.insert(SessionFiles::session_path_for_file(file));
        }
    }
    
    // Progress tracking
    int total = static_cast<int>(files.size());
    int completed = 0;
    
    if (progress_cb) {
        progress_cb(completed, total, "Indexing session files...");
    }
    
    // Index each session file
    for (const auto& abs_path : files) {
        // Check if file needs reindexing
        if (!needs_full_reindex && 
            sessions_dirty_files_.count(abs_path) == 0) {
            completed++;
            if (progress_cb) {
                progress_cb(completed, total, "");
            }
            continue;
        }
        
        // Build session entry
        auto entry_result = SessionFiles::build_session_entry(abs_path);
        if (!entry_result.ok() || !entry_result.value().has_value()) {
            completed++;
            if (progress_cb) {
                progress_cb(completed, total, "");
            }
            continue;
        }
        
        auto entry = entry_result.value().value();
        
        // Check hash
        if (!needs_full_reindex) {
            auto query = db_->prepare(
                "SELECT hash FROM files WHERE path = ? AND source = ?"
            );
            // TODO: Execute and check hash
        }
        
        // Index file
        // auto index_result = index_file(entry, "sessions", entry.content);
        // if (!index_result.ok()) {
        //     return index_result;
        // }
        
        // Reset session delta
        reset_session_delta(abs_path, entry.size);
        
        completed++;
        if (progress_cb) {
            progress_cb(completed, total, "");
        }
    }
    
    // Clean up stale entries (only for full reindex)
    if (!active_paths.empty()) {
        auto stale_query = db_->prepare(
            "SELECT path FROM files WHERE source = ?"
        );
        // TODO: Execute and cleanup
    }
    
    // Clear dirty flags
    if (target_session_files.empty()) {
        sessions_dirty_files_.clear();
    } else {
        for (const auto& file : target_session_files) {
            sessions_dirty_files_.erase(file);
        }
    }
    sessions_dirty_ = !sessions_dirty_files_.empty();
    
    return Result<void>::ok();
}

// 对齐 checkShouldReindex (manager-sync-ops.ts - auto_reindex logic)
Result<bool> MemoryManagerSyncOps::check_should_reindex(
    const std::string& saved_provider,
    const std::string& saved_model,
    const std::string& saved_provider_key
) {
    // Check if provider/model/key changed
    if (!provider_) {
        return Result<bool>::ok(false);
    }
    
    std::string current_provider = provider_->provider;
    std::string current_model = provider_->model;
    std::string current_key = provider_key_.value_or("");
    
    bool provider_changed = (current_provider != saved_provider);
    bool model_changed = (current_model != saved_model);
    bool key_changed = (current_key != saved_provider_key);
    
    if (provider_changed || model_changed || key_changed) {
        return Result<bool>::ok(true);
    }
    
    return Result<bool>::ok(false);
}

// 对齐 startMemoryWatcher (manager-sync-ops.ts:377-432)
Result<void> MemoryManagerSyncOps::start_memory_watcher() {
    if (!sources_.count("memory") || !watch_enabled_ || watcher_active_) {
        return Result<void>::ok();
    }
    
    // Build watch paths
    std::vector<std::string> watch_paths;
    watch_paths.push_back(workspace_dir_ + "/MEMORY.md");
    watch_paths.push_back(workspace_dir_ + "/memory.md");
    watch_paths.push_back(workspace_dir_ + "/memory/**/*.md");
    
    // TODO: Add extraPaths and multimodal extensions
    
    // Start watching (platform-specific)
    // On Linux: inotify
    // On macOS: FSEvents
    // On Windows: FindFirstChangeNotification
    
    watcher_active_ = true;
    return Result<void>::ok();
}

// 对齐 startSessionWatcher (manager-sync-ops.ts:434-448)
Result<void> MemoryManagerSyncOps::start_session_watcher() {
    if (!sources_.count("sessions") || session_watch_active_) {
        return Result<void>::ok();
    }
    
    // Subscribe to session transcript updates
    // TODO: Implement session event listener
    
    session_watch_active_ = true;
    return Result<void>::ok();
}

// 对齐 saveIndexMeta (manager-sync-ops.ts - metadata persistence)
Result<void> MemoryManagerSyncOps::save_index_meta() {
    if (!provider_) {
        return Result<void>::ok();
    }
    
    // Build metadata JSON
    std::stringstream meta_json;
    meta_json << "{\n";
    meta_json << "  \"provider\": \"" << provider_->provider << "\",\n";
    meta_json << "  \"model\": \"" << provider_->model << "\",\n";
    meta_json << "  \"providerKey\": \"" << provider_key_.value_or("") << "\",\n";
    meta_json << "  \"chunkTokens\": " << chunk_tokens_ << ",\n";
    meta_json << "  \"chunkOverlap\": " << chunk_overlap_ << ",\n";
    meta_json << "  \"vectorDims\": " << (vector_dims_.has_value() ? std::to_string(vector_dims_.value()) : "null") << "\n";
    meta_json << "}";
    
    // Save to DB
    std::string meta_str = meta_json.str();
    auto stmt = db_->prepare(
        "INSERT OR REPLACE INTO metadata (key, value) VALUES (?, ?)"
    );
    if (!stmt.ok()) {
        return Result<void>::error("Failed to prepare metadata insert");
    }
    
    // TODO: Execute statement with META_KEY and meta_str
    
    return Result<void>::ok();
}

// 对齐 loadIndexMeta (manager-sync-ops.ts - metadata loading)
Result<IndexMetadata> MemoryManagerSyncOps::load_index_meta() {
    auto stmt = db_->prepare(
        "SELECT value FROM metadata WHERE key = ?"
    );
    if (!stmt.ok()) {
        return Result<IndexMetadata>::error("Failed to prepare metadata query");
    }
    
    // TODO: Execute and parse JSON
    IndexMetadata meta;
    meta.provider = "";
    meta.model = "";
    meta.provider_key = "";
    meta.chunk_tokens = chunk_tokens_;
    meta.chunk_overlap = chunk_overlap_;
    
    return Result<IndexMetadata>::ok(meta);
}

// Helper: Reset session delta (对齐 manager-sync-ops.ts:596-604)
void MemoryManagerSyncOps::reset_session_delta(
    const std::string& abs_path,
    size_t size
) {
    auto it = session_deltas_.find(abs_path);
    if (it == session_deltas_.end()) {
        return;
    }
    
    it->second.last_size = size;
    it->second.pending_bytes = 0;
    it->second.pending_messages = 0;
}

// Helper: Check if session file belongs to agent (对齐 manager-sync-ops.ts:606-614)
bool MemoryManagerSyncOps::is_session_file_for_agent(
    const std::string& session_file
) const {
    if (session_file.empty()) {
        return false;
    }
    
    // Get sessions directory for agent
    std::string sessions_dir = get_sessions_dir_for_agent(agent_id_);
    
    // Check if file is under sessions directory
    fs::path resolved_file = fs::absolute(session_file);
    fs::path resolved_dir = fs::absolute(sessions_dir);
    
    return resolved_file.string().find(resolved_dir.string() + "/") == 0;
}

// Helper: Get sessions directory (对齐 resolveSessionTranscriptsDirForAgent)
std::string MemoryManagerSyncOps::get_sessions_dir_for_agent(
    const std::string& agent_id
) const {
    // TODO: Implement proper path resolution
    return workspace_dir_ + "/.claw/sessions/" + agent_id;
}

}  // namespace memory
}  // namespace hunterclaw
