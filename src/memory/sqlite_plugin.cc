#include "hunterclaw/memory/sqlite_plugin.h"
#include "hunterclaw/memory/provider_registry.h"
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <set>
#include <map>
#include <chrono>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

namespace {
    auto log() {
        auto logger = spdlog::get("memory");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("memory");
        }
        return logger;
    }
}

// ============================================================================
// SQLiteMemoryManager::Impl
// ============================================================================

class SQLiteMemoryManager::Impl {
public:
    sqlite3* db = nullptr;
    Config config;

    ~Impl() {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }

    core::Result<void> init_database() {
        int rc = sqlite3_open(config.db_path.c_str(), &db);
        if (rc != SQLITE_OK) {
            return core::Result<void>::err(
                "Failed to open database: " + std::string(sqlite3_errmsg(db)));
        }

        // Schema aligned with OpenClaw manager.ts (files + chunks + embedding_cache)
        const char* schema = R"(
            CREATE TABLE IF NOT EXISTS files (
                path TEXT NOT NULL,
                source TEXT NOT NULL DEFAULT 'memory',
                hash TEXT,
                mtime INTEGER,
                size INTEGER,
                PRIMARY KEY (path)
            );
            CREATE TABLE IF NOT EXISTS chunks (
                id TEXT PRIMARY KEY,
                path TEXT NOT NULL,
                source TEXT NOT NULL DEFAULT 'memory',
                start_line INTEGER NOT NULL DEFAULT 0,
                end_line INTEGER NOT NULL DEFAULT 0,
                hash TEXT,
                model TEXT,
                text TEXT NOT NULL,
                embedding TEXT,
                updated_at INTEGER
            );
            CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
                text,
                id UNINDEXED,
                path UNINDEXED,
                source UNINDEXED,
                model UNINDEXED,
                start_line UNINDEXED,
                end_line UNINDEXED
            );
            CREATE TABLE IF NOT EXISTS embedding_cache (
                provider TEXT NOT NULL,
                model TEXT NOT NULL,
                provider_key TEXT NOT NULL DEFAULT '',
                hash TEXT NOT NULL,
                embedding TEXT NOT NULL,
                dims INTEGER,
                updated_at INTEGER,
                PRIMARY KEY (provider, model, provider_key, hash)
            );
        )";

        char* err_msg = nullptr;
        rc = sqlite3_exec(db, schema, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string error = err_msg ? err_msg : "unknown";
            sqlite3_free(err_msg);
            return core::Result<void>::err("Schema error: " + error);
        }

        log()->info("SQLiteMemoryManager: database initialized at {}", config.db_path);
        return core::Result<void>::ok();
    }

    // Cosine similarity
    double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) const {
        if (a.size() != b.size() || a.empty()) return 0.0;
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            dot += a[i] * b[i];
            na  += a[i] * a[i];
            nb  += b[i] * b[i];
        }
        if (na == 0.0 || nb == 0.0) return 0.0;
        return dot / (std::sqrt(na) * std::sqrt(nb));
    }
};

// ============================================================================
// SQLiteMemoryManager
// ============================================================================

SQLiteMemoryManager::SQLiteMemoryManager(const Config& config)
    : impl_(std::make_unique<Impl>())
{
    impl_->config = config;
    auto r = impl_->init_database();
    if (!r.ok()) {
        log()->error("SQLiteMemoryManager: init failed: {}", r.error());
    }
}

SQLiteMemoryManager::~SQLiteMemoryManager() = default;

// MemorySearchManager::search (对齐 manager.ts:search)
std::vector<MemorySearchResult> SQLiteMemoryManager::search(
    const std::string& query,
    const SearchOptions& opts)
{
    if (!impl_->db) return {};

    const int limit = opts.max_results.value_or(6);
    const double min_score = opts.min_score.value_or(0.35);

    std::vector<MemorySearchResult> results;

    if (impl_->config.hybrid_search) {
        // Hybrid: FTS keyword search
        const char* fts_sql =
            "SELECT c.path, c.source, c.start_line, c.end_line, c.text "
            "FROM chunks_fts f "
            "JOIN chunks c ON c.id = f.id "
            "WHERE chunks_fts MATCH ? "
            "ORDER BY rank LIMIT ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db, fts_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, limit * 2);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                MemorySearchResult r;
                r.path       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                r.source     = MemorySource::Memory;
                r.start_line = sqlite3_column_int(stmt, 2);
                r.end_line   = sqlite3_column_int(stmt, 3);
                r.snippet    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                r.score      = impl_->config.text_weight;
                results.push_back(r);
            }
            sqlite3_finalize(stmt);
        }
    } else {
        // FTS-only
        const char* fts_sql =
            "SELECT c.path, c.source, c.start_line, c.end_line, c.text "
            "FROM chunks_fts f "
            "JOIN chunks c ON c.id = f.id "
            "WHERE chunks_fts MATCH ? "
            "ORDER BY rank LIMIT ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db, fts_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, limit);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                MemorySearchResult r;
                r.path       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                r.source     = MemorySource::Memory;
                r.start_line = sqlite3_column_int(stmt, 2);
                r.end_line   = sqlite3_column_int(stmt, 3);
                r.snippet    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                r.score      = 0.8;
                results.push_back(r);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Filter by min_score and truncate
    results.erase(
        std::remove_if(results.begin(), results.end(),
            [min_score](const MemorySearchResult& r) { return r.score < min_score; }),
        results.end());
    if ((int)results.size() > limit) results.resize(limit);
    return results;
}

// MemorySearchManager::read_file (对齐 manager.ts:readFile)
ReadFileResult SQLiteMemoryManager::read_file(const ReadFileParams& params) {
    ReadFileResult result;
    result.path = params.rel_path;

    // Try to read from filesystem (memory .md files)
    if (!params.rel_path.empty() && params.rel_path.back() != '/') {
        std::ifstream f(params.rel_path);
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            if (!params.from && !params.lines) {
                result.text = std::move(content);
            } else {
                std::istringstream ss(content);
                std::string line;
                std::vector<std::string> lines;
                while (std::getline(ss, line)) lines.push_back(line);
                int start = std::max(1, params.from.value_or(1));
                int count = std::max(1, params.lines.value_or((int)lines.size()));
                std::string out;
                for (int i = start - 1; i < start - 1 + count && i < (int)lines.size(); ++i) {
                    if (!out.empty()) out += '\n';
                    out += lines[i];
                }
                result.text = std::move(out);
            }
        }
    }
    return result;
}

// MemorySearchManager::status (对齐 manager.ts:status)
MemoryProviderStatus SQLiteMemoryManager::status() {
    MemoryProviderStatus s;
    s.backend  = "builtin";
    s.provider = "sqlite-memory";
    s.db_path  = impl_->config.db_path;

    if (impl_->db) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT COUNT(*) FROM files", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                s.files = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT COUNT(*) FROM chunks", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                s.chunks = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    MemoryProviderStatus::FtsStatus fts;
    fts.enabled   = true;
    fts.available = (impl_->db != nullptr);
    s.fts = fts;

    return s;
}

// MemorySearchManager::sync (对齐 manager.ts:sync)
void SQLiteMemoryManager::sync(const SyncParams& params) {
    if (params.progress) {
        params.progress({1, 1, std::string("SQLite memory ready")});
    }
}

// MemorySearchManager::probe_embedding_availability
MemoryEmbeddingProbeResult SQLiteMemoryManager::probe_embedding_availability() {
    // SQLite plugin uses FTS-only mode by default (no embedding provider)
    return MemoryEmbeddingProbeResult{false,
        std::string("SQLite plugin uses FTS-only mode; configure an embedding provider")};
}

// MemorySearchManager::probe_vector_availability
bool SQLiteMemoryManager::probe_vector_availability() {
    return false;  // sqlite-vec not loaded by default
}

// MemorySearchManager::close (对齐 manager.ts:close)
void SQLiteMemoryManager::close() {
    if (impl_->db) {
        sqlite3_close(impl_->db);
        impl_->db = nullptr;
    }
}

// Factory (对齐 MemoryIndexManager::get)
std::shared_ptr<SQLiteMemoryManager> SQLiteMemoryManager::create(
    const std::string& /*agent_id*/,
    const std::string& workspace_dir,
    const std::map<std::string, std::string>& plugin_config)
{
    Config cfg;
    cfg.db_path = workspace_dir + "/.hunterclaw/memory.db";

    auto it = plugin_config.find("db_path");
    if (it != plugin_config.end()) cfg.db_path = it->second;

    it = plugin_config.find("embedding_provider");
    if (it != plugin_config.end()) cfg.embedding_provider = it->second;

    it = plugin_config.find("hybrid_search");
    if (it != plugin_config.end()) cfg.hybrid_search = (it->second == "true");

    it = plugin_config.find("vector_weight");
    if (it != plugin_config.end()) cfg.vector_weight = std::stod(it->second);

    it = plugin_config.find("text_weight");
    if (it != plugin_config.end()) cfg.text_weight = std::stod(it->second);

    it = plugin_config.find("mmr_enabled");
    if (it != plugin_config.end()) cfg.mmr_enabled = (it->second == "true");

    it = plugin_config.find("temporal_decay");
    if (it != plugin_config.end()) cfg.temporal_decay = (it->second == "true");

    // Ensure parent directory exists
    fs::create_directories(fs::path(cfg.db_path).parent_path());

    return std::make_shared<SQLiteMemoryManager>(cfg);
}

// ============================================================================
// Plugin definition factory
// ============================================================================

MemoryPluginDefinition make_sqlite_memory_plugin_definition() {
    MemoryPluginDefinition def;
    def.id          = "sqlite-memory";
    def.name        = "SQLite Memory";
    def.description = "Local SQLite-based memory storage with FTS5 and vector search";
    def.version     = "1.0.0";
    def.kind        = PluginKind::Memory;

    def.create_manager = [](
        const std::string& agent_id,
        const std::string& workspace_dir,
        const std::map<std::string, std::string>& plugin_config
    ) -> std::shared_ptr<MemorySearchManager> {
        return SQLiteMemoryManager::create(agent_id, workspace_dir, plugin_config);
    };

    return def;
}

} // namespace memory
} // namespace hunterclaw

// ============================================================================
// Plugin .so export
// Convention: extern "C" void register_memory_plugin(MemoryPluginRegistry&)
// ============================================================================

extern "C" {

void register_memory_plugin(hunterclaw::memory::MemoryPluginRegistry& registry) {
    registry.register_plugin(
        hunterclaw::memory::make_sqlite_memory_plugin_definition());
}

} // extern "C"
