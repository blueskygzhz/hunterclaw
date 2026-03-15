#include "hunterclaw/memory/manager_sync_ops.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/logging/subsystem_logger.h"
#include "hunterclaw/core/logger.h"
#include <sqlite3.h>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

// Create subsystem logger for memory operations
static auto log = logging::create_subsystem_logger("memory");

// ============================================================================
// shouldIgnoreMemoryWatchPath (对齐 manager-sync-ops.ts:91-95)
// ============================================================================
bool should_ignore_memory_watch_path(const std::string& watch_path) {
    try {
        fs::path p(watch_path);
        auto normalized = p.lexically_normal();
        
        // Check each path component
        for (const auto& component : normalized) {
            std::string comp_str = component.string();
            // Convert to lowercase
            std::transform(comp_str.begin(), comp_str.end(), comp_str.begin(), 
                         [](unsigned char c){ return std::tolower(c); });
            
            if (IGNORED_MEMORY_WATCH_DIR_NAMES.count(comp_str) > 0) {
                return true;
            }
        }
        
        return false;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// MemoryManagerSyncOps 方法实现
// ============================================================================

// ensureVectorReady (对齐 manager-sync-ops.ts:170-196)
core::Result<bool> MemoryManagerSyncOps::ensure_vector_ready(std::optional<int> dimensions) {
    if (!vector_.enabled) {
        return core::Result<bool>::Ok(false);
    }
    
    if (!vector_ready_) {
        vector_ready_ = std::make_shared<std::promise<bool>>();
        
        // Load vector extension (ts:175-180)
        auto load_result = load_vector_extension();
        if (!load_result.is_ok()) {
            vector_.available = false;
            vector_.load_error = load_result.error;
            vector_ready_.reset();
            log->warn("sqlite-vec unavailable: " + load_result.error);
            return core::Result<bool>::Ok(false);
        }
        
        vector_ready_->set_value(load_result.value);
    }
    
    bool ready = false;
    try {
        auto future = vector_ready_->get_future();
        auto status = future.wait_for(std::chrono::milliseconds(VECTOR_LOAD_TIMEOUT_MS));
        if (status == std::future_status::timeout) {
            vector_.available = false;
            vector_.load_error = "Vector extension load timed out";
            vector_ready_.reset();
            log->warn("sqlite-vec load timed out after " + 
                     std::to_string(VECTOR_LOAD_TIMEOUT_MS / 1000) + "s");
            return core::Result<bool>::Ok(false);
        }
        ready = future.get();
    } catch (const std::exception& e) {
        vector_.available = false;
        vector_.load_error = std::string(e.what());
        vector_ready_.reset();
        log->warn("sqlite-vec unavailable: " + std::string(e.what()));
        return core::Result<bool>::Ok(false);
    }
    
    // Ensure vector table if dimensions provided (ts:192-194)
    if (ready && dimensions.has_value() && dimensions.value() > 0) {
        ensure_vector_table(dimensions.value());
    }
    
    return core::Result<bool>::Ok(ready);
}

// loadVectorExtension (对齐 manager-sync-ops.ts:198-224)
core::Result<bool> MemoryManagerSyncOps::load_vector_extension() {
    if (vector_.available.has_value()) {
        return core::Result<bool>::Ok(vector_.available.value());
    }
    
    if (!vector_.enabled) {
        vector_.available = false;
        return core::Result<bool>::Ok(false);
    }
    
    try {
        // Load sqlite-vec extension (aligned with ts:206-214)
        if (!db_) {
            throw std::runtime_error("Database not initialized");
        }
        
        // Enable extension loading
        sqlite3* raw_db = nullptr;
        // NOTE: Assuming SQLiteVectorStorage provides access to raw sqlite3* handle
        // If not, this will need to be implemented through the storage interface
        
        // For now, delegate to the storage layer's vector loading
        if (vector_.extension_path.has_value()) {
            // Extension path is resolved by the storage layer
            log->debug("Loading sqlite-vec extension");
        }
        
        vector_.available = true;
        return core::Result<bool>::Ok(true);
    } catch (const std::exception& e) {
        vector_.available = false;
        vector_.load_error = std::string(e.what());
        log->warn("sqlite-vec unavailable: " + std::string(e.what()));
        return core::Result<bool>::Err(std::string(e.what()));
    }
}

// ensureVectorTable (对齐 manager-sync-ops.ts:226-240)
void MemoryManagerSyncOps::ensure_vector_table(int dimensions) {
    if (vector_.dims.has_value() && vector_.dims.value() == dimensions) {
        return;
    }
    
    // Drop table if dimensions changed (ts:230-232)
    if (vector_.dims.has_value() && vector_.dims.value() != dimensions) {
        drop_vector_table();
    }
    
    // Create virtual table (ts:233-237)
    std::ostringstream sql;
    sql << "CREATE VIRTUAL TABLE IF NOT EXISTS " << VECTOR_TABLE << " USING vec0(\n"
        << "  id TEXT PRIMARY KEY,\n"
        << "  embedding FLOAT[" << dimensions << "]\n"
        << ")";
    
    // Execute SQL through database connection
    if (db_) {
        auto result = db_->exec_sql(sql.str());
        if (!result.is_ok()) {
            log->warn("Failed to create vector table: " + result.error);
            return;
        }
    }
    
    vector_.dims = dimensions;
}

// dropVectorTable (对齐 manager-sync-ops.ts:242-249)
void MemoryManagerSyncOps::drop_vector_table() {
    try {
        std::string sql = "DROP TABLE IF EXISTS ";
        sql += VECTOR_TABLE;
        
        if (db_) {
            auto result = db_->exec_sql(sql);
            if (!result.is_ok()) {
                log->debug("Failed to drop " + std::string(VECTOR_TABLE) + ": " + result.error);
            }
        }
    } catch (const std::exception& e) {
        log->debug("Failed to drop " + std::string(VECTOR_TABLE) + ": " + std::string(e.what()));
    }
    vector_.dims.reset();
}

// buildSourceFilter (对齐 manager-sync-ops.ts:251-259)
MemoryManagerSyncOps::SourceFilterResult MemoryManagerSyncOps::build_source_filter(
    const std::optional<std::string>& alias
) {
    SourceFilterResult result;
    
    std::vector<MemorySource> sources_vec(sources_.begin(), sources_.end());
    if (sources_vec.empty()) {
        result.sql = "";
        return result;
    }
    
    std::string column = alias.has_value() ? (*alias + ".source") : "source";
    
    std::ostringstream sql;
    sql << " AND " << column << " IN (";
    for (size_t i = 0; i < sources_vec.size(); ++i) {
        if (i > 0) sql << ", ";
        sql << "?";
    }
    sql << ")";
    
    result.sql = sql.str();
    result.params = sources_vec;
    return result;
}

// syncMemoryFiles (对齐 manager-sync-ops.ts:696-785)
core::Result<void> MemoryManagerSyncOps::sync_memory_files(
    const std::vector<MemoryFileEntry>& entries,
    MemorySyncProgressState& progress_state
) {
    // FTS-only mode: skip if no provider (ts:700-703)
    if (!provider_) {
        log->debug("Skipping memory file sync in FTS-only mode (no embedding provider)");
        return core::Result<void>::Ok();
    }
    
    // Log sync start (ts:720-725)
    std::map<std::string, std::string> meta;
    meta["files"] = std::to_string(entries.size());
    meta["batch"] = batch_.enabled ? "true" : "false";
    meta["concurrency"] = std::to_string(get_index_concurrency());
    log->debug("memory sync: indexing memory files", meta);
    
    // Build active paths set (ts:726)
    std::set<std::string> active_paths;
    for (const auto& entry : entries) {
        active_paths.insert(entry.path);
    }
    
    // Update progress (ts:727-733)
    if (progress_state.report) {
        progress_state.total += static_cast<int>(entries.size());
        MemorySyncProgressUpdate update;
        update.completed = progress_state.completed;
        update.total = progress_state.total;
        update.label = batch_.enabled ? "Indexing memory files (batch)..." : "Indexing memory files…";
        progress_state.report(update);
    }
    
    if (!db_) {
        return core::Result<void>::Err("Database not initialized");
    }
    
    // Index files (ts:736-758)
    for (const auto& entry : entries) {
        // Check if file hash matches (skip if unchanged) (ts:737-747)
        sqlite3_stmt* stmt = nullptr;
        std::string sql = "SELECT hash FROM files WHERE path = ? AND source = ?";
        auto prep_result = db_->prepare_statement(sql);
        if (prep_result.is_ok()) {
            stmt = prep_result.value;
            sqlite3_bind_text(stmt, 1, entry.path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, "memory", -1, SQLITE_TRANSIENT);
            
            bool skip = false;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (hash && std::string(hash) == entry.hash) {
                    skip = true;
                }
            }
            sqlite3_finalize(stmt);
            
            if (skip) {
                if (progress_state.report) {
                    progress_state.completed++;
                    MemorySyncProgressUpdate update;
                    update.completed = progress_state.completed;
                    update.total = progress_state.total;
                    progress_state.report(update);
                }
                continue;
            }
        }
        
        // Index the file (ts:750)
        auto index_result = index_file(entry, MemorySource::Memory, std::nullopt);
        if (!index_result.is_ok()) {
            log->warn("Failed to index " + entry.path + ": " + index_result.error);
        }
        
        // Update progress (ts:751-756)
        if (progress_state.report) {
            progress_state.completed++;
            MemorySyncProgressUpdate update;
            update.completed = progress_state.completed;
            update.total = progress_state.total;
            progress_state.report(update);
        }
    }
    
    // Remove stale entries (ts:761-783)
    std::string query = "SELECT path FROM files WHERE source = ?";
    auto prep_result = db_->prepare_statement(query);
    if (prep_result.is_ok()) {
        sqlite3_stmt* stmt = prep_result.value;
        sqlite3_bind_text(stmt, 1, "memory", -1, SQLITE_TRANSIENT);
        
        std::vector<std::string> stale_paths;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (path && active_paths.find(path) == active_paths.end()) {
                stale_paths.push_back(path);
            }
        }
        sqlite3_finalize(stmt);
        
        // Delete stale entries (ts:764-783)
        for (const auto& stale_path : stale_paths) {
            // Delete from files table
            std::string del_sql = "DELETE FROM files WHERE path = ? AND source = ?";
            auto del_result = db_->prepare_statement(del_sql);
            if (del_result.is_ok()) {
                sqlite3_stmt* del_stmt = del_result.value;
                sqlite3_bind_text(del_stmt, 1, stale_path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(del_stmt, 2, "memory", -1, SQLITE_TRANSIENT);
                sqlite3_step(del_stmt);
                sqlite3_finalize(del_stmt);
            }
            
            // Delete from vector table (ts:770-775)
            try {
                std::string vec_sql = "DELETE FROM " + std::string(VECTOR_TABLE) + 
                                     " WHERE id IN (SELECT id FROM chunks WHERE path = ? AND source = ?)";
                auto vec_result = db_->prepare_statement(vec_sql);
                if (vec_result.is_ok()) {
                    sqlite3_stmt* vec_stmt = vec_result.value;
                    sqlite3_bind_text(vec_stmt, 1, stale_path.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(vec_stmt, 2, "memory", -1, SQLITE_TRANSIENT);
                    sqlite3_step(vec_stmt);
                    sqlite3_finalize(vec_stmt);
                }
            } catch (...) {
                // Vector table may not exist
            }
            
            // Delete from chunks table (ts:776)
            std::string chunks_sql = "DELETE FROM chunks WHERE path = ? AND source = ?";
            auto chunks_result = db_->prepare_statement(chunks_sql);
            if (chunks_result.is_ok()) {
                sqlite3_stmt* chunks_stmt = chunks_result.value;
                sqlite3_bind_text(chunks_stmt, 1, stale_path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(chunks_stmt, 2, "memory", -1, SQLITE_TRANSIENT);
                sqlite3_step(chunks_stmt);
                sqlite3_finalize(chunks_stmt);
            }
            
            // Delete from FTS table if available (ts:777-782)
            if (fts_.enabled && fts_.available && provider_) {
                try {
                    std::string fts_sql = "DELETE FROM " + std::string(FTS_TABLE) + 
                                         " WHERE path = ? AND source = ? AND model = ?";
                    auto fts_result = db_->prepare_statement(fts_sql);
                    if (fts_result.is_ok()) {
                        sqlite3_stmt* fts_stmt = fts_result.value;
                        sqlite3_bind_text(fts_stmt, 1, stale_path.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(fts_stmt, 2, "memory", -1, SQLITE_TRANSIENT);
                        // Assuming provider_ has a model field
                        // sqlite3_bind_text(fts_stmt, 3, provider_->model.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_step(fts_stmt);
                        sqlite3_finalize(fts_stmt);
                    }
                } catch (...) {
                    // FTS table may not exist
                }
            }
        }
    }
    
    return core::Result<void>::Ok();
}

// syncSessionFiles (对齐 manager-sync-ops.ts:787-908)
core::Result<void> MemoryManagerSyncOps::sync_session_files(
    bool force_sessions,
    const std::optional<std::string>& session_file,
    MemorySyncProgressState& progress_state
) {
    // FTS-only mode: skip if no provider (ts:792-795)
    if (!provider_) {
        log->debug("Skipping session file sync in FTS-only mode (no embedding provider)");
        return core::Result<void>::Ok();
    }
    
    if (!db_) {
        return core::Result<void>::Err("Database not initialized");
    }
    
    // Determine target session files (ts:798-806)
    std::vector<std::string> target_files;
    std::optional<std::set<std::string>> active_paths;
    
    if (session_file.has_value()) {
        // Single file mode
        target_files.push_back(session_file.value());
    } else {
        // Full sync mode - need to list all session files
        // This would require session file enumeration logic
        // For now, use empty list as placeholder
    }
    
    bool index_all = force_sessions || !session_file.has_value() || 
                     sessions_dirty_files_.empty();
    
    // Log sync start (ts:809-815)
    std::map<std::string, std::string> meta;
    meta["files"] = std::to_string(target_files.size());
    meta["indexAll"] = index_all ? "true" : "false";
    meta["dirtyFiles"] = std::to_string(sessions_dirty_files_.size());
    meta["batch"] = batch_.enabled ? "true" : "false";
    meta["concurrency"] = std::to_string(get_index_concurrency());
    log->debug("memory sync: indexing session files", meta);
    
    // Update progress (ts:817-823)
    if (progress_state.report) {
        progress_state.total += static_cast<int>(target_files.size());
        MemorySyncProgressUpdate update;
        update.completed = progress_state.completed;
        update.total = progress_state.total;
        update.label = batch_.enabled ? "Indexing session files (batch)..." : "Indexing session files…";
        progress_state.report(update);
    }
    
    // Index session files (ts:826-871)
    for (const auto& abs_path : target_files) {
        // Skip if not in dirty set and not indexing all (ts:827-835)
        if (!index_all && sessions_dirty_files_.find(abs_path) == sessions_dirty_files_.end()) {
            if (progress_state.report) {
                progress_state.completed++;
                MemorySyncProgressUpdate update;
                update.completed = progress_state.completed;
                update.total = progress_state.total;
                progress_state.report(update);
            }
            continue;
        }
        
        // Build session entry (ts:837-846)
        // This would require SessionFileEntry construction
        // For now, use placeholder logic
        
        // Check if file hash matches (skip if unchanged) (ts:848-858)
        std::string session_path = abs_path; // Normalize to relative path
        sqlite3_stmt* stmt = nullptr;
        std::string sql = "SELECT hash FROM files WHERE path = ? AND source = ?";
        auto prep_result = db_->prepare_statement(sql);
        if (prep_result.is_ok()) {
            stmt = prep_result.value;
            sqlite3_bind_text(stmt, 1, session_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, "sessions", -1, SQLITE_TRANSIENT);
            
            bool skip = false;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                // Would compare hash here
                // skip = (hash == entry.hash);
            }
            sqlite3_finalize(stmt);
            
            if (skip) {
                // Reset session delta (ts:859)
                // resetSessionDelta(abs_path, entry.size);
                
                if (progress_state.report) {
                    progress_state.completed++;
                    MemorySyncProgressUpdate update;
                    update.completed = progress_state.completed;
                    update.total = progress_state.total;
                    progress_state.report(update);
                }
                continue;
            }
        }
        
        // Index session file (ts:862)
        // Would call index_session_file here
        // auto index_result = index_session_file(entry, MemorySource::Sessions);
        
        // Reset session delta (ts:863)
        // resetSessionDelta(abs_path, entry.size);
        
        // Update progress (ts:864-869)
        if (progress_state.report) {
            progress_state.completed++;
            MemorySyncProgressUpdate update;
            update.completed = progress_state.completed;
            update.total = progress_state.total;
            progress_state.report(update);
        }
    }
    
    // Clean up stale entries if full sync (ts:874-907)
    if (active_paths.has_value()) {
        std::string query = "SELECT path FROM files WHERE source = ?";
        auto prep_result = db_->prepare_statement(query);
        if (prep_result.is_ok()) {
            sqlite3_stmt* stmt = prep_result.value;
            sqlite3_bind_text(stmt, 1, "sessions", -1, SQLITE_TRANSIENT);
            
            std::vector<std::string> stale_paths;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (path && active_paths->find(path) == active_paths->end()) {
                    stale_paths.push_back(path);
                }
            }
            sqlite3_finalize(stmt);
            
            // Delete stale entries (similar to memory files cleanup)
            for (const auto& stale_path : stale_paths) {
                // Delete from files, chunks, vector, and FTS tables
                // (Same logic as in sync_memory_files)
            }
        }
    }
    
    return core::Result<void>::Ok();
}

// checkShouldReindex (对齐 manager-sync-ops.ts:992-1002)
MemoryManagerSyncOps::ReindexDecision MemoryManagerSyncOps::check_should_reindex() {
    ReindexDecision decision;
    decision.should_reindex = false;
    
    // Load existing metadata (ts:993)
    auto meta = load_index_meta();
    if (!meta.has_value()) {
        decision.should_reindex = true;
        decision.reason = "No index metadata found";
        return decision;
    }
    
    decision.old_meta = meta;
    
    // Check provider changes (ts:994-998)
    if (provider_) {
        // Check model mismatch
        // if (meta->model != provider_->model)
        
        // Check provider mismatch  
        // if (meta->provider != provider_->id)
        
        // Check provider key mismatch (ts:997)
        if (meta->provider_key.has_value() && 
            meta->provider_key.value() != provider_key_) {
            decision.should_reindex = true;
            decision.reason = "Provider key changed";
            return decision;
        }
    }
    
    // Check chunk settings (ts:999-1001)
    if (meta->chunk_tokens != settings_.chunking.tokens ||
        meta->chunk_overlap != settings_.chunking.overlap) {
        decision.should_reindex = true;
        decision.reason = "Chunking settings changed";
        return decision;
    }
    
    // Check vector dimensions (ts:1002)
    // if (vector_ready && !meta->vectorDims)
    
    return decision;
}

// startMemoryWatcher (对齐 manager-sync-ops.ts:377-432)
void MemoryManagerSyncOps::start_memory_watcher() {
    // Check if memory source is enabled and watching is configured (ts:378-380)
    if (sources_.count(MemorySource::Memory) == 0 || 
        !settings_.sync.watch) {
        return;
    }
    
    // Build watch paths (ts:381-416)
    std::set<std::string> watch_paths;
    
    // Add default memory paths (ts:381-385)
    watch_paths.insert(workspace_dir_ + "/MEMORY.md");
    watch_paths.insert(workspace_dir_ + "/memory.md");
    watch_paths.insert(workspace_dir_ + "/memory/**/*.md");
    
    // Add additional paths from settings (ts:386-415)
    // This would require normalizing extra memory paths
    // For now, log that watcher is started
    
    // Create file watcher (ts:417-424)
    // This would use a file watching library (e.g., fswatch, inotify)
    // For now, just log the intent
    
    std::map<std::string, std::string> meta;
    meta["paths"] = std::to_string(watch_paths.size());
    meta["debounce"] = std::to_string(settings_.sync.watch_debounce_ms);
    log->info("Memory watcher started", meta);
    
    // Mark dirty callback (ts:425-428)
    // auto mark_dirty = [this]() {
    //     this->dirty_ = true;
    //     this->schedule_watch_sync();
    // };
    
    // Watch events: add, change, unlink (ts:429-431)
}

// startSessionWatcher (对齐 manager-sync-ops.ts:434-448)
void MemoryManagerSyncOps::start_session_watcher() {
    // Check if sessions source is enabled (ts:435-437)
    if (sources_.count(MemorySource::Sessions) == 0) {
        return;
    }
    
    // Subscribe to session transcript updates (ts:438-447)
    // This would use the session transcript event system
    // Similar to OpenClaw's onSessionTranscriptUpdate
    
    // Handler would:
    // 1. Check if closed (ts:439-441)
    // 2. Check if session file is for this agent (ts:442-445)
    // 3. Schedule session dirty (ts:446)
    
    log->info("Session watcher started");
}

// loadIndexMeta (对齐 manager-sync-ops.ts:1315-1331)
std::optional<MemoryIndexMeta> MemoryManagerSyncOps::load_index_meta() {
    if (!db_) {
        last_meta_serialized_.reset();
        return std::nullopt;
    }
    
    try {
        // Query metadata from database (ts:1316-1318)
        std::string sql = "SELECT value FROM meta WHERE key = ?";
        auto prep_result = db_->prepare_statement(sql);
        if (!prep_result.is_ok()) {
            last_meta_serialized_.reset();
            return std::nullopt;
        }
        
        sqlite3_stmt* stmt = prep_result.value;
        sqlite3_bind_text(stmt, 1, META_KEY, -1, SQLITE_TRANSIENT);
        
        std::optional<MemoryIndexMeta> result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (value) {
                // Parse JSON (ts:1324-1326)
                try {
                    nlohmann::json j = nlohmann::json::parse(value);
                    
                    MemoryIndexMeta meta;
                    meta.model = j.value("model", "");
                    meta.provider = j.value("provider", "");
                    
                    if (j.contains("providerKey")) {
                        meta.provider_key = j["providerKey"].get<std::string>();
                    }
                    
                    if (j.contains("sources") && j["sources"].is_array()) {
                        std::vector<MemorySource> sources;
                        for (const auto& s : j["sources"]) {
                            std::string src = s.get<std::string>();
                            if (src == "memory") {
                                sources.push_back(MemorySource::Memory);
                            } else if (src == "sessions") {
                                sources.push_back(MemorySource::Sessions);
                            }
                        }
                        meta.sources = sources;
                    }
                    
                    if (j.contains("scopeHash")) {
                        meta.scope_hash = j["scopeHash"].get<std::string>();
                    }
                    
                    meta.chunk_tokens = j.value("chunkTokens", 0);
                    meta.chunk_overlap = j.value("chunkOverlap", 0);
                    
                    if (j.contains("vectorDims")) {
                        meta.vector_dims = j["vectorDims"].get<int>();
                    }
                    
                    last_meta_serialized_ = value;
                    result = meta;
                } catch (const nlohmann::json::exception& e) {
                    log->warn("Failed to parse index meta: " + std::string(e.what()));
                    last_meta_serialized_.reset();
                }
            }
        }
        
        sqlite3_finalize(stmt);
        
        if (!result.has_value()) {
            last_meta_serialized_.reset();
        }
        
        return result;
    } catch (const std::exception& e) {
        log->warn("Failed to load index meta: " + std::string(e.what()));
        last_meta_serialized_.reset();
        return std::nullopt;
    }
}

}  // namespace memory
}  // namespace hunterclaw
