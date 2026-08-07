#include <optional>
#include "hunterclaw/memory/sqlite_storage.h"
#include <sqlite3.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <mutex>
#include <cstring>
#include <random>
#include <chrono>
#include <iomanip>
#include <regex>
#include <algorithm>
#include <set>
#include <vector>

namespace hunterclaw {
namespace memory {

namespace {

// 🎯 严格对齐 OpenClaw memory 注解 (project-memory-bootstrap: HTML 注释携带):
//   <!-- importance: 8 -->  <!-- trigger: release helper -->  <!-- project: key -->
//   从 chunk 文本解析召回元数据，写入 memory_index_chunk_recall_metadata。
struct RecallAnnotation {
    std::optional<int> importance;      // 1..10 (超出范围忽略)
    std::optional<std::string> triggers;
    std::optional<std::string> project_key;
    bool any() const { return importance || triggers || project_key; }
};

inline std::string trim_copy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

RecallAnnotation parse_recall_annotation(const std::string& text) {
    RecallAnnotation ann;
    // importance: 捕获 1-2 位数字
    static const std::regex re_importance(
        R"(<!--\s*importance\s*:\s*([0-9]{1,2})\s*-->)", std::regex::icase);
    // trigger(s): 捕获到 --> 前的内容
    static const std::regex re_trigger(
        R"(<!--\s*triggers?\s*:\s*(.*?)\s*-->)", std::regex::icase);
    // project: 捕获到 --> 前的内容（可能多个，用 ; 合并）
    static const std::regex re_project(
        R"(<!--\s*project\s*:\s*(.*?)\s*-->)", std::regex::icase);

    std::smatch m;
    if (std::regex_search(text, m, re_importance) && m.size() > 1) {
        int v = std::atoi(m[1].str().c_str());
        if (v >= 1 && v <= 10) ann.importance = v;   // 对齐 CHECK(importance BETWEEN 1 AND 10)
    }
    // triggers: 可能多条，用 "; " 连接（对齐 OC 存储为单字符串）
    {
        std::vector<std::string> parts;
        auto begin = std::sregex_iterator(text.begin(), text.end(), re_trigger);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string v = trim_copy((*it)[1].str());
            if (!v.empty()) parts.push_back(v);
        }
        if (!parts.empty()) {
            std::string joined;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) joined += "; ";
                joined += parts[i];
            }
            ann.triggers = joined;
        }
    }
    // project keys: 多条用 ";" 连接（对齐 OC project_key split(";")）
    {
        std::vector<std::string> keys;
        auto begin = std::sregex_iterator(text.begin(), text.end(), re_project);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string v = trim_copy((*it)[1].str());
            if (!v.empty()) keys.push_back(v);
        }
        if (!keys.empty()) {
            std::string joined;
            for (size_t i = 0; i < keys.size(); ++i) {
                if (i) joined += ";";
                joined += keys[i];
            }
            ann.project_key = joined;
        }
    }
    return ann;
}

// Helper: execute SQL without result
bool exec_sql(sqlite3* db, const std::string& sql, std::string* error = nullptr) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (error && err_msg) {
            *error = err_msg;
        }
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

// Helper: prepare statement
sqlite3_stmt* prepare_stmt(sqlite3* db, const std::string& sql, std::string* error = nullptr) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        if (error) {
            *error = sqlite3_errmsg(db);
        }
        return nullptr;
    }
    return stmt;
}

} // anonymous namespace

// ============================================================================
// SQLiteVectorStorage Implementation
// ============================================================================

SQLiteVectorStorage::SQLiteVectorStorage(const Config& config)
    : config_(config), db_(nullptr), fts_available_(false), vec_available_(false) {}

SQLiteVectorStorage::~SQLiteVectorStorage() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

core::Result<std::shared_ptr<SQLiteVectorStorage>> SQLiteVectorStorage::create(const Config& config) {
    auto storage = std::shared_ptr<SQLiteVectorStorage>(new SQLiteVectorStorage(config));
    
    // Open database
    auto open_res = storage->open_database();
    if (!open_res.is_ok()) {
        return core::Result<std::shared_ptr<SQLiteVectorStorage>>::Err(open_res.error);
    }
    
    // Try enable FTS5
    if (config.enable_fts) {
        auto fts_res = storage->try_enable_fts5();
        if (!fts_res.is_ok()) {
            storage->fts_error_ = fts_res.error;
            storage->fts_available_ = false;
        } else {
            storage->fts_available_ = fts_res.get();
        }
    }
    
    // Load sqlite-vec extension if provided
    if (config.vec_extension_path.has_value() && config.enable_vector) {
        auto vec_res = storage->load_sqlite_vec_extension();
        if (!vec_res.is_ok()) {
            storage->vec_error_ = vec_res.error;
            storage->vec_available_ = false;
        } else {
            storage->vec_available_ = vec_res.get();
        }
    }
    
    // Ensure schema
    auto schema_res = storage->ensure_schema();
    if (!schema_res.is_ok()) {
        return core::Result<std::shared_ptr<SQLiteVectorStorage>>::Err(schema_res.error);
    }
    
    return core::Result<std::shared_ptr<SQLiteVectorStorage>>::Ok(storage);
}

core::Result<bool> SQLiteVectorStorage::open_database() {
    if (db_) {
        return core::Result<bool>::Ok(true);
    }
    
    // Ensure parent directory exists
    std::filesystem::path path(config_.db_path);
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return core::Result<bool>::Err("Failed to create directory: " + ec.message());
        }
    }
    
    int rc = sqlite3_open(config_.db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return core::Result<bool>::Err("Failed to open database: " + err);
    }
    
    // Enable WAL mode for better concurrency
    ::hunterclaw::memory::exec_sql(db_, "PRAGMA journal_mode=WAL");
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> SQLiteVectorStorage::ensure_schema() {
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }

    std::string error;

    // 🎯 严格对齐 OpenClaw canonical memory schema
    //   (packages/memory-host-sdk/src/host/memory-schema-{base,fts,recall,provenance}.ts)
    //   全部使用 memory_index_* 前缀 + STRICT。

    // memory_index_meta
    if (!::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TABLE IF NOT EXISTS memory_index_meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        ) STRICT;
    )", &error)) {
        return core::Result<bool>::Err("Failed to create memory_index_meta: " + error);
    }

    // memory_index_sources (对齐 OC: 原 files 表 → sources；mtime REAL；UNIQUE(path,source))
    if (!::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TABLE IF NOT EXISTS memory_index_sources (
            id INTEGER PRIMARY KEY,
            path TEXT NOT NULL,
            source TEXT NOT NULL DEFAULT 'memory',
            hash TEXT NOT NULL,
            mtime REAL NOT NULL,
            size INTEGER NOT NULL,
            UNIQUE (path, source)
        ) STRICT;
    )", &error)) {
        return core::Result<bool>::Err("Failed to create memory_index_sources: " + error);
    }

    // memory_index_chunks (对齐 OC)
    if (!::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TABLE IF NOT EXISTS memory_index_chunks (
            id TEXT PRIMARY KEY,
            path TEXT NOT NULL,
            source TEXT NOT NULL DEFAULT 'memory',
            start_line INTEGER NOT NULL,
            end_line INTEGER NOT NULL,
            hash TEXT NOT NULL,
            model TEXT NOT NULL,
            text TEXT NOT NULL,
            embedding TEXT NOT NULL,
            updated_at INTEGER NOT NULL
        ) STRICT;
    )", &error)) {
        return core::Result<bool>::Err("Failed to create memory_index_chunks: " + error);
    }

    // memory_index_chunk_recall_metadata (对齐 OC memory-schema-recall.ts)
    if (!::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TABLE IF NOT EXISTS memory_index_chunk_recall_metadata (
            chunk_id TEXT PRIMARY KEY,
            importance INTEGER CHECK (importance IS NULL OR importance BETWEEN 1 AND 10),
            triggers TEXT,
            project_key TEXT,
            FOREIGN KEY (chunk_id) REFERENCES memory_index_chunks(id) ON DELETE CASCADE
        ) STRICT;
    )", &error)) {
        return core::Result<bool>::Err("Failed to create memory_index_chunk_recall_metadata: " + error);
    }

    // memory_index_chunk_provenance (对齐 OC memory-schema-provenance.ts)
    if (!::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TABLE IF NOT EXISTS memory_index_chunk_provenance (
            chunk_id TEXT PRIMARY KEY,
            origin_class TEXT NOT NULL CHECK (origin_class IN ('owner', 'agent', 'untrusted', 'system')),
            session_kind TEXT NOT NULL CHECK (session_kind IN ('interactive', 'cron', 'heartbeat', 'subagent', 'unknown')),
            observed_at INTEGER NOT NULL,
            supersedes_key TEXT,
            FOREIGN KEY (chunk_id) REFERENCES memory_index_chunks(id) ON DELETE CASCADE
        ) STRICT;
    )", &error)) {
        return core::Result<bool>::Err("Failed to create memory_index_chunk_provenance: " + error);
    }

    // memory_index_state (对齐 OC: 单行 revision 计数)
    if (!::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TABLE IF NOT EXISTS memory_index_state (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            revision INTEGER NOT NULL
        ) STRICT;
    )", &error)) {
        return core::Result<bool>::Err("Failed to create memory_index_state: " + error);
    }
    // 初始化 revision 单行（对齐 OC: 默认 revision=0）
    ::hunterclaw::memory::exec_sql(db_,
        "INSERT OR IGNORE INTO memory_index_state (id, revision) VALUES (1, 0);");

    // memory_embedding_cache (对齐 OC memory-schema-base.ts)
    if (!::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TABLE IF NOT EXISTS memory_embedding_cache (
            provider TEXT NOT NULL,
            model TEXT NOT NULL,
            provider_key TEXT NOT NULL,
            hash TEXT NOT NULL,
            embedding TEXT NOT NULL,
            dims INTEGER,
            updated_at INTEGER NOT NULL,
            PRIMARY KEY (provider, model, provider_key, hash)
        ) STRICT;
    )", &error)) {
        return core::Result<bool>::Err("Failed to create memory_embedding_cache: " + error);
    }

    ::hunterclaw::memory::exec_sql(db_,
        "CREATE INDEX IF NOT EXISTS idx_embedding_cache_updated_at ON memory_embedding_cache(updated_at);");

    // FTS5 body table (对齐 OC memory-schema-fts.ts: memory_index_chunks_fts)
    if (config_.enable_fts) {
        std::string fts_error;
        std::string tokenizer_clause;
        if (config_.fts_tokenizer == "trigram") {
            tokenizer_clause = ", tokenize=\"trigram case_sensitive 0\"";
        } else {
            tokenizer_clause = ", tokenize=\"unicode61\"";
        }
        // body FTS: 列顺序严格对齐 OC (text, id, path, source, model, start_line, end_line)
        std::string fts_sql = R"(
            CREATE VIRTUAL TABLE IF NOT EXISTS memory_index_chunks_fts USING fts5(
                text,
                id UNINDEXED,
                path UNINDEXED,
                source UNINDEXED,
                model UNINDEXED,
                start_line UNINDEXED,
                end_line UNINDEXED)" + tokenizer_clause + R"(
            );
        )";
        bool fts_ok = ::hunterclaw::memory::exec_sql(db_, fts_sql, &fts_error);
        if (fts_ok) {
            fts_available_ = true;
            // path FTS (对齐 OC ensureMemoryPathFtsSchema: memory_index_paths_fts + triggers)
            std::string paths_fts_sql = R"(
                CREATE VIRTUAL TABLE IF NOT EXISTS memory_index_paths_fts USING fts5(
                    path,
                    source UNINDEXED)" + tokenizer_clause + R"(
                );
            )";
            ::hunterclaw::memory::exec_sql(db_, paths_fts_sql);
            // canonical source→path_fts 维护 triggers (对齐 OC MEMORY_PATH_FTS_TRIGGER_DEFINITIONS)
            ::hunterclaw::memory::exec_sql(db_, R"(
                CREATE TRIGGER IF NOT EXISTS memory_index_paths_fts_after_insert
                AFTER INSERT ON memory_index_sources
                BEGIN
                    INSERT INTO memory_index_paths_fts (rowid, path, source)
                    VALUES (NEW.id, NEW.path, NEW.source);
                END;
            )");
            ::hunterclaw::memory::exec_sql(db_, R"(
                CREATE TRIGGER IF NOT EXISTS memory_index_paths_fts_after_update
                AFTER UPDATE OF id, path, source ON memory_index_sources
                BEGIN
                    DELETE FROM memory_index_paths_fts WHERE rowid = OLD.id;
                    INSERT INTO memory_index_paths_fts (rowid, path, source)
                    VALUES (NEW.id, NEW.path, NEW.source);
                END;
            )");
            ::hunterclaw::memory::exec_sql(db_, R"(
                CREATE TRIGGER IF NOT EXISTS memory_index_paths_fts_after_delete
                AFTER DELETE ON memory_index_sources
                BEGIN
                    DELETE FROM memory_index_paths_fts WHERE rowid = OLD.id;
                END;
            )");
        } else {
            fts_available_ = false;
            fts_error_ = fts_error;
        }
    }

    // 🎯 严格对齐 OpenClaw memory-schema.ts:619-649: revision 计数 triggers。
    //   sources / chunks 各 insert/update/delete → memory_index_state.revision += 1，
    //   用于外部感知索引变更（缓存失效/增量同步）。无条件建立（非 FTS 依赖）。
    ::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TRIGGER IF NOT EXISTS memory_index_sources_revision_after_insert
        AFTER INSERT ON memory_index_sources
        BEGIN UPDATE memory_index_state SET revision = revision + 1 WHERE id = 1; END;
    )");
    ::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TRIGGER IF NOT EXISTS memory_index_sources_revision_after_update
        AFTER UPDATE ON memory_index_sources
        BEGIN UPDATE memory_index_state SET revision = revision + 1 WHERE id = 1; END;
    )");
    ::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TRIGGER IF NOT EXISTS memory_index_sources_revision_after_delete
        AFTER DELETE ON memory_index_sources
        BEGIN UPDATE memory_index_state SET revision = revision + 1 WHERE id = 1; END;
    )");
    ::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TRIGGER IF NOT EXISTS memory_index_chunks_revision_after_insert
        AFTER INSERT ON memory_index_chunks
        BEGIN UPDATE memory_index_state SET revision = revision + 1 WHERE id = 1; END;
    )");
    ::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TRIGGER IF NOT EXISTS memory_index_chunks_revision_after_update
        AFTER UPDATE ON memory_index_chunks
        BEGIN UPDATE memory_index_state SET revision = revision + 1 WHERE id = 1; END;
    )");
    ::hunterclaw::memory::exec_sql(db_, R"(
        CREATE TRIGGER IF NOT EXISTS memory_index_chunks_revision_after_delete
        AFTER DELETE ON memory_index_chunks
        BEGIN UPDATE memory_index_state SET revision = revision + 1 WHERE id = 1; END;
    )");

    // Indices (canonical 表名)
    ::hunterclaw::memory::exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_path ON memory_index_chunks(path);");
    ::hunterclaw::memory::exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_source ON memory_index_chunks(source);");
    // 🎯 对齐 OC memory-schema.ts:651: sources.source 索引
    ::hunterclaw::memory::exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_memory_index_sources_source ON memory_index_sources(source);");

    // sqlite-vec virtual table - only if vec extension loaded
    if (vec_available_) {
        ::hunterclaw::memory::exec_sql(db_, R"(
            CREATE VIRTUAL TABLE IF NOT EXISTS memory_index_chunks_vec USING vec0(
                id TEXT PRIMARY KEY,
                embedding FLOAT[1536]
            );
        )");
    }

    return core::Result<bool>::Ok(true);
}

core::Result<bool> SQLiteVectorStorage::load_sqlite_vec_extension() {
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    if (!config_.vec_extension_path.has_value()) {
        return core::Result<bool>::Ok(false);
    }
    
    // Enable extension loading
    int rc = sqlite3_enable_load_extension(db_, 1);
    if (rc != SQLITE_OK) {
        return core::Result<bool>::Err("Failed to enable extensions: " + std::string(sqlite3_errmsg(db_)));
    }
    
    // Load extension
    char* err_msg = nullptr;
    rc = sqlite3_load_extension(db_, config_.vec_extension_path.value().c_str(), nullptr, &err_msg);
    
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : sqlite3_errmsg(db_);
        sqlite3_free(err_msg);
        return core::Result<bool>::Err("Failed to load sqlite-vec: " + error);
    }
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> SQLiteVectorStorage::try_enable_fts5() {
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    // Try to create a test FTS5 table to check if FTS5 is available
    std::string error;
    bool fts5_ok = ::hunterclaw::memory::exec_sql(db_, 
        "CREATE VIRTUAL TABLE IF NOT EXISTS fts5_test USING fts5(content)",
        &error);
    
    if (fts5_ok) {
        ::hunterclaw::memory::exec_sql(db_, "DROP TABLE IF EXISTS fts5_test");
        return core::Result<bool>::Ok(true);
    }
    
    return core::Result<bool>::Err("FTS5 not available: " + error);
}

core::Result<bool> SQLiteVectorStorage::create_tables() {
    // Already done in ensure_schema
    return ensure_schema();
}

core::Result<bool> SQLiteVectorStorage::create_indices() {
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    std::string error;
    if (!::hunterclaw::memory::exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_vec_path ON memory_index_chunks_vec(path)", &error)) {
        return core::Result<bool>::Err("Failed to create index: " + error);
    }
    
    if (!::hunterclaw::memory::exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_vec_source ON memory_index_chunks_vec(source)", &error)) {
        return core::Result<bool>::Err("Failed to create index: " + error);
    }
    
    return core::Result<bool>::Ok(true);
}

// ============================================================================
// Chunk Operations
// ============================================================================

std::string SQLiteVectorStorage::generate_id() {
    // Generate UUID-like ID
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(16) << dis(gen);
    ss << std::setw(16) << dis(gen);
    
    return ss.str();
}

std::string SQLiteVectorStorage::serialize_embedding(const std::vector<float>& embedding) {
    return std::string(reinterpret_cast<const char*>(embedding.data()), 
                      embedding.size() * sizeof(float));
}

std::vector<float> SQLiteVectorStorage::deserialize_embedding(const std::string& blob) {
    const float* data = reinterpret_cast<const float*>(blob.data());
    size_t count = blob.size() / sizeof(float);
    return std::vector<float>(data, data + count);
}

std::string SQLiteVectorStorage::compute_content_hash(const std::string& content) {
    // Simple hash for now - in production use SHA256 or similar
    std::hash<std::string> hasher;
    std::stringstream ss;
    ss << std::hex << hasher(content);
    return ss.str();
}

core::Result<std::string> SQLiteVectorStorage::add_chunk(
    const std::string& path,
    const std::string& content,
    const std::vector<float>& embedding,
    const ChunkMetadata& metadata) {
    
    if (!db_) {
        return core::Result<std::string>::Err("Database not open");
    }
    
    std::string id = generate_id();

    // 🎯 对齐 OC canonical: 主数据写 memory_index_chunks（非 vec 表）。
    const char* sql = R"(
        INSERT OR REPLACE INTO memory_index_chunks
        (id, path, source, start_line, end_line, hash, model, text, embedding, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<std::string>::Err("Failed to prepare insert: " + error);
    }

    int64_t updated_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string emb_text = serialize_embedding(embedding);
    std::string content_hash = compute_content_hash(content);

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, to_string(metadata.source).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, metadata.start_line);
    sqlite3_bind_int(stmt, 5, metadata.end_line);
    sqlite3_bind_text(stmt, 6, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, metadata.model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, emb_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 10, updated_at);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return core::Result<std::string>::Err("Failed to insert chunk: " + std::string(sqlite3_errmsg(db_)));
    }

    // 🎯 对齐 OC memory-schema-provenance.ts: 新chunk 默认写 provenance
    //   (origin_class='untrusted', session_kind='unknown', observed_at=updated_at)。
    //   与 OC 存量迁移默认值一致；上层若有可信来源可后续upsert 覆盖。
    {
        sqlite3_stmt* pstmt = prepare_stmt(db_, R"(
            INSERT OR IGNORE INTO memory_index_chunk_provenance
            (chunk_id, origin_class, session_kind, observed_at)
            VALUES (?, 'untrusted', 'unknown', ?)
        )");
        if (pstmt) {
            sqlite3_bind_text(pstmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(pstmt, 2, updated_at);
            sqlite3_step(pstmt);
            sqlite3_finalize(pstmt);
        }
    }

    // 🎯 严格对齐 OC recall curation: 从 chunk 文本解析 importance/triggers/project
    //   注解并upsert 到 memory_index_chunk_recall_metadata（懒写：无注解则不写，
    //   读时返回 null）。对齐 memory-schema-recall.ts 的 ON CONFLICT(chunk_id) upsert。
    {
        RecallAnnotation ann = parse_recall_annotation(content);
        if (ann.any()) {
            sqlite3_stmt* rstmt = prepare_stmt(db_, R"(
                INSERT INTO memory_index_chunk_recall_metadata
                (chunk_id, importance, triggers, project_key)
                VALUES (?, ?, ?, ?)
                ON CONFLICT(chunk_id) DO UPDATE SET
                    importance = excluded.importance,
                    triggers = excluded.triggers,
                    project_key = excluded.project_key
            )");
            if (rstmt) {
                sqlite3_bind_text(rstmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
                if (ann.importance) sqlite3_bind_int(rstmt, 2, *ann.importance);
                else sqlite3_bind_null(rstmt, 2);
                if (ann.triggers) sqlite3_bind_text(rstmt, 3, ann.triggers->c_str(), -1, SQLITE_TRANSIENT);
                else sqlite3_bind_null(rstmt, 3);
                if (ann.project_key) sqlite3_bind_text(rstmt, 4, ann.project_key->c_str(), -1, SQLITE_TRANSIENT);
                else sqlite3_bind_null(rstmt, 4);
                sqlite3_step(rstmt);
                sqlite3_finalize(rstmt);
            }
        }
    }

    // vec 表（canonical: 仅 id + embedding），仅在 vec 扩展可用时。
    if (vec_available_ && !embedding.empty()) {
        sqlite3_stmt* vstmt = prepare_stmt(db_,
            "INSERT OR REPLACE INTO memory_index_chunks_vec (id, embedding) VALUES (?, ?)");
        if (vstmt) {
            std::string blob = serialize_embedding(embedding);
            sqlite3_bind_text(vstmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(vstmt, 2, blob.data(), blob.size(), SQLITE_TRANSIENT);
            sqlite3_step(vstmt);
            sqlite3_finalize(vstmt);
        }
    }

    // FTS（canonical 列顺序: text, id, path, source, model, start_line, end_line）
    if (fts_available_) {
        const char* fts_sql = R"(
            INSERT INTO memory_index_chunks_fts (text, id, path, source, model, start_line, end_line)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt* fts_stmt = prepare_stmt(db_, fts_sql);
        if (fts_stmt) {
            sqlite3_bind_text(fts_stmt, 1, content.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(fts_stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(fts_stmt, 3, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(fts_stmt, 4, to_string(metadata.source).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(fts_stmt, 5, metadata.model.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(fts_stmt, 6, metadata.start_line);
            sqlite3_bind_int(fts_stmt, 7, metadata.end_line);
            sqlite3_step(fts_stmt);
            sqlite3_finalize(fts_stmt);
        }
    }

    return core::Result<std::string>::Ok(id);
}

core::Result<std::vector<std::string>> SQLiteVectorStorage::add_chunks_batch(
    const std::vector<std::tuple<std::string, std::string, std::vector<float>, ChunkMetadata>>& chunks) {
    
    if (chunks.empty()) {
        return core::Result<std::vector<std::string>>::Ok(std::vector<std::string>());
    }
    
    std::vector<std::string> ids;
    ids.reserve(chunks.size());
    
    auto tx_begin = begin_transaction();
    if (!tx_begin.is_ok()) {
        return core::Result<std::vector<std::string>>::Err(tx_begin.error);
    }
    
    for (const auto& [path, content, embedding, metadata] : chunks) {
        auto res = add_chunk(path, content, embedding, metadata);
        if (!res.is_ok()) {
            rollback_transaction();
            return core::Result<std::vector<std::string>>::Err(res.error);
        }
        ids.push_back(res.get());
    }
    
    auto tx_commit = commit_transaction();
    if (!tx_commit.is_ok()) {
        return core::Result<std::vector<std::string>>::Err(tx_commit.error);
    }
    
    return core::Result<std::vector<std::string>>::Ok(ids);
}

core::Result<int> SQLiteVectorStorage::delete_chunks_by_path(const std::string& path) {
    if (!db_) {
        return core::Result<int>::Err("Database not open");
    }
    
    const char* sql = "DELETE FROM memory_index_chunks_vec WHERE path = ?";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<int>::Err("Failed to prepare delete: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        return core::Result<int>::Err("Failed to delete chunks: " + std::string(sqlite3_errmsg(db_)));
    }
    
    // Delete from FTS
    if (fts_available_) {
        const char* fts_sql = "DELETE FROM memory_index_chunks_fts WHERE path = ?";
        sqlite3_stmt* fts_stmt = prepare_stmt(db_, fts_sql);
        if (fts_stmt) {
            sqlite3_bind_text(fts_stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(fts_stmt);
            sqlite3_finalize(fts_stmt);
        }
    }
    
    return core::Result<int>::Ok(changes);
}

core::Result<int> SQLiteVectorStorage::delete_chunks_by_source(MemorySource source) {
    if (!db_) {
        return core::Result<int>::Err("Database not open");
    }
    
    const char* sql = "DELETE FROM memory_index_chunks_vec WHERE source = ?";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<int>::Err("Failed to prepare delete: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, to_string(source).c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        return core::Result<int>::Err("Failed to delete chunks: " + std::string(sqlite3_errmsg(db_)));
    }
    
    // Delete from FTS
    if (fts_available_) {
        const char* fts_sql = "DELETE FROM memory_index_chunks_fts WHERE source = ?";
        sqlite3_stmt* fts_stmt = prepare_stmt(db_, fts_sql);
        if (fts_stmt) {
            sqlite3_bind_text(fts_stmt, 1, to_string(source).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(fts_stmt);
            sqlite3_finalize(fts_stmt);
        }
    }
    
    return core::Result<int>::Ok(changes);
}

core::Result<bool> SQLiteVectorStorage::clear_all() {
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    std::string error;
    if (!::hunterclaw::memory::exec_sql(db_, "DELETE FROM memory_index_chunks_vec", &error)) {
        return core::Result<bool>::Err("Failed to clear memory_index_chunks_vec: " + error);
    }
    
    if (fts_available_) {
        ::hunterclaw::memory::exec_sql(db_, "DELETE FROM memory_index_chunks_fts");
    }
    
    ::hunterclaw::memory::exec_sql(db_, "DELETE FROM memory_embedding_cache");
    
    return core::Result<bool>::Ok(true);
}

// ============================================================================
// Vector Search
// ============================================================================

MemorySearchResult SQLiteVectorStorage::result_from_row(sqlite3_stmt* stmt, double score) {
    MemorySearchResult result;
    result.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    
    std::string source_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    result.source = (source_str == "memory") ? MemorySource::Memory : MemorySource::Sessions;
    
    result.start_line = sqlite3_column_int(stmt, 2);
    result.end_line = sqlite3_column_int(stmt, 3);
    result.snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    result.score = score;
    
    return result;
}

double SQLiteVectorStorage::bm25_rank_to_score(double rank) {
    // 严格对齐 OpenClaw hybrid.ts::bm25RankToScore
    if (!std::isfinite(rank)) {
        return 1.0 / (1.0 + 999.0);
    }
    if (rank < 0) {
        double relevance = -rank;
        return relevance / (1.0 + relevance);
    }
    return 1.0 / (1.0 + rank);
}

core::Result<std::vector<MemorySearchResult>> SQLiteVectorStorage::vector_search(
    const std::vector<float>& query_embedding,
    int limit,
    double min_score) {
    
    if (!db_) {
        return core::Result<std::vector<MemorySearchResult>>::Err("Database not open");
    }
    
    // 对齐 openclaw: 向量搜索通过 manager_search.cc 的 search_vector_impl 实现
    // 这里提供一个简单的 fallback: 内存余弦相似度
    const char* sql = "SELECT id, path, start_line, end_line, text, embedding, source FROM memory_index_chunks WHERE model != ''";
    sqlite3_stmt* stmt = prepare_stmt(db_, sql);
    if (!stmt) return core::Result<std::vector<MemorySearchResult>>::Ok({});

    auto cosine_sim = [](const std::vector<float>& a, const std::vector<float>& b) -> double {
        if (a.size() != b.size() || a.empty()) return 0.0;
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            dot += a[i]*b[i]; na += a[i]*a[i]; nb += b[i]*b[i];
        }
        return (na == 0.0 || nb == 0.0) ? 0.0 : dot / (std::sqrt(na) * std::sqrt(nb));
    };

    struct Cand { MemorySearchResult r; double s; };
    std::vector<Cand> cands;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* emb_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        if (!emb_text) continue;
        std::string emb_str(emb_text);
        std::vector<float> emb;
        if (!emb_str.empty() && emb_str[0] == '[') {
            std::string s = emb_str.substr(1, emb_str.size()-2);
            std::istringstream iss(s); std::string tok;
            while (std::getline(iss, tok, ',')) { try { emb.push_back(std::stof(tok)); } catch (...) {} }
        }
        double score = cosine_sim(query_embedding, emb);
        if (score < min_score || !std::isfinite(score)) continue;
        MemorySearchResult res;
        res.path       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        res.start_line = sqlite3_column_int(stmt, 2);
        res.end_line   = sqlite3_column_int(stmt, 3);
        res.snippet    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        std::string src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        res.source     = (src == "sessions") ? MemorySource::Sessions : MemorySource::Memory;
        res.score      = score;
        cands.push_back({std::move(res), score});
    }
    sqlite3_finalize(stmt);
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b){ return a.s > b.s; });
    std::vector<MemorySearchResult> results;
    for (size_t i = 0; i < cands.size() && static_cast<int>(i) < limit; ++i)
        results.push_back(std::move(cands[i].r));
    return core::Result<std::vector<MemorySearchResult>>::Ok(std::move(results));
}

core::Result<std::vector<MemorySearchResult>> SQLiteVectorStorage::vector_search_filtered(
    const std::vector<float>& query_embedding,
    int limit,
    double min_score,
    const std::string& source_filter) {
    
    // 对齐 openclaw: 带 source 过滤的向量搜索
    // 利用 vector_search 的结果再过滤
    auto res = vector_search(query_embedding, limit * 2, min_score);
    if (res.is_err()) return res;
    std::vector<MemorySearchResult> filtered;
    for (auto& r : res.get()) {
        std::string src = (r.source == MemorySource::Sessions) ? "sessions" : "memory";
        if (source_filter.empty() || src == source_filter) {
            filtered.push_back(std::move(r));
            if (static_cast<int>(filtered.size()) >= limit) break;
        }
    }
    return core::Result<std::vector<MemorySearchResult>>::Ok(std::move(filtered));
}

// ============================================================================
// Full-Text Search
// ============================================================================

core::Result<std::vector<MemorySearchResult>> SQLiteVectorStorage::fts_search(
    const std::string& query,
    int limit) {
    
    if (!db_) {
        return core::Result<std::vector<MemorySearchResult>>::Err("Database not open");
    }
    
    if (!fts_available_) {
        return core::Result<std::vector<MemorySearchResult>>::Err("FTS not available");
    }
    
    const char* sql = R"(
        SELECT f.path, f.source, f.start_line, f.end_line, f.content,
               bm25(f) as rank
        FROM memory_index_chunks_fts f
        WHERE memory_index_chunks_fts MATCH ?
        ORDER BY rank
        LIMIT ?
    )";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<std::vector<MemorySearchResult>>::Err("Failed to prepare FTS search: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    
    std::vector<MemorySearchResult> results;
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        double rank = sqlite3_column_double(stmt, 5);
        double score = bm25_rank_to_score(rank);
        results.push_back(result_from_row(stmt, score));
    }
    
    sqlite3_finalize(stmt);
    
    return core::Result<std::vector<MemorySearchResult>>::Ok(results);
}

core::Result<std::vector<MemorySearchResult>> SQLiteVectorStorage::fts_search_filtered(
    const std::string& query,
    int limit,
    const std::string& source_filter) {
    
    if (!db_) {
        return core::Result<std::vector<MemorySearchResult>>::Err("Database not open");
    }
    
    if (!fts_available_) {
        return core::Result<std::vector<MemorySearchResult>>::Err("FTS not available");
    }
    
    const char* sql = R"(
        SELECT f.path, f.source, f.start_line, f.end_line, f.content,
               bm25(f) as rank
        FROM memory_index_chunks_fts f
        WHERE memory_index_chunks_fts MATCH ? AND source = ?
        ORDER BY rank
        LIMIT ?
    )";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<std::vector<MemorySearchResult>>::Err("Failed to prepare FTS search: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, source_filter.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);
    
    std::vector<MemorySearchResult> results;
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        double rank = sqlite3_column_double(stmt, 5);
        double score = bm25_rank_to_score(rank);
        results.push_back(result_from_row(stmt, score));
    }
    
    sqlite3_finalize(stmt);
    
    return core::Result<std::vector<MemorySearchResult>>::Ok(results);
}

// ============================================================================
// Embedding Cache
// ============================================================================

core::Result<std::optional<std::vector<float>>> SQLiteVectorStorage::get_cached_embedding(
    const std::string& content_hash,
    const std::string& model) {
    
    if (!db_) {
        return core::Result<std::optional<std::vector<float>>>::Err("Database not open");
    }
    
    // 对齐 openclaw memory_embedding_cache 表结构: (provider, model, provider_key, hash, embedding, dims, updated_at)
    // provider_key = provider + ":" + model
    const char* sql = "SELECT embedding FROM memory_embedding_cache WHERE hash = ? AND model = ?";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<std::optional<std::vector<float>>>::Err("Failed to prepare cache query: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, model.c_str(), -1, SQLITE_TRANSIENT);
    
    std::optional<std::vector<float>> result;
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // 对齐 openclaw: embedding 存储为 JSON array 字符串
        const char* emb_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (emb_text) {
            // Try JSON parse first, then blob fallback
            std::string emb_str(emb_text);
            if (!emb_str.empty() && emb_str[0] == '[') {
                // JSON array format
                std::vector<float> embedding;
                // Simple JSON float array parser
                std::string s = emb_str.substr(1, emb_str.size() - 2);
                std::istringstream iss(s);
                std::string token;
                while (std::getline(iss, token, ',')) {
                    try { embedding.push_back(std::stof(token)); } catch (...) {}
                }
                if (!embedding.empty()) result = embedding;
            } else {
                // Blob format
                int blob_size = sqlite3_column_bytes(stmt, 0);
                const char* blob_data = static_cast<const char*>(sqlite3_column_blob(stmt, 0));
                if (blob_data && blob_size > 0) {
                    std::string blob(blob_data, blob_size);
                    result = deserialize_embedding(blob);
                }
            }
        }
    }
    
    sqlite3_finalize(stmt);
    
    return core::Result<std::optional<std::vector<float>>>::Ok(result);
}

core::Result<bool> SQLiteVectorStorage::cache_embedding(
    const std::string& content_hash,
    const std::vector<float>& embedding,
    const std::string& provider_key) {
    
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    // 对齐 openclaw memory_embedding_cache 表结构:
    // (provider, model, provider_key, hash, embedding, dims, updated_at)
    // provider_key 格式: "provider:model"
    std::string provider, model;
    auto colon = provider_key.find(':');
    if (colon != std::string::npos) {
        provider = provider_key.substr(0, colon);
        model = provider_key.substr(colon + 1);
    } else {
        provider = provider_key;
        model = provider_key;
    }

    // 对齐 openclaw: embedding 存储为 JSON array 字符串
    std::ostringstream emb_json;
    emb_json << "[";
    for (size_t i = 0; i < embedding.size(); ++i) {
        if (i > 0) emb_json << ",";
        emb_json << embedding[i];
    }
    emb_json << "]";
    std::string emb_str = emb_json.str();

    const char* sql = R"(
        INSERT OR REPLACE INTO memory_embedding_cache
        (provider, model, provider_key, hash, embedding, dims, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    )";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<bool>::Err("Failed to prepare cache insert: " + error);
    }
    
    int64_t updated_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_text(stmt, 1, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, provider_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, emb_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, static_cast<int>(embedding.size()));
    sqlite3_bind_int64(stmt, 7, updated_at);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        return core::Result<bool>::Err("Failed to cache embedding: " + std::string(sqlite3_errmsg(db_)));
    }
    
    return core::Result<bool>::Ok(true);
}

core::Result<int> SQLiteVectorStorage::cleanup_old_cache(int64_t older_than_timestamp) {
    if (!db_) {
        return core::Result<int>::Err("Database not open");
    }
    
    // 对齐 openclaw: updated_at 字段
    const char* sql = "DELETE FROM memory_embedding_cache WHERE updated_at < ?";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<int>::Err("Failed to prepare cache cleanup: " + error);
    }
    
    sqlite3_bind_int64(stmt, 1, older_than_timestamp);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        return core::Result<int>::Err("Failed to cleanup cache: " + std::string(sqlite3_errmsg(db_)));
    }
    
    return core::Result<int>::Ok(changes);
}

// ============================================================================
// Statistics
// ============================================================================

core::Result<SQLiteVectorStorage::Stats> SQLiteVectorStorage::get_stats() {
    if (!db_) {
        return core::Result<Stats>::Err("Database not open");
    }
    
    Stats stats{};
    
    // Count total chunks (对齐 openclaw: SELECT COUNT(*) as c FROM memory_index_chunks)
    const char* count_sql = "SELECT COUNT(*) FROM memory_index_chunks";
    sqlite3_stmt* stmt = prepare_stmt(db_, count_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_chunks = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    // Count by source
    const char* memory_sql = "SELECT COUNT(*) FROM memory_index_chunks WHERE source = 'memory'";
    stmt = prepare_stmt(db_, memory_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.memory_chunks = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    const char* sessions_sql = "SELECT COUNT(*) FROM memory_index_chunks WHERE source = 'sessions'";
    stmt = prepare_stmt(db_, sessions_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.sessions_chunks = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    // Count unique files (对齐 openclaw: SELECT COUNT(*) as c FROM memory_index_sources)
    const char* files_sql = "SELECT COUNT(*) FROM memory_index_sources";
    stmt = prepare_stmt(db_, files_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_files = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    // Count cache entries
    const char* cache_sql = "SELECT COUNT(*) FROM memory_embedding_cache";
    stmt = prepare_stmt(db_, cache_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.cache_entries = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    // Get DB file size
    std::error_code ec;
    auto file_size = std::filesystem::file_size(config_.db_path, ec);
    if (!ec) {
        stats.db_size_bytes = file_size;
    }
    
    return core::Result<Stats>::Ok(stats);
}

core::Result<std::vector<MemoryProviderStatus::SourceCount>> SQLiteVectorStorage::get_source_counts() {
    if (!db_) {
        return core::Result<std::vector<MemoryProviderStatus::SourceCount>>::Err("Database not open");
    }
    
    // 对齐 openclaw: SELECT source, COUNT(*) as c FROM memory_index_sources GROUP BY source
    const char* sql = "SELECT source, COUNT(DISTINCT path) as files, COUNT(*) as chunks FROM memory_index_chunks GROUP BY source";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<std::vector<MemoryProviderStatus::SourceCount>>::Err("Failed to query source counts: " + error);
    }
    
    std::vector<MemoryProviderStatus::SourceCount> counts;
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MemoryProviderStatus::SourceCount count;
        std::string source_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        count.source = (source_str == "memory") ? MemorySource::Memory : MemorySource::Sessions;
        count.files = sqlite3_column_int(stmt, 1);
        count.chunks = sqlite3_column_int(stmt, 2);
        counts.push_back(count);
    }
    
    sqlite3_finalize(stmt);
    
    return core::Result<std::vector<MemoryProviderStatus::SourceCount>>::Ok(counts);
}

// 🎯 对齐 OC: 读取索引 revision（memory_index_state.revision, id=1）。
core::Result<int64_t> SQLiteVectorStorage::get_index_revision() {
    if (!db_) {
        return core::Result<int64_t>::Err("Database not open");
    }
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_,
        "SELECT revision FROM memory_index_state WHERE id = 1", &error);
    if (!stmt) {
        return core::Result<int64_t>::Err("Failed to query index revision: " + error);
    }
    int64_t revision = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        revision = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return core::Result<int64_t>::Ok(revision);
}

// 🎯 对齐 OC readMemoryRecallMetadata: 批量读取 chunk 召回元数据（left join）。
core::Result<std::map<std::string, SQLiteVectorStorage::RecallMetadata>>
SQLiteVectorStorage::get_recall_metadata(const std::vector<std::string>& chunk_ids) {
    std::map<std::string, RecallMetadata> result;
    if (!db_) {
        return core::Result<std::map<std::string, RecallMetadata>>::Err("Database not open");
    }
    if (chunk_ids.empty()) {
        return core::Result<std::map<std::string, RecallMetadata>>::Ok(result);
    }
    // 构造 IN (?, ?, ...) 占位符
    std::string placeholders;
    for (size_t i = 0; i < chunk_ids.size(); ++i) {
        if (i) placeholders += ",";
        placeholders += "?";
    }
    std::string sql =
        "SELECT chunk.id, metadata.importance, metadata.triggers, metadata.project_key, "
        "       prov.origin_class "
        "FROM memory_index_chunks AS chunk "
        "LEFT JOIN memory_index_chunk_recall_metadata AS metadata ON metadata.chunk_id = chunk.id "
        "LEFT JOIN memory_index_chunk_provenance AS prov ON prov.chunk_id = chunk.id "
        "WHERE chunk.id IN (" + placeholders + ")";

    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<std::map<std::string, RecallMetadata>>::Err(
            "Failed to query recall metadata: " + error);
    }
    for (size_t i = 0; i < chunk_ids.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), chunk_ids[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!cid) continue;
        RecallMetadata meta;
        if (sqlite3_column_type(stmt, 1) != SQLITE_NULL)
            meta.importance = sqlite3_column_int(stmt, 1);
        if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (t) meta.triggers = std::string(t);
        }
        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (p) meta.project_key = std::string(p);
        }
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
            const char* o = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            if (o) meta.origin_class = std::string(o);
        }
        result[std::string(cid)] = std::move(meta);
    }
    sqlite3_finalize(stmt);
    return core::Result<std::map<std::string, RecallMetadata>>::Ok(result);
}

// 🎯 对齐 OC readCuratedMemoryCandidates + readCuratedCandidateBatch
//   (memory-recall-metadata.ts) 的通用内部实现。keyset 分页拉取curated chunk，
//   按 require_triggers / require_project 约束 SQL 与资格过滤后填满 limit。
//   trigger 版：require_triggers=true, require_project=false。
//   project 版：require_project=true, require_triggers=false。
static core::Result<std::vector<MemorySearchResult>> list_curated_candidates_impl(
    sqlite3* db_,
    int limit,
    const std::optional<std::vector<std::string>>& active_project_keys,
    bool require_project,
    bool require_triggers) {
    std::vector<MemorySearchResult> results;
    if (!db_) {
        return core::Result<std::vector<MemorySearchResult>>::Err("Database not open");
    }
    if (limit <= 0) {
        return core::Result<std::vector<MemorySearchResult>>::Ok(results);
    }

    // 对齐 OC INVALID_PROJECT_ANNOTATION_KEY (internal.ts)
    static const std::string kInvalidProjectAnnotationKey = "!invalid-project-annotation";

    // 对齐 OC: active = new Set(keys.trim().filter(Boolean))；未提供时 active=undefined。
    std::optional<std::set<std::string>> active;
    if (active_project_keys.has_value()) {
        std::set<std::string> s;
        for (const auto& k : *active_project_keys) {
            std::string t = k;
            // trim
            size_t b = t.find_first_not_of(" \t\r\n");
            size_t e = t.find_last_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
            t = t.substr(b, e - b + 1);
            if (!t.empty()) s.insert(t);
        }
        active = std::move(s);
    }

    // 对齐 OC: batchSize = max(64, limit)
    const int batch_size = std::max(64, limit);

    // keyset 游标 (对齐 OC cursor: { importance, path, id })
    bool has_cursor = false;
    int cursor_importance = 0;  // coalesce(importance,0)
    std::string cursor_path;
    std::string cursor_id;

    // 拆分 project_key（";" 分隔、trim、去空）→ vector
    auto split_project_keys = [](const std::string& raw) -> std::vector<std::string> {
        std::vector<std::string> keys;
        std::string cur;
        for (char c : raw) {
            if (c == ';') {
                // trim cur
                size_t b = cur.find_first_not_of(" \t\r\n");
                size_t e = cur.find_last_not_of(" \t\r\n");
                if (b != std::string::npos) keys.push_back(cur.substr(b, e - b + 1));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        size_t b = cur.find_first_not_of(" \t\r\n");
        size_t e = cur.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) keys.push_back(cur.substr(b, e - b + 1));
        return keys;
    };

    while (static_cast<int>(results.size()) < limit) {
        // 对齐 OC readCuratedCandidateBatch SQL：
        //   requireTriggers=true → metadata.triggers IS NOT NULL
        //   requireProject=false（trigger 路径）
        //   ORDER BY coalesce(importance,0) DESC, path, id
        std::string sql =
            "SELECT chunk.id, chunk.path, chunk.source, chunk.start_line, chunk.end_line, chunk.text, "
            "       metadata.importance, metadata.triggers, metadata.project_key, prov.origin_class, "
            "       COALESCE(metadata.importance, 0) AS imp0 "
            "FROM memory_index_chunks AS chunk "
            "LEFT JOIN memory_index_chunk_recall_metadata AS metadata ON metadata.chunk_id = chunk.id "
            "LEFT JOIN memory_index_chunk_provenance AS prov ON prov.chunk_id = chunk.id "
            "WHERE chunk.source = 'memory' "
            "  AND chunk.path IN ('MEMORY.md', 'USER.md') ";
        // 对齐 OC readCuratedCandidateBatch：requireTriggers / requireProject 条件。
        if (require_triggers) {
            sql += "  AND metadata.triggers IS NOT NULL ";
        }
        if (require_project) {
            sql += "  AND metadata.project_key IS NOT NULL ";
        }
        if (has_cursor) {
            // 对齐 OC keyset：coalesce(importance,0) < c.imp0
            //   OR (= c.imp0 AND (path > c.path OR (path = c.path AND id > c.id)))
            // 注意：WHERE 不能引用 SELECT 列别名，需重复 COALESCE 表达式。
            sql +=
                "  AND ( COALESCE(metadata.importance, 0) < ? "
                "        OR ( COALESCE(metadata.importance, 0) = ? "
                "             AND ( chunk.path > ? "
                "                   OR ( chunk.path = ? AND chunk.id > ? ) ) ) ) ";
        }
        sql +=
            "ORDER BY imp0 DESC, chunk.path ASC, chunk.id ASC "
            "LIMIT ?";

        std::string error;
        sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
        if (!stmt) {
            return core::Result<std::vector<MemorySearchResult>>::Err(
                "Failed to query curated trigger candidates: " + error);
        }
        int idx = 1;
        if (has_cursor) {
            sqlite3_bind_int(stmt, idx++, cursor_importance);
            sqlite3_bind_int(stmt, idx++, cursor_importance);
            sqlite3_bind_text(stmt, idx++, cursor_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, idx++, cursor_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, idx++, cursor_id.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int(stmt, idx, batch_size);

        int row_count = 0;
        std::string last_path, last_id;
        int last_imp0 = 0;
        bool reached_limit = false;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ++row_count;
            std::string id   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            std::string path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            int start_line   = sqlite3_column_int(stmt, 3);
            int end_line     = sqlite3_column_int(stmt, 4);
            const char* text_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            std::string text = text_c ? text_c : "";

            std::optional<int> importance;
            if (sqlite3_column_type(stmt, 6) != SQLITE_NULL)
                importance = sqlite3_column_int(stmt, 6);
            std::optional<std::string> triggers;
            if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
                if (t) triggers = std::string(t);
            }
            std::optional<std::string> project_key;
            if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
                const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
                if (p) project_key = std::string(p);
            }
            std::optional<std::string> origin_class;
            if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
                const char* o = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
                if (o) origin_class = std::string(o);
            }
            int imp0 = sqlite3_column_int(stmt, 10);

            // 记录本批最后一行（用于游标推进），必须先于资格过滤 continue。
            last_path = path;
            last_id   = id;
            last_imp0 = imp0;

            // 对齐 OC资格过滤（在keyset 批之上应用）
            std::vector<std::string> stored_keys;
            if (project_key.has_value()) {
                stored_keys = split_project_keys(*project_key);
            }
            // 含 INVALID_PROJECT_ANNOTATION_KEY → 剔除
            if (std::find(stored_keys.begin(), stored_keys.end(),
                          kInvalidProjectAnnotationKey) != stored_keys.end()) {
                continue;
            }
            // 对齐 OC 条件：
            //   (active===undefined && !requireProject) ||
            //   (project_key===null && !requireProject) ||
            //   (storedKeys.length>0 && storedKeys.every(k => active.has(k)))
            bool eligible = false;
            if (!active.has_value() && !require_project) {
                eligible = true;
            } else if (!project_key.has_value() && !require_project) {
                eligible = true;
            } else if (!stored_keys.empty()) {
                bool all_in = active.has_value();  // active 为空集则 every→需active 存在
                if (all_in) {
                    for (const auto& k : stored_keys) {
                        if (active->find(k) == active->end()) { all_in = false; break; }
                    }
                }
                eligible = all_in;
            }
            if (!eligible) {
                continue;
            }

            // 对齐 OC toCuratedMemorySearchResults：score=0, snippet=text, source=memory；
            //importance/triggers/project_key trim 后写入。
            MemorySearchResult r;
            r.path= path;
            r.start_line = start_line;
            r.end_line   = end_line;
            r.score      = 0.0;
            r.snippet    = text;
            r.source     = MemorySource::Memory;
            r.id         = id;
            if (importance.has_value()) r.importance = importance;
            if (triggers.has_value()) {
                std::string t = *triggers;
                size_t b = t.find_first_not_of(" \t\r\n");
                size_t e = t.find_last_not_of(" \t\r\n");
                if (b != std::string::npos) r.triggers = t.substr(b, e - b + 1);
            }
            if (project_key.has_value()) {
                std::string p = *project_key;
                size_t b = p.find_first_not_of(" \t\r\n");
                size_t e = p.find_last_not_of(" \t\r\n");
                if (b != std::string::npos) r.project_key = p.substr(b, e - b + 1);
            }
            if (origin_class.has_value()) r.origin_class = origin_class;

            results.push_back(std::move(r));
            if (static_cast<int>(results.size()) == limit) {
                reached_limit = true;
                break;
            }
        }
        sqlite3_finalize(stmt);

        if (reached_limit) break;
        // 对齐 OC: if (!last || rows.length < batchSize) break;
        if (row_count == 0 || row_count < batch_size) break;
        // 推进游标
        has_cursor = true;
        cursor_importance = last_imp0;
        cursor_path = last_path;
        cursor_id = last_id;
    }

    return core::Result<std::vector<MemorySearchResult>>::Ok(std::move(results));
}

//🎯 对齐 OC readCuratedMemoryTriggerCandidates:
//   requireTriggers=true, requireProject=false。
core::Result<std::vector<MemorySearchResult>>
SQLiteVectorStorage::list_curated_trigger_candidates(
    int limit,
    const std::optional<std::vector<std::string>>& active_project_keys) {
    return list_curated_candidates_impl(
        db_, limit, active_project_keys,
        /*require_project=*/false, /*require_triggers=*/true);
}

// 🎯 对齐 OC readCuratedProjectMemoryCandidates:
//   requireProject=true, requireTriggers=false；active 为空直接返回空。
core::Result<std::vector<MemorySearchResult>>
SQLiteVectorStorage::list_curated_project_candidates(
    int limit,
    const std::vector<std::string>& active_project_keys) {
    // 对齐 OC: if (activeProjectKeys.length === 0) return [];
    if (active_project_keys.empty()) {
        return core::Result<std::vector<MemorySearchResult>>::Ok({});
    }
    return list_curated_candidates_impl(
        db_, limit, active_project_keys,
        /*require_project=*/true, /*require_triggers=*/false);
}

core::Result<bool> SQLiteVectorStorage::begin_transaction() {
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    std::string error;
    if (!::hunterclaw::memory::exec_sql(db_, "BEGIN TRANSACTION", &error)) {
        return core::Result<bool>::Err("Failed to begin transaction: " + error);
    }
    return core::Result<bool>::Ok(true);
}

core::Result<bool> SQLiteVectorStorage::commit_transaction() {
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    std::string error;
    if (!::hunterclaw::memory::exec_sql(db_, "COMMIT", &error)) {
        return core::Result<bool>::Err("Failed to commit transaction: " + error);
    }
    return core::Result<bool>::Ok(true);
}

core::Result<bool> SQLiteVectorStorage::rollback_transaction() {
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    std::string error;
    if (!::hunterclaw::memory::exec_sql(db_, "ROLLBACK", &error)) {
        return core::Result<bool>::Err("Failed to rollback transaction: " + error);
    }
    return core::Result<bool>::Ok(true);
}

core::Result<bool> SQLiteVectorStorage::exec_sql(const std::string& sql) {
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    std::string error;
    if (!::hunterclaw::memory::exec_sql(db_, sql, &error)) {
        return core::Result<bool>::Err(error);
    }
    return core::Result<bool>::Ok(true);
}

core::Result<sqlite3_stmt*> SQLiteVectorStorage::prepare_statement(const std::string& sql) {
    if (!db_) {
        return core::Result<sqlite3_stmt*>::Err("Database not open");
    }
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<sqlite3_stmt*>::Err(error);
    }
    return core::Result<sqlite3_stmt*>::Ok(stmt);
}

// ============================================================================
// Transaction RAII Helper
// ============================================================================

SQLiteVectorStorage::Transaction::Transaction(SQLiteVectorStorage& storage)
    : storage_(storage), committed_(false) {
    storage_.begin_transaction();
}

SQLiteVectorStorage::Transaction::~Transaction() {
    if (!committed_) {
        storage_.rollback_transaction();
    }
}

core::Result<bool> SQLiteVectorStorage::Transaction::commit() {
    auto res = storage_.commit_transaction();
    if (res.is_ok()) {
        committed_ = true;
    }
    return res;
}

} // namespace memory
} // namespace hunterclaw
