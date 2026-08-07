#include "hunterclaw/memory/sqlite_plugin.h"
#include "hunterclaw/memory/provider_registry.h"
#include "hunterclaw/memory/query_expansion.h"
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
            return core::Result<void>::Err(
                "Failed to open database: " + std::string(sqlite3_errmsg(db)));
        }

        // Schema aligned with OpenClaw manager.ts (files + chunks + memory_embedding_cache)
        const char* schema = R"(
            CREATE TABLE IF NOT EXISTS memory_index_sources (
                path TEXT NOT NULL,
                source TEXT NOT NULL DEFAULT 'memory',
                hash TEXT,
                mtime INTEGER,
                size INTEGER,
                PRIMARY KEY (path)
            );
            CREATE TABLE IF NOT EXISTS memory_index_chunks (
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
            CREATE VIRTUAL TABLE IF NOT EXISTS memory_index_chunks_fts USING fts5(
                text,
                id UNINDEXED,
                path UNINDEXED,
                source UNINDEXED,
                model UNINDEXED,
                start_line UNINDEXED,
                end_line UNINDEXED
            );
            CREATE TABLE IF NOT EXISTS memory_embedding_cache (
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
            return core::Result<void>::Err("Schema error: " + error);
        }

        log()->info("SQLiteMemoryManager: database initialized at {}", config.db_path);
        return core::Result<void>::Ok();
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
    if (!r.ok) {
        log()->error("SQLiteMemoryManager: init failed: {}", r.error);
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

    // 对齐 manager.ts: cleaned = query.trim()
    std::string cleaned = query;
    cleaned.erase(0, cleaned.find_first_not_of(" \t\n\r"));
    cleaned.erase(cleaned.find_last_not_of(" \t\n\r") + 1);
    if (cleaned.empty()) return {};

    // 对齐 manager.ts FTS-only 模式:
    // const keywords = extractKeywords(cleaned);
    // const searchTerms = keywords.length > 0 ? keywords : [cleaned];
    // Search with each keyword and merge results (dedup by id, keep highest score)
    auto keywords = extract_keywords(cleaned);
    std::vector<std::string> search_terms;
    if (!keywords.empty()) {
        search_terms = keywords;
    } else {
        search_terms.push_back(cleaned);
    }

    // 对齐 manager.ts: const seenIds = new Map<string, ...>()
    std::map<std::string, MemorySearchResult> seen_ids;

    const char* fts_sql =
        "SELECT c.id, c.path, c.source, c.start_line, c.end_line, c.text,"
        "       bm25(memory_index_chunks_fts) AS rank"
        "  FROM memory_index_chunks_fts f"
        "  JOIN memory_index_chunks c ON c.id = f.id"
        " WHERE memory_index_chunks_fts MATCH ?"
        " ORDER BY rank ASC"
        " LIMIT ?";

    for (const auto& term : search_terms) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db, fts_sql, -1, &stmt, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_text(stmt, 1, term.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit * 2);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            double rank    = sqlite3_column_double(stmt, 6);
            // 对齐 openclaw: bm25RankToScore(rank) — rank is negative, score = max(0, 1 + rank/10)
            double score   = std::max(0.0, 1.0 + rank / 10.0);

            MemorySearchResult r;
            r.path       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            std::string src_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            r.source     = (src_str == "sessions") ? MemorySource::Sessions : MemorySource::Memory;
            r.start_line = sqlite3_column_int(stmt, 3);
            r.end_line   = sqlite3_column_int(stmt, 4);
            r.snippet    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            r.score      = score;

            // 对齐 manager.ts: keep highest score for each id
            auto it = seen_ids.find(id);
            if (it == seen_ids.end() || score > it->second.score) {
                seen_ids[id] = r;
            }
        }
        sqlite3_finalize(stmt);
    }

    // 对齐 manager.ts: toSorted + filter(score >= minScore) + slice(0, maxResults)
    std::vector<MemorySearchResult> results;
    results.reserve(seen_ids.size());
    for (auto& [id, r] : seen_ids) results.push_back(std::move(r));
    std::sort(results.begin(), results.end(),
              [](const MemorySearchResult& a, const MemorySearchResult& b) {
                  return a.score > b.score;
              });
    results.erase(
        std::remove_if(results.begin(), results.end(),
            [min_score](const MemorySearchResult& r) { return r.score < min_score; }),
        results.end());
    if (static_cast<int>(results.size()) > limit) results.resize(limit);
    return results;
}

// MemorySearchManager::read_file (对齐 manager.ts:readFile)
ReadFileResult SQLiteMemoryManager::read_file(const ReadFileParams& params) {
    ReadFileResult result;

    // 对齐 openclaw: rawPath = params.relPath.trim()
    std::string raw_path = params.rel_path;
    raw_path.erase(0, raw_path.find_first_not_of(" \t\n\r"));
    raw_path.erase(raw_path.find_last_not_of(" \t\n\r") + 1);

    if (raw_path.empty()) {
        result.error = "path required";
        return result;
    }

    // 对齐 openclaw: only allow .md files
    if (raw_path.size() < 3 || raw_path.substr(raw_path.size() - 3) != ".md") {
        result.error = "path required";
        return result;
    }

    // 对齐 openclaw: isMemoryPath check (MEMORY.md, memory.md, memory/*)
    auto is_memory_path = [](const std::string& rel) -> bool {
        if (rel.empty()) return false;
        if (rel == "MEMORY.md" || rel == "memory.md") return true;
        return rel.rfind("memory/", 0) == 0;
    };

    if (!is_memory_path(raw_path)) {
        result.error = "path required";
        return result;
    }

    // Try to read from filesystem
    std::ifstream f(raw_path);
    if (!f.is_open()) {
        result.path = raw_path;
        result.text = "";
        return result;
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    if (!params.from && !params.lines) {
        result.text = std::move(content);
        result.path = raw_path;
        return result;
    }

    // 按行切片 (对齐 openclaw: lines.slice(start-1, start-1+count).join("\n"))
    std::istringstream ss(content);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(ss, line)) lines.push_back(line);

    const int start = std::max(1, params.from.value_or(1));
    const int count = std::max(1, params.lines.value_or(static_cast<int>(lines.size())));
    const int begin_idx = start - 1;
    const int end_idx   = std::min(begin_idx + count, static_cast<int>(lines.size()));

    std::ostringstream out;
    for (int i = begin_idx; i < end_idx; ++i) {
        if (i > begin_idx) out << "\n";
        out << lines[i];
    }
    result.text = out.str();
    result.path = raw_path;
    return result;
}

// MemorySearchManager::status (对齐 manager.ts:status)
MemoryProviderStatus SQLiteMemoryManager::status() {
    MemoryProviderStatus s;
    // 对齐 openclaw: backend = "builtin"
    s.backend       = "builtin";
    s.provider      = "none";   // SQLite plugin: FTS-only, no embedding provider
    s.db_path       = impl_->config.db_path;
    s.workspace_dir = "";
    s.dirty         = false;

    if (impl_->db) {
        sqlite3_stmt* stmt = nullptr;
        // files count
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT COUNT(*) FROM memory_index_sources", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                s.files = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        // chunks count
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT COUNT(*) FROM memory_index_chunks", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                s.chunks = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        // per-source counts (对齐 openclaw: sourceCounts)
        s.source_counts = std::vector<MemoryProviderStatus::SourceCount>{};
        for (const char* src : {"memory", "sessions"}) {
            MemoryProviderStatus::SourceCount sc;
            sc.source = (std::string(src) == "memory") ? MemorySource::Memory : MemorySource::Sessions;
            if (sqlite3_prepare_v2(impl_->db,
                    "SELECT COUNT(*) FROM memory_index_sources WHERE source = ?", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, src, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) sc.files = sqlite3_column_int(stmt, 0);
                sqlite3_finalize(stmt);
            }
            if (sqlite3_prepare_v2(impl_->db,
                    "SELECT COUNT(*) FROM memory_index_chunks WHERE source = ?", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, src, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) sc.chunks = sqlite3_column_int(stmt, 0);
                sqlite3_finalize(stmt);
            }
            s.source_counts->push_back(sc);
        }
        // cache status (对齐 openclaw: cache)
        MemoryProviderStatus::CacheStatus cache;
        cache.enabled = false;  // SQLite plugin: no embedding cache
        s.cache = cache;
    }

    // FTS 状态 (对齐 openclaw: fts)
    MemoryProviderStatus::FtsStatus fts;
    fts.enabled   = true;
    fts.available = (impl_->db != nullptr);
    s.fts = fts;

    // Vector 状态 (对齐 openclaw: vector)
    MemoryProviderStatus::VectorStatus vec;
    vec.enabled   = impl_->config.hybrid_search;
    vec.available = false;  // SQLite plugin: no sqlite-vec by default
    s.vector = vec;

    // Batch 状态 (对齐 openclaw: batch)
    MemoryProviderStatus::BatchStatus batch;
    batch.enabled   = false;
    batch.failures  = 0;
    batch.limit     = 0;
    batch.wait      = false;
    batch.concurrency = 1;
    batch.poll_interval_ms = 0;
    batch.timeout_ms = 0;
    s.batch = batch;

    // Custom 状态 (对齐 openclaw: custom.searchMode)
    MemoryProviderStatus::CustomStatus custom;
    custom.search_mode = "fts-only";
    s.custom = custom;

    return s;
}

// MemorySearchManager::sync (对齐 manager.ts:sync)
// SQLiteMemoryManager is a lightweight plugin; sync just ensures the DB is ready
// and reports progress. Full indexing is done via the MemoryIndexManager path.
void SQLiteMemoryManager::sync(const SyncParams& params) {
    if (params.progress) {
        params.progress({0, 1, std::string("Initializing SQLite memory…")});
    }
    // Ensure DB is open and schema is valid
    if (!impl_->db) {
        auto r = impl_->init_database();
        if (!r.ok) {
            log()->warn("SQLiteMemoryManager::sync: DB init failed: {}", r.error);
        }
    }
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
