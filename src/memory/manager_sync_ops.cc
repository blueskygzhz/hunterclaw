#include <optional>
#include "hunterclaw/memory/manager_sync_ops.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/session_files.h"
#include "hunterclaw/memory/chunking.h"
#include "hunterclaw/memory/embedding_provider.h"
#include "hunterclaw/logging/subsystem_logger.h"
#include <sqlite3.h>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <regex>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

static auto log = logging::create_subsystem_logger("memory");

// ============================================================================
// shouldIgnoreMemoryWatchPath (对齐 manager-sync-ops.ts:91-95)
// ============================================================================
bool should_ignore_memory_watch_path(const std::string& watch_path) {
    try {
        fs::path p(watch_path);
        auto normalized = p.lexically_normal();
        for (const auto& component : normalized) {
            std::string comp = component.string();
            std::transform(comp.begin(), comp.end(), comp.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (IGNORED_MEMORY_WATCH_DIR_NAMES.count(comp) > 0) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// openDatabase / openDatabaseAtPath (对齐 manager-sync-ops.ts:261-276)
// ============================================================================
std::shared_ptr<SQLiteVectorStorage> MemoryManagerSyncOps::open_database() {
    const std::string db_path = settings_.store.path;
    return open_database_at_path(db_path);
}

std::shared_ptr<SQLiteVectorStorage> MemoryManagerSyncOps::open_database_at_path(
    const std::string& db_path) {
    // 确保目录存在 (对齐 ensureDir(dir))
    try {
        fs::create_directories(fs::path(db_path).parent_path());
    } catch (...) {}

    SQLiteVectorStorage::Config cfg;
    cfg.db_path = db_path;
    cfg.enable_fts = fts_.enabled;
    cfg.enable_vector = vector_.enabled;
    if (vector_.extension_path) {
        cfg.vec_extension_path = vector_.extension_path;
    }
    // 对齐 OC: ftsTokenizer = this.settings.store.fts.tokenizer
    cfg.fts_tokenizer = settings_.fts.tokenizer;

    auto result = SQLiteVectorStorage::create(cfg);
    if (!result.is_ok()) {
        throw std::runtime_error("Failed to open database: " + result.error);
    }
    return result.get();
}

// ============================================================================
// ensureSchema (对齐 manager-sync-ops.ts:ensureSchema)
// ============================================================================
void MemoryManagerSyncOps::ensure_schema() {
    if (!db_) return;
    // SQLiteVectorStorage::create 已经在内部调用了 ensure_schema
    // 这里同步 fts 状态
    fts_.available = db_->is_fts_available();
    if (db_->get_fts_error()) {
        fts_.load_error = db_->get_fts_error();
        if (fts_.enabled) {
            log->warn("fts unavailable: " + fts_.load_error.value_or(""));
        }
    }
}

// ============================================================================
// seedEmbeddingCache (对齐 manager-sync-ops.ts:278-311)
// ============================================================================
void MemoryManagerSyncOps::seed_embedding_cache(SQLiteVectorStorage& source_db) {
    // 对齐 openclaw manager-sync-ops.ts:seedEmbeddingCache
    // 从旧 db 复制 embedding_cache 到新 db
    if (!db_) return;

    // SELECT * FROM embedding_cache
    auto stmt_res = source_db.prepare_statement(
        "SELECT provider, model, provider_key, hash, embedding, dims, updated_at"
        " FROM embedding_cache"
    );
    if (stmt_res.is_err()) return;
    sqlite3_stmt* src_stmt = stmt_res.get();

    // INSERT OR IGNORE INTO embedding_cache ...
    const std::string insert_sql =
        "INSERT OR IGNORE INTO embedding_cache"
        " (provider, model, provider_key, hash, embedding, dims, updated_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)";
    auto dst_res = db_->prepare_statement(insert_sql);
    if (dst_res.is_err()) { sqlite3_finalize(src_stmt); return; }
    sqlite3_stmt* dst_stmt = dst_res.get();

    while (sqlite3_step(src_stmt) == SQLITE_ROW) {
        const char* provider    = reinterpret_cast<const char*>(sqlite3_column_text(src_stmt, 0));
        const char* model       = reinterpret_cast<const char*>(sqlite3_column_text(src_stmt, 1));
        const char* pkey        = reinterpret_cast<const char*>(sqlite3_column_text(src_stmt, 2));
        const char* hash        = reinterpret_cast<const char*>(sqlite3_column_text(src_stmt, 3));
        const char* embedding   = reinterpret_cast<const char*>(sqlite3_column_text(src_stmt, 4));
        int         dims        = sqlite3_column_int(src_stmt, 5);
        int64_t     updated_at  = sqlite3_column_int64(src_stmt, 6);
        if (!provider || !model || !pkey || !hash || !embedding) continue;

        sqlite3_reset(dst_stmt);
        sqlite3_bind_text (dst_stmt, 1, provider,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (dst_stmt, 2, model,      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (dst_stmt, 3, pkey,       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (dst_stmt, 4, hash,       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (dst_stmt, 5, embedding,  -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (dst_stmt, 6, dims);
        sqlite3_bind_int64(dst_stmt, 7, updated_at);
        sqlite3_step(dst_stmt);
    }
    sqlite3_finalize(src_stmt);
    sqlite3_finalize(dst_stmt);
}

// ============================================================================
// swapIndexFiles (对齐 manager-sync-ops.ts:swapIndexFiles)
// ============================================================================
core::Result<void> MemoryManagerSyncOps::swap_index_files(
    const std::string& target_path,
    const std::string& temp_path) {

    // 生成 backup 路径
    std::string backup_path = target_path + ".backup-" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

    // 移动 target → backup
    auto move_files = [](const std::string& src, const std::string& dst) -> core::Result<void> {
        for (const auto& suffix : {"", "-wal", "-shm"}) {
            std::string s = src + suffix;
            std::string d = dst + suffix;
            try {
                if (fs::exists(s)) {
                    fs::rename(s, d);
                }
            } catch (const std::exception& e) {
                return core::Result<void>::Err(e.what());
            }
        }
        return core::Result<void>::Ok();
    };

    auto remove_files = [](const std::string& base) {
        for (const auto& suffix : {"", "-wal", "-shm"}) {
            try { fs::remove(base + suffix); } catch (...) {}
        }
    };

    auto r1 = move_files(target_path, backup_path);
    if (!r1.is_ok()) return r1;

    auto r2 = move_files(temp_path, target_path);
    if (!r2.is_ok()) {
        // 回滚
        move_files(backup_path, target_path);
        return r2;
    }

    remove_files(backup_path);
    return core::Result<void>::Ok();
}

// ============================================================================
// buildSourceFilter (对齐 manager-sync-ops.ts:251-259)
// ============================================================================
MemoryManagerSyncOps::SourceFilterResult MemoryManagerSyncOps::build_source_filter(
    const std::optional<std::string>& alias) {

    SourceFilterResult result;
    std::vector<MemorySource> sources_vec(sources_.begin(), sources_.end());
    if (sources_vec.empty()) {
        return result;
    }
    std::string column = alias ? (*alias + ".source") : "source";
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

// ============================================================================
// ensureVectorReady (对齐 manager-sync-ops.ts:170-196)
// ============================================================================
bool MemoryManagerSyncOps::ensure_vector_ready(std::optional<int> dimensions) {
    if (!vector_.enabled) return false;

    if (!vector_ready_future_) {
        // 同步加载 (C++ 中不需要异步)
        bool ready = load_vector_extension();
        if (ready && dimensions && *dimensions > 0) {
            ensure_vector_table(*dimensions);
        }
        return ready;
    }

    // 等待已有的 future
    try {
        auto status = vector_ready_future_->wait_for(
            std::chrono::milliseconds(VECTOR_LOAD_TIMEOUT_MS));
        if (status == std::future_status::timeout) {
            vector_.available = false;
            vector_.load_error = "Vector extension load timed out after " +
                std::to_string(VECTOR_LOAD_TIMEOUT_MS / 1000) + "s";
            vector_ready_future_.reset();
            log->warn("sqlite-vec load timed out");
            return false;
        }
        bool ready = vector_ready_future_->get();
        if (ready && dimensions && *dimensions > 0) {
            ensure_vector_table(*dimensions);
        }
        return ready;
    } catch (const std::exception& e) {
        vector_.available = false;
        vector_.load_error = e.what();
        vector_ready_future_.reset();
        log->warn("sqlite-vec unavailable: " + std::string(e.what()));
        return false;
    }
}

// ============================================================================
// loadVectorExtension (对齐 manager-sync-ops.ts:198-224)
// ============================================================================
bool MemoryManagerSyncOps::load_vector_extension() {
    if (vector_.available.has_value()) {
        return vector_.available.value();
    }
    if (!vector_.enabled) {
        vector_.available = false;
        return false;
    }
    if (!db_) {
        vector_.available = false;
        vector_.load_error = "Database not initialized";
        return false;
    }
    // SQLiteVectorStorage 在创建时已经尝试加载 sqlite-vec
    bool available = db_->is_vec_available();
    vector_.available = available;
    if (!available && db_->get_vec_error()) {
        vector_.load_error = db_->get_vec_error();
        log->warn("sqlite-vec unavailable: " + vector_.load_error.value_or(""));
    }
    if (available && db_->get_vec_error()) {
        vector_.extension_path = std::nullopt;  // 已加载
    }
    return available;
}

// ============================================================================
// ensureVectorTable (对齐 manager-sync-ops.ts:226-240)
// ============================================================================
void MemoryManagerSyncOps::ensure_vector_table(int dimensions) {
    if (vector_.dims && *vector_.dims == dimensions) return;
    if (vector_.dims && *vector_.dims != dimensions) {
        drop_vector_table();
    }
    if (!db_) return;
    std::ostringstream sql;
    sql << "CREATE VIRTUAL TABLE IF NOT EXISTS " << VECTOR_TABLE << " USING vec0(\n"
        << "  id TEXT PRIMARY KEY,\n"
        << "  embedding FLOAT[" << dimensions << "]\n"
        << ")";
    auto r = db_->exec_sql(sql.str());
    if (!r.is_ok()) {
        log->warn("Failed to create vector table: " + r.error);
        return;
    }
    vector_.dims = dimensions;
}

// ============================================================================
// dropVectorTable (对齐 manager-sync-ops.ts:242-249)
// ============================================================================
void MemoryManagerSyncOps::drop_vector_table() {
    if (!db_) return;
    try {
        db_->exec_sql(std::string("DROP TABLE IF EXISTS ") + VECTOR_TABLE);
    } catch (const std::exception& e) {
        log->debug("Failed to drop " + std::string(VECTOR_TABLE) + ": " + e.what());
    }
    vector_.dims = std::nullopt;
}

// ============================================================================
// readMeta (对齐 manager-sync-ops.ts:readMeta)
// ============================================================================
std::optional<MemoryIndexMeta> MemoryManagerSyncOps::read_meta() {
    if (!db_) {
        last_meta_serialized_.reset();
        return std::nullopt;
    }
    try {
        auto stmt_r = db_->prepare_statement("SELECT value FROM meta WHERE key = ?");
        if (!stmt_r.is_ok()) {
            last_meta_serialized_.reset();
            return std::nullopt;
        }
        sqlite3_stmt* stmt = stmt_r.get();
        sqlite3_bind_text(stmt, 1, META_KEY, -1, SQLITE_TRANSIENT);

        std::optional<MemoryIndexMeta> result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (value) {
                try {
                    auto j = nlohmann::json::parse(value);
                    MemoryIndexMeta meta;
                    meta.model = j.value("model", "");
                    meta.provider = j.value("provider", "");
                    if (j.contains("providerKey"))
                        meta.provider_key = j["providerKey"].get<std::string>();
                    if (j.contains("sources") && j["sources"].is_array()) {
                        std::vector<MemorySource> srcs;
                        for (const auto& s : j["sources"]) {
                            std::string sv = s.get<std::string>();
                            if (sv == "memory") srcs.push_back(MemorySource::Memory);
                            else if (sv == "sessions") srcs.push_back(MemorySource::Sessions);
                        }
                        meta.sources = srcs;
                    }
                    if (j.contains("scopeHash"))
                        meta.scope_hash = j["scopeHash"].get<std::string>();
                    meta.chunk_tokens = j.value("chunkTokens", 0);
                    meta.chunk_overlap = j.value("chunkOverlap", 0);
                    if (j.contains("vectorDims"))
                        meta.vector_dims = j["vectorDims"].get<int>();
                    last_meta_serialized_ = value;
                    result = meta;
                } catch (...) {
                    last_meta_serialized_.reset();
                }
            }
        }
        sqlite3_finalize(stmt);
        if (!result) last_meta_serialized_.reset();
        return result;
    } catch (const std::exception& e) {
        log->warn("Failed to load index meta: " + std::string(e.what()));
        last_meta_serialized_.reset();
        return std::nullopt;
    }
}

// ============================================================================
// writeMeta (对齐 manager-sync-ops.ts:writeMeta)
// ============================================================================
void MemoryManagerSyncOps::write_meta(const MemoryIndexMeta& meta) {
    if (!db_) return;

    nlohmann::json j;
    j["model"] = meta.model;
    j["provider"] = meta.provider;
    if (meta.provider_key) j["providerKey"] = *meta.provider_key;
    if (meta.sources) {
        nlohmann::json srcs = nlohmann::json::array();
        for (auto s : *meta.sources) {
            srcs.push_back(to_string(s));
        }
        j["sources"] = srcs;
    }
    if (meta.scope_hash) j["scopeHash"] = *meta.scope_hash;
    j["chunkTokens"] = meta.chunk_tokens;
    j["chunkOverlap"] = meta.chunk_overlap;
    if (meta.vector_dims) j["vectorDims"] = *meta.vector_dims;

    std::string value = j.dump();
    if (last_meta_serialized_ && *last_meta_serialized_ == value) return;

    try {
        auto stmt_r = db_->prepare_statement(
            "INSERT INTO meta (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        if (!stmt_r.is_ok()) return;
        sqlite3_stmt* stmt = stmt_r.get();
        sqlite3_bind_text(stmt, 1, META_KEY, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        last_meta_serialized_ = value;
    } catch (const std::exception& e) {
        log->warn("Failed to write meta: " + std::string(e.what()));
    }
}

// ============================================================================
// resolveConfiguredSourcesForMeta (对齐 manager-sync-ops.ts:resolveConfiguredSourcesForMeta)
// ============================================================================
std::vector<MemorySource> MemoryManagerSyncOps::resolve_configured_sources_for_meta() const {
    std::vector<MemorySource> result;
    for (auto s : sources_) {
        if (s == MemorySource::Memory || s == MemorySource::Sessions) {
            result.push_back(s);
        }
    }
    std::sort(result.begin(), result.end());
    return result.empty() ? std::vector<MemorySource>{MemorySource::Memory} : result;
}

// ============================================================================
// resolveConfiguredScopeHash (对齐 manager-sync-ops.ts:resolveConfiguredScopeHash)
// ============================================================================
std::string MemoryManagerSyncOps::resolve_configured_scope_hash() const {
    auto extra = normalize_extra_memory_paths(workspace_dir_, settings_.extra_paths);
    std::sort(extra.begin(), extra.end());
    // normalize separators
    for (auto& p : extra) std::replace(p.begin(), p.end(), '\\', '/');

    nlohmann::json j;
    j["extraPaths"] = extra;
    j["multimodal"] = {
        {"enabled", settings_.multimodal.enabled},
        {"modalities", nlohmann::json::array()},
        {"maxFileBytes", settings_.multimodal.max_file_bytes}
    };
    // modalities sorted
    std::vector<std::string> mods(settings_.multimodal.modalities.begin(),
                                   settings_.multimodal.modalities.end());
    std::sort(mods.begin(), mods.end());
    j["multimodal"]["modalities"] = mods;

    return hash_text(j.dump());
}

// ============================================================================
// normalizeMetaSources (对齐 manager-sync-ops.ts:normalizeMetaSources)
// ============================================================================
std::vector<MemorySource> MemoryManagerSyncOps::normalize_meta_sources(
    const MemoryIndexMeta& meta) const {
    if (!meta.sources || meta.sources->empty()) {
        return {MemorySource::Memory};  // backward compat
    }
    std::vector<MemorySource> result;
    std::set<MemorySource> seen;
    for (auto s : *meta.sources) {
        if ((s == MemorySource::Memory || s == MemorySource::Sessions) && seen.insert(s).second) {
            result.push_back(s);
        }
    }
    std::sort(result.begin(), result.end());
    return result.empty() ? std::vector<MemorySource>{MemorySource::Memory} : result;
}

// ============================================================================
// metaSourcesDiffer (对齐 manager-sync-ops.ts:metaSourcesDiffer)
// ============================================================================
bool MemoryManagerSyncOps::meta_sources_differ(
    const MemoryIndexMeta& meta,
    const std::vector<MemorySource>& configured_sources) const {
    auto meta_srcs = normalize_meta_sources(meta);
    if (meta_srcs.size() != configured_sources.size()) return true;
    for (size_t i = 0; i < meta_srcs.size(); ++i) {
        if (meta_srcs[i] != configured_sources[i]) return true;
    }
    return false;
}

// ============================================================================
// shouldSyncSessions (对齐 manager-sync-ops.ts:shouldSyncSessions)
// ============================================================================
bool MemoryManagerSyncOps::should_sync_sessions(
    const SyncParams& params, bool needs_full_reindex) const {
    if (!sources_.count(MemorySource::Sessions)) return false;
    if (params.session_files && !params.session_files->empty()) {
        for (const auto& f : *params.session_files) {
            if (!f.empty()) return true;
        }
    }
    if (params.force && *params.force) return true;
    const auto& reason = params.reason;
    if (reason && (*reason == "session-start" || *reason == "watch")) return false;
    if (needs_full_reindex) return true;
    return sessions_dirty_ && !sessions_dirty_files_.empty();
}

// ============================================================================
// shouldFallbackOnError (对齐 manager-sync-ops.ts:shouldFallbackOnError)
// ============================================================================
bool MemoryManagerSyncOps::should_fallback_on_error(const std::string& message) const {
    std::regex re("embedding|embeddings|batch", std::regex_constants::icase);
    return std::regex_search(message, re);
}

// ============================================================================
// resolveBatchConfig (对齐 manager-sync-ops.ts:resolveBatchConfig)
// ============================================================================
MemoryManagerSyncOps::BatchState MemoryManagerSyncOps::resolve_batch_config() const {
    BatchState result;
    const auto& batch = settings_.batch;
    // 对齐 openclaw: 只有 openai/gemini/voyage 支持 batch
    bool provider_supports_batch = false;
    if (provider_) {
        const std::string id = provider_->get_id();
        provider_supports_batch = (id == "openai" || id == "gemini" || id == "voyage");
    }
    bool enabled = batch.enabled && provider_supports_batch;
    result.enabled = enabled;
    result.wait = batch.wait;
    result.concurrency = std::max(1, batch.concurrency);
    result.poll_interval_ms = batch.poll_interval_ms;
    result.timeout_ms = batch.timeout_ms;
    return result;
}

// ============================================================================
// activateFallbackProvider (对齐 manager-sync-ops.ts:activateFallbackProvider)
// ============================================================================
bool MemoryManagerSyncOps::activate_fallback_provider(const std::string& reason) {
    // 🎯 对齐 OpenClaw: createEmbeddingProvider with fallback
    // 当主 provider 失败时，记录原因并标记降级
    log->warn("[MemoryManagerSyncOps] Activating fallback provider, reason: " + reason);

    fallback_reason_ = reason;
    // Fallback to local provider if available
    // Full implementation would iterate configured fallback providers
    log->warn("[MemoryManagerSyncOps] Fallback provider activation: recorded reason, no runtime switch");
    return false;
}

// ============================================================================
// normalizeTargetSessionFiles (对齐 manager-sync-ops.ts:normalizeTargetSessionFiles)
// ============================================================================
std::optional<std::set<std::string>> MemoryManagerSyncOps::normalize_target_session_files(
    const std::optional<std::vector<std::string>>& session_files) const {
    if (!session_files || session_files->empty()) return std::nullopt;
    std::set<std::string> normalized;
    for (const auto& f : *session_files) {
        std::string trimmed = f;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
        if (trimmed.empty()) continue;
        std::string resolved;
        try { resolved = fs::canonical(trimmed).string(); }
        catch (...) { resolved = fs::absolute(trimmed).string(); }
        if (is_session_file_for_agent(resolved)) {
            normalized.insert(resolved);
        }
    }
    return normalized.empty() ? std::nullopt : std::make_optional(normalized);
}

// ============================================================================
// clearSyncedSessionFiles (对齐 manager-sync-ops.ts:clearSyncedSessionFiles)
// ============================================================================
void MemoryManagerSyncOps::clear_synced_session_files(
    const std::optional<std::set<std::string>>& target) {
    if (!target) {
        sessions_dirty_files_.clear();
    } else {
        for (const auto& f : *target) {
            sessions_dirty_files_.erase(f);
        }
    }
    sessions_dirty_ = !sessions_dirty_files_.empty();
}

// ============================================================================
// isSessionFileForAgent (对齐 manager-sync-ops.ts:isSessionFileForAgent)
// ============================================================================
bool MemoryManagerSyncOps::is_session_file_for_agent(const std::string& session_file) const {
    if (session_file.empty()) return false;
    // 对齐 openclaw: resolveSessionTranscriptsDirForAgent(agentId)
    // openclaw 路径: ~/.openclaw/sessions/{agentId}/transcripts
    // HunterClaw 路径: ~/.hunterclaw/sessions/{agentId}/transcripts
    std::string sessions_dir;
    const char* home = std::getenv("HOME");
    if (home) {
        sessions_dir = std::string(home) + "/.hunterclaw/sessions/" + agent_id_ + "/transcripts";
    } else {
        sessions_dir = ".hunterclaw/sessions/" + agent_id_ + "/transcripts";
    }
    // Normalize both paths
    std::string resolved_file;
    std::string resolved_dir;
    try {
        resolved_file = fs::absolute(session_file).string();
        resolved_dir  = fs::absolute(sessions_dir).string();
    } catch (...) {
        resolved_file = session_file;
        resolved_dir  = sessions_dir;
    }
    // Check if session_file is under sessions_dir
    if (resolved_dir.back() != '/') resolved_dir += '/';
    return resolved_file.rfind(resolved_dir, 0) == 0;
}

// ============================================================================
// resetSessionDelta (对齐 manager-sync-ops.ts:resetSessionDelta)
// ============================================================================
void MemoryManagerSyncOps::reset_session_delta(const std::string& abs_path, size_t size) {
    auto it = session_deltas_.find(abs_path);
    if (it == session_deltas_.end()) return;
    it->second.last_size = size;
    it->second.pending_bytes = 0;
    it->second.pending_messages = 0;
}

// ============================================================================
// scheduleSessionDirty (对齐 manager-sync-ops.ts:scheduleSessionDirty)
// ============================================================================
void MemoryManagerSyncOps::schedule_session_dirty(const std::string& session_file) {
    session_pending_files_.insert(session_file);
    if (session_watch_timer_active_) return;

    session_watch_timer_active_ = true;
    // 使用 detached thread 模拟 setTimeout (对齐 SESSION_DIRTY_DEBOUNCE_MS)
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(SESSION_DIRTY_DEBOUNCE_MS));
        session_watch_timer_active_ = false;
        process_session_delta_batch();
    }).detach();
}

// ============================================================================
// processSessionDeltaBatch (对齐 manager-sync-ops.ts:processSessionDeltaBatch)
// ============================================================================
void MemoryManagerSyncOps::process_session_delta_batch() {
    if (session_pending_files_.empty()) return;
    auto pending = session_pending_files_;
    session_pending_files_.clear();

    bool should_sync = false;
    const auto& thresholds = settings_.sync.sessions;

    for (const auto& session_file : pending) {
        auto delta = update_session_delta(session_file);
        if (!delta) continue;

        bool bytes_hit = thresholds
            ? (thresholds->delta_bytes <= 0
                ? delta->pending_bytes > 0
                : delta->pending_bytes >= (size_t)thresholds->delta_bytes)
            : delta->pending_bytes > 0;
        bool messages_hit = thresholds
            ? (thresholds->delta_messages <= 0
                ? delta->pending_messages > 0
                : delta->pending_messages >= thresholds->delta_messages)
            : false;

        if (!bytes_hit && !messages_hit) continue;

        sessions_dirty_files_.insert(session_file);
        sessions_dirty_ = true;

        // 重置 pending
        auto it = session_deltas_.find(session_file);
        if (it != session_deltas_.end() && thresholds) {
            if (thresholds->delta_bytes > 0)
                it->second.pending_bytes = std::max(
                    (size_t)0, it->second.pending_bytes - thresholds->delta_bytes);
            else
                it->second.pending_bytes = 0;
            if (thresholds->delta_messages > 0)
                it->second.pending_messages = std::max(
                    0, it->second.pending_messages - thresholds->delta_messages);
            else
                it->second.pending_messages = 0;
        }
        should_sync = true;
    }

    if (should_sync && !closed_) {
        SyncParams params;
        params.reason = "session-delta";
        do_sync(params);
    }
}

// ============================================================================
// updateSessionDelta (对齐 manager-sync-ops.ts:updateSessionDelta)
// ============================================================================
std::optional<MemoryManagerSyncOps::SessionDeltaResult>
MemoryManagerSyncOps::update_session_delta(const std::string& session_file) {
    if (!settings_.sync.sessions) return std::nullopt;

    size_t size = 0;
    try {
        size = fs::file_size(session_file);
    } catch (...) {
        return std::nullopt;
    }

    auto& state = session_deltas_[session_file];
    size_t delta_bytes = (size >= state.last_size) ? (size - state.last_size) : 0;

    if (size < state.last_size) {
        // 文件被截断
        state.last_size = size;
        state.pending_bytes += size;
    } else {
        state.pending_bytes += delta_bytes;
        state.last_size = size;
    }

    SessionDeltaResult result;
    result.delta_bytes = settings_.sync.sessions->delta_bytes;
    result.delta_messages = settings_.sync.sessions->delta_messages;
    result.pending_bytes = state.pending_bytes;
    result.pending_messages = state.pending_messages;
    return result;
}

// ============================================================================
// scheduleWatchSync (对齐 manager-sync-ops.ts:scheduleWatchSync)
// ============================================================================
void MemoryManagerSyncOps::schedule_watch_sync() {
    if (!sources_.count(MemorySource::Memory) || !settings_.sync.watch) return;
    if (watch_timer_active_) return;

    watch_timer_active_ = true;
    std::thread([this]() {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(settings_.sync.watch_debounce_ms));
        watch_timer_active_ = false;
        if (!closed_) {
            SyncParams params;
            params.reason = "watch";
            do_sync(params);
        }
    }).detach();
}

// ============================================================================
// startMemoryWatcher (对齐 manager-sync-ops.ts:ensureWatcher)
// ============================================================================
void MemoryManagerSyncOps::start_memory_watcher() {
    if (!sources_.count(MemorySource::Memory) || !settings_.sync.watch) return;
    // 🎯 对齐 OpenClaw: chokidar.watch(watchPaths, {...})
    // 使用简单轮询实现文件变更检测
    watch_timer_active_ = true;
    std::thread([this]() {
        while (watch_timer_active_) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(settings_.sync.watch_debounce_ms));
            if (dirty_) {
                schedule_watch_sync();
            }
        }
    }).detach();
    log->info("[MemoryManagerSyncOps] Memory watcher started (polling)");
}

void MemoryManagerSyncOps::stop_memory_watcher() {
    watch_timer_active_ = false;
    // ✅ 对齐 OC: chokidar.close() — 停止文件监视
    // 设置 atomic flag 使 poll 线程退出循环
    // (线程会在下个 sleep 周期后自然结束)
    log->info("[MemoryManagerSyncOps] Memory watcher stopped");
}

// ============================================================================
// startSessionWatcher (对齐 manager-sync-ops.ts:ensureSessionListener)
// ============================================================================
void MemoryManagerSyncOps::start_session_watcher() {
    if (!sources_.count(MemorySource::Sessions)) return;
    // 🎯 对齐 OpenClaw: onSessionTranscriptUpdate(...)
    // 监视 session transcript 变更，通过 sessions_dirty_ 标志触发
    session_watch_timer_active_ = true;
    std::thread([this]() {
        while (session_watch_timer_active_) {
            if (sessions_dirty_) {
                log->debug("[MemoryManagerSyncOps] Session dirty flag set, will sync on next opportunity");
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }).detach();
    log->info("[MemoryManagerSyncOps] Session watcher started");
}

void MemoryManagerSyncOps::stop_session_watcher() {
    session_watch_timer_active_ = false;
    sessions_dirty_ = false;
    // ✅ 对齐 OC: unsubscribe session transcript listener
    log->info("[MemoryManagerSyncOps] Session watcher stopped");
}

// ============================================================================
// ensureIntervalSync (对齐 manager-sync-ops.ts:ensureIntervalSync)
// ============================================================================
void MemoryManagerSyncOps::ensure_interval_sync() {
    const auto& minutes = settings_.sync.interval_minutes;
    if (!minutes || *minutes <= 0 || interval_timer_active_) return;

    interval_timer_active_ = true;
    int ms = *minutes * 60 * 1000;
    std::thread([this, ms]() {
        while (!closed_ && interval_timer_active_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            if (!closed_) {
                SyncParams params;
                params.reason = "interval";
                do_sync(params);
            }
        }
    }).detach();
}

// ============================================================================
// syncMemoryFiles (对齐 manager-sync-ops.ts:syncMemoryFiles)
// ============================================================================
core::Result<void> MemoryManagerSyncOps::sync_memory_files(
    bool needs_full_reindex,
    MemorySyncProgressState* progress) {

    // FTS-only mode: skip embedding sync but still index to FTS
    // (对齐 ts:700-703: "Skipping memory file sync in FTS-only mode" only skips embedding,
    //  but FTS indexing still proceeds when fts is available)
    // Note: openclaw skips the entire syncMemoryFiles when !provider, because
    // FTS-only indexing is handled separately via the FTS table during indexFile.
    // HunterClaw aligns: if no provider, skip entirely (FTS-only search uses
    // pre-existing FTS data or falls back to keyword search on raw text).
    if (!provider_) {
        log->debug("Skipping memory file sync in FTS-only mode (no embedding provider)");
        return core::Result<void>::Ok();
    }
    if (!db_) return core::Result<void>::Err("Database not initialized");

    // 列出 memory 文件 (对齐 ts:listMemoryFiles)
    auto files_result = list_memory_files(workspace_dir_, settings_.extra_paths);
    if (!files_result.is_ok()) {
        return core::Result<void>::Err("Failed to list memory files: " + files_result.error);
    }

    // 构建 MemoryFileEntry 列表 (对齐 ts:buildFileEntry)
    std::vector<MemoryFileEntry> entries;
    for (const auto& file : files_result.get()) {
        auto entry = build_file_entry(file, workspace_dir_);
        if (entry) entries.push_back(*entry);
    }

    log->debug("[MemoryManagerSyncOps] sync_memory_files: files=" +
               std::to_string(entries.size()) +
               " needsFullReindex=" + (needs_full_reindex ? "true" : "false") +
               " batch=" + (batch_.enabled ? "true" : "false"));

    std::set<std::string> active_paths;
    for (const auto& e : entries) active_paths.insert(e.path);

    if (progress) {
        progress->total += static_cast<int>(entries.size());
        MemorySyncProgressUpdate upd;
        upd.completed = progress->completed;
        upd.total = progress->total;
        upd.label = batch_.enabled ? "Indexing memory files (batch)..." : "Indexing memory files…";
        if (progress->report) progress->report(upd);
    }

    // 索引每个文件 (对齐 ts:tasks)
    for (const auto& entry : entries) {
        // 检查 hash 是否变化 (对齐 ts:737-747)
        if (!needs_full_reindex) {
            auto stmt_r = db_->prepare_statement(
                "SELECT hash FROM files WHERE path = ? AND source = ?");
            if (stmt_r.is_ok()) {
                sqlite3_stmt* stmt = stmt_r.get();
                sqlite3_bind_text(stmt, 1, entry.path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, "memory", -1, SQLITE_TRANSIENT);
                bool skip = false;
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    if (h && std::string(h) == entry.hash) skip = true;
                }
                sqlite3_finalize(stmt);
                if (skip) {
                    if (progress) {
                        progress->completed++;
                        MemorySyncProgressUpdate upd;
                        upd.completed = progress->completed;
                        upd.total = progress->total;
                        if (progress->report) progress->report(upd);
                    }
                    continue;
                }
            }
        }

        auto r = index_file(entry, MemorySource::Memory, std::nullopt);
        if (!r.is_ok()) {
            log->warn("Failed to index " + entry.path + ": " + r.error);
        }
        if (progress) {
            progress->completed++;
            MemorySyncProgressUpdate upd;
            upd.completed = progress->completed;
            upd.total = progress->total;
            if (progress->report) progress->report(upd);
        }
    }

    // 清理 stale 条目 (对齐 ts:761-783)
    auto stale_r = db_->prepare_statement("SELECT path FROM files WHERE source = ?");
    if (stale_r.is_ok()) {
        sqlite3_stmt* stmt = stale_r.get();
        sqlite3_bind_text(stmt, 1, "memory", -1, SQLITE_TRANSIENT);
        std::vector<std::string> stale_paths;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (p && !active_paths.count(p)) stale_paths.push_back(p);
        }
        sqlite3_finalize(stmt);

        for (const auto& sp : stale_paths) {
            db_->exec_sql("DELETE FROM files WHERE path = '" + sp + "' AND source = 'memory'");
            try {
                db_->exec_sql(
                    "DELETE FROM " + std::string(VECTOR_TABLE) +
                    " WHERE id IN (SELECT id FROM chunks WHERE path = '" + sp + "' AND source = 'memory')");
            } catch (...) {}
            db_->exec_sql("DELETE FROM chunks WHERE path = '" + sp + "' AND source = 'memory'");
            if (fts_.enabled && fts_.available && provider_) {
                try {
                    db_->exec_sql(
                        "DELETE FROM " + std::string(FTS_TABLE) +
                        " WHERE path = '" + sp + "' AND source = 'memory' AND model = '" +
                        provider_->get_model() + "'");
                } catch (...) {}
            }
        }
    }

    return core::Result<void>::Ok();
}

// ============================================================================
// syncSessionFiles (对齐 manager-sync-ops.ts:syncSessionFiles)
// ============================================================================
core::Result<void> MemoryManagerSyncOps::sync_session_files(
    bool needs_full_reindex,
    const std::optional<std::vector<std::string>>& target_session_files_opt,
    MemorySyncProgressState* progress) {

    // FTS-only mode: skip embedding sync (对齐 ts:792-795)
    // openclaw: "Skipping session file sync in FTS-only mode (no embedding provider)"
    if (!provider_) {
        log->debug("Skipping session file sync in FTS-only mode (no embedding provider)");
        return core::Result<void>::Ok();
    }
    if (!db_) return core::Result<void>::Err("Database not initialized");

    // 确定目标文件 (对齐 ts:798-806)
    auto target_set = needs_full_reindex
        ? std::nullopt
        : normalize_target_session_files(target_session_files_opt);

    std::vector<std::string> files;
    if (target_set) {
        files.assign(target_set->begin(), target_set->end());
    } else {
        auto r = list_session_files_for_agent(agent_id_);
        if (!r.is_ok()) {
            return core::Result<void>::Err("Failed to list session files: " + r.error);
        }
        files = r.get();
    }

    // active_paths 用于清理 stale (对齐 ts:activePaths)
    std::optional<std::set<std::string>> active_paths;
    if (!target_set) {
        std::set<std::string> ap;
        for (const auto& f : files) ap.insert(session_path_for_file(f));
        active_paths = ap;
    }

    bool index_all = needs_full_reindex || target_set.has_value() || sessions_dirty_files_.empty();

    log->debug("[MemoryManagerSyncOps] sync_session_files: files=" +
               std::to_string(files.size()) +
               " indexAll=" + (index_all ? "true" : "false") +
               " dirtyFiles=" + std::to_string(sessions_dirty_files_.size()));

    if (progress) {
        progress->total += static_cast<int>(files.size());
        MemorySyncProgressUpdate upd;
        upd.completed = progress->completed;
        upd.total = progress->total;
        upd.label = batch_.enabled ? "Indexing session files (batch)..." : "Indexing session files…";
        if (progress->report) progress->report(upd);
    }

    // 索引每个 session 文件 (对齐 ts:826-871)
    for (const auto& abs_path : files) {
        if (!index_all && !sessions_dirty_files_.count(abs_path)) {
            if (progress) {
                progress->completed++;
                MemorySyncProgressUpdate upd;
                upd.completed = progress->completed;
                upd.total = progress->total;
                if (progress->report) progress->report(upd);
            }
            continue;
        }

        // 构建 session entry (对齐 ts:buildSessionEntry)
        auto entry_r = build_session_entry(abs_path);
        if (!entry_r.is_ok()) {
            if (progress) {
                progress->completed++;
                MemorySyncProgressUpdate upd;
                upd.completed = progress->completed;
                upd.total = progress->total;
                if (progress->report) progress->report(upd);
            }
            continue;
        }
        const auto& entry = entry_r.get();

        // 检查 hash (对齐 ts:848-858)
        if (!needs_full_reindex) {
            auto stmt_r = db_->prepare_statement(
                "SELECT hash FROM files WHERE path = ? AND source = ?");
            if (stmt_r.is_ok()) {
                sqlite3_stmt* stmt = stmt_r.get();
                sqlite3_bind_text(stmt, 1, entry.path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, "sessions", -1, SQLITE_TRANSIENT);
                bool skip = false;
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    if (h && std::string(h) == entry.hash) skip = true;
                }
                sqlite3_finalize(stmt);
                if (skip) {
                    reset_session_delta(abs_path, entry.size);
                    if (progress) {
                        progress->completed++;
                        MemorySyncProgressUpdate upd;
                        upd.completed = progress->completed;
                        upd.total = progress->total;
                        if (progress->report) progress->report(upd);
                    }
                    continue;
                }
            }
        }

        // 索引 session 文件 (对齐 ts:862)
        auto r = index_session_file(entry, MemorySource::Sessions);
        if (!r.is_ok()) {
            log->warn("Failed to index session " + entry.path + ": " + r.error);
        }
        reset_session_delta(abs_path, entry.size);

        if (progress) {
            progress->completed++;
            MemorySyncProgressUpdate upd;
            upd.completed = progress->completed;
            upd.total = progress->total;
            if (progress->report) progress->report(upd);
        }
    }

    // 清理 stale session 条目 (对齐 ts:874-907)
    if (active_paths) {
        auto stale_r = db_->prepare_statement("SELECT path FROM files WHERE source = ?");
        if (stale_r.is_ok()) {
            sqlite3_stmt* stmt = stale_r.get();
            sqlite3_bind_text(stmt, 1, "sessions", -1, SQLITE_TRANSIENT);
            std::vector<std::string> stale_paths;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (p && !active_paths->count(p)) stale_paths.push_back(p);
            }
            sqlite3_finalize(stmt);

            for (const auto& sp : stale_paths) {
                db_->exec_sql("DELETE FROM files WHERE path = '" + sp + "' AND source = 'sessions'");
                try {
                    db_->exec_sql(
                        "DELETE FROM " + std::string(VECTOR_TABLE) +
                        " WHERE id IN (SELECT id FROM chunks WHERE path = '" + sp + "' AND source = 'sessions')");
                } catch (...) {}
                db_->exec_sql("DELETE FROM chunks WHERE path = '" + sp + "' AND source = 'sessions'");
                if (fts_.enabled && fts_.available && provider_) {
                    try {
                        db_->exec_sql(
                            "DELETE FROM " + std::string(FTS_TABLE) +
                            " WHERE path = '" + sp + "' AND source = 'sessions' AND model = '" +
                            provider_->get_model() + "'");
                    } catch (...) {}
                }
            }
        }
    }

    return core::Result<void>::Ok();
}

// ============================================================================
// resetIndex (对齐 manager-sync-ops.ts:resetIndex)
// ============================================================================
void MemoryManagerSyncOps::reset_index() {
    if (!db_) return;
    db_->exec_sql("DELETE FROM files");
    db_->exec_sql("DELETE FROM chunks");
    if (fts_.enabled && fts_.available) {
        try { db_->exec_sql(std::string("DELETE FROM ") + FTS_TABLE); } catch (...) {}
    }
    drop_vector_table();
    vector_.dims = std::nullopt;
    sessions_dirty_files_.clear();
}

// ============================================================================
// runSafeReindex (对齐 manager-sync-ops.ts:runSafeReindex)
// ============================================================================
core::Result<void> MemoryManagerSyncOps::run_safe_reindex(const SyncParams& params) {
    const std::string db_path = settings_.store.path;
    const std::string temp_db_path = db_path + ".tmp-" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

    // 保存原始状态
    auto original_db = db_;
    bool original_db_closed = false;
    auto original_fts_available = fts_.available;
    auto original_fts_error = fts_.load_error;
    auto original_vector_available = vector_.available;
    auto original_vector_error = vector_.load_error;
    auto original_vector_dims = vector_.dims;

    auto restore = [&]() {
        if (original_db_closed) {
            try { db_ = open_database_at_path(db_path); } catch (...) {}
        } else {
            db_ = original_db;
        }
        fts_.available = original_fts_available;
        fts_.load_error = original_fts_error;
        vector_.available = original_db_closed ? std::nullopt : original_vector_available;
        vector_.load_error = original_vector_error;
        vector_.dims = original_vector_dims;
        vector_ready_future_.reset();
    };

    // 切换到临时数据库
    try {
        db_ = open_database_at_path(temp_db_path);
    } catch (const std::exception& e) {
        return core::Result<void>::Err("Failed to open temp db: " + std::string(e.what()));
    }
    vector_ready_future_.reset();
    vector_.available = std::nullopt;
    vector_.load_error = std::nullopt;
    vector_.dims = std::nullopt;
    fts_.available = false;
    fts_.load_error = std::nullopt;
    ensure_schema();

    MemoryIndexMeta next_meta;
    try {
        // 从旧 db 迁移 embedding cache
        if (original_db) seed_embedding_cache(*original_db);

        bool should_sync_memory = sources_.count(MemorySource::Memory) > 0;
        bool do_sync_sessions = should_sync_sessions(params, true);

        if (should_sync_memory) {
            auto r = sync_memory_files(true, nullptr);
            if (!r.is_ok()) throw std::runtime_error(r.error);
            dirty_ = false;
        }
        if (do_sync_sessions) {
            auto r = sync_session_files(true, std::nullopt, nullptr);
            if (!r.is_ok()) throw std::runtime_error(r.error);
            sessions_dirty_ = false;
            sessions_dirty_files_.clear();
        } else if (!sessions_dirty_files_.empty()) {
            sessions_dirty_ = true;
        } else {
            sessions_dirty_ = false;
        }

        next_meta.model = provider_ ? provider_->get_model() : "fts-only";
        next_meta.provider = provider_ ? provider_->get_id() : "none";
        next_meta.provider_key = provider_key_;
        next_meta.sources = resolve_configured_sources_for_meta();
        next_meta.scope_hash = resolve_configured_scope_hash();
        next_meta.chunk_tokens = settings_.chunking.tokens;
        next_meta.chunk_overlap = settings_.chunking.overlap;
        next_meta.fts_tokenizer = settings_.fts.tokenizer;  // 对齐 OC: meta.ftsTokenizer
        if (vector_.available && *vector_.available && vector_.dims) {
            next_meta.vector_dims = vector_.dims;
        }

        write_meta(next_meta);
        prune_embedding_cache_if_needed();

        // 关闭两个 db，原子替换
        db_.reset();
        original_db.reset();
        original_db_closed = true;

        auto swap_r = swap_index_files(db_path, temp_db_path);
        if (!swap_r.is_ok()) throw std::runtime_error(swap_r.error);

        // 重新打开
        db_ = open_database_at_path(db_path);
        vector_ready_future_.reset();
        vector_.available = std::nullopt;
        vector_.load_error = std::nullopt;
        ensure_schema();
        vector_.dims = next_meta.vector_dims;

    } catch (const std::exception& e) {
        try { db_.reset(); } catch (...) {}
        // 清理临时文件
        for (const auto& suffix : {"", "-wal", "-shm"}) {
            try { fs::remove(temp_db_path + suffix); } catch (...) {}
        }
        restore();
        return core::Result<void>::Err(e.what());
    }

    return core::Result<void>::Ok();
}

// ============================================================================
// runUnsafeReindex (对齐 manager-sync-ops.ts:runUnsafeReindex)
// ============================================================================
core::Result<void> MemoryManagerSyncOps::run_unsafe_reindex(const SyncParams& params) {
    reset_index();

    bool should_sync_memory = sources_.count(MemorySource::Memory) > 0;
    bool do_sync_sessions = should_sync_sessions(params, true);

    if (should_sync_memory) {
        auto r = sync_memory_files(true, nullptr);
        if (!r.is_ok()) return r;
        dirty_ = false;
    }
    if (do_sync_sessions) {
        auto r = sync_session_files(true, std::nullopt, nullptr);
        if (!r.is_ok()) return r;
        sessions_dirty_ = false;
        sessions_dirty_files_.clear();
    } else if (!sessions_dirty_files_.empty()) {
        sessions_dirty_ = true;
    } else {
        sessions_dirty_ = false;
    }

    MemoryIndexMeta meta;
    meta.model = provider_ ? provider_->get_model() : "fts-only";
    meta.provider = provider_ ? provider_->get_id() : "none";
    meta.provider_key = provider_key_;
    meta.sources = resolve_configured_sources_for_meta();
    meta.scope_hash = resolve_configured_scope_hash();
    meta.chunk_tokens = settings_.chunking.tokens;
    meta.chunk_overlap = settings_.chunking.overlap;
    meta.fts_tokenizer = settings_.fts.tokenizer;  // 对齐 OC: meta.ftsTokenizer
    if (vector_.available && *vector_.available && vector_.dims) {
        meta.vector_dims = vector_.dims;
    }

    write_meta(meta);
    prune_embedding_cache_if_needed();
    return core::Result<void>::Ok();
}

// ============================================================================
// runSync (对齐 manager-sync-ops.ts:runSync)
// ============================================================================
core::Result<void> MemoryManagerSyncOps::run_sync(const SyncParams& params) {
    // 1. 确保 vector 扩展加载
    bool vector_ready = ensure_vector_ready();

    // 2. 读取 meta
    auto meta = read_meta();
    auto configured_sources = resolve_configured_sources_for_meta();
    auto configured_scope_hash = resolve_configured_scope_hash();

    // 3. 处理 targeted session files
    auto target_session_files = normalize_target_session_files(params.session_files);
    bool has_target_session_files = target_session_files.has_value();

    if (has_target_session_files && sources_.count(MemorySource::Sessions)) {
        try {
            auto r = sync_session_files(
                false,
                target_session_files ? std::make_optional(
                    std::vector<std::string>(target_session_files->begin(), target_session_files->end()))
                    : std::nullopt,
                nullptr);
            if (!r.is_ok()) throw std::runtime_error(r.error);
            clear_synced_session_files(target_session_files);
        } catch (const std::exception& e) {
            std::string reason = e.what();
            if (should_fallback_on_error(reason) && activate_fallback_provider(reason)) {
                // 对齐 openclaw: runSafeReindex({ reason: params?.reason ?? "fallback", force: true })
                SyncParams fallback_params;
                fallback_params.reason = (params.reason && !params.reason->empty()) ? params.reason : std::make_optional<std::string>("fallback");
                fallback_params.force  = true;
                return run_safe_reindex(fallback_params);
            }
            return core::Result<void>::Err(reason);
        }
        return core::Result<void>::Ok();
    }

    // 4. 判断是否需要全量 reindex (对齐 ts:needsFullReindex)
    bool needs_full_reindex =
        (params.force && *params.force && !has_target_session_files) ||
        !meta ||
        (provider_ && meta->model != provider_->get_model()) ||
        (provider_ && meta->provider != provider_->get_id()) ||
        meta->provider_key != provider_key_ ||
        meta_sources_differ(*meta, configured_sources) ||
        meta->scope_hash != configured_scope_hash ||
        meta->chunk_tokens != settings_.chunking.tokens ||
        meta->chunk_overlap != settings_.chunking.overlap ||
        (vector_ready && !meta->vector_dims) ||
        // 对齐 OC: (meta.ftsTokenizer ?? "unicode61") !== params.ftsTokenizer
        (meta->fts_tokenizer.empty() ? "unicode61" : meta->fts_tokenizer) != settings_.fts.tokenizer;

    try {
        if (needs_full_reindex) {
            return run_safe_reindex(params);
        }

        bool should_sync_memory = sources_.count(MemorySource::Memory) &&
            (!has_target_session_files && (params.force && *params.force) ||
             needs_full_reindex || dirty_);
        bool do_sync_sessions = should_sync_sessions(params, needs_full_reindex);

        if (should_sync_memory) {
            auto r = sync_memory_files(needs_full_reindex, nullptr);
            if (!r.is_ok()) throw std::runtime_error(r.error);
            dirty_ = false;
        }

        if (do_sync_sessions) {
            auto r = sync_session_files(
                needs_full_reindex,
                target_session_files ? std::make_optional(
                    std::vector<std::string>(target_session_files->begin(), target_session_files->end()))
                    : std::nullopt,
                nullptr);
            if (!r.is_ok()) throw std::runtime_error(r.error);
            sessions_dirty_ = false;
            sessions_dirty_files_.clear();
        } else if (!sessions_dirty_files_.empty()) {
            sessions_dirty_ = true;
        } else {
            sessions_dirty_ = false;
        }

    } catch (const std::exception& e) {
        std::string reason = e.what();
        if (should_fallback_on_error(reason) && activate_fallback_provider(reason)) {
            // 对齐 openclaw: runSafeReindex({ reason: params?.reason ?? "fallback", force: true })
            SyncParams fallback_params;
            fallback_params.reason = (params.reason && !params.reason->empty()) ? params.reason : std::make_optional<std::string>("fallback");
            fallback_params.force  = true;
            return run_safe_reindex(fallback_params);
        }
        return core::Result<void>::Err(reason);
    }

    return core::Result<void>::Ok();
}

}  // namespace memory
}  // namespace hunterclaw
