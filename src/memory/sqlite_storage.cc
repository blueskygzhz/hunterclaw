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

namespace hunterclaw {
namespace memory {

namespace {

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
            storage->fts_available_ = fts_res.value;
        }
    }
    
    // Load sqlite-vec extension if provided
    if (config.vec_extension_path.has_value() && config.enable_vector) {
        auto vec_res = storage->load_sqlite_vec_extension();
        if (!vec_res.is_ok()) {
            storage->vec_error_ = vec_res.error;
            storage->vec_available_ = false;
        } else {
            storage->vec_available_ = vec_res.value;
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
    
    // Create chunks_vec table using SCHEMA_CHUNKS_VEC
    if (!::hunterclaw::memory::exec_sql(db_, SCHEMA_CHUNKS_VEC, &error)) {
        return core::Result<bool>::Err("Failed to create chunks_vec: " + error);
    }
    
    // Create FTS table using SCHEMA_CHUNKS_FTS if FTS available
    if (fts_available_) {
        if (!::hunterclaw::memory::exec_sql(db_, SCHEMA_CHUNKS_FTS, &error)) {
            // Don't fail if FTS creation fails, just mark as unavailable
            fts_available_ = false;
            fts_error_ = error;
        }
    }
    
    // Create embedding_cache table
    if (!::hunterclaw::memory::exec_sql(db_, SCHEMA_EMBEDDING_CACHE, &error)) {
        return core::Result<bool>::Err("Failed to create embedding_cache: " + error);
    }
    
    // Create indices
    ::hunterclaw::memory::exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_vec_path ON chunks_vec(path)");
    ::hunterclaw::memory::exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_vec_source ON chunks_vec(source)");
    
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
    if (!::hunterclaw::memory::exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_vec_path ON chunks_vec(path)", &error)) {
        return core::Result<bool>::Err("Failed to create index: " + error);
    }
    
    if (!::hunterclaw::memory::exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_vec_source ON chunks_vec(source)", &error)) {
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
    
    const char* sql = R"(
        INSERT OR REPLACE INTO chunks_vec 
        (id, path, source, model, start_line, end_line, content, embedding, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<std::string>::Err("Failed to prepare insert: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, to_string(metadata.source).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, metadata.model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, metadata.start_line);
    sqlite3_bind_int(stmt, 6, metadata.end_line);
    sqlite3_bind_text(stmt, 7, content.c_str(), -1, SQLITE_TRANSIENT);
    
    std::string blob = serialize_embedding(embedding);
    sqlite3_bind_blob(stmt, 8, blob.data(), blob.size(), SQLITE_TRANSIENT);
    
    int64_t created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, 9, created_at);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        return core::Result<std::string>::Err("Failed to insert chunk: " + std::string(sqlite3_errmsg(db_)));
    }
    
    // Update FTS if available
    if (fts_available_) {
        const char* fts_sql = R"(
            INSERT INTO chunks_fts (id, path, source, model, start_line, end_line, content)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        )";
        
        sqlite3_stmt* fts_stmt = prepare_stmt(db_, fts_sql);
        if (fts_stmt) {
            sqlite3_bind_text(fts_stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(fts_stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(fts_stmt, 3, to_string(metadata.source).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(fts_stmt, 4, metadata.model.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(fts_stmt, 5, metadata.start_line);
            sqlite3_bind_int(fts_stmt, 6, metadata.end_line);
            sqlite3_bind_text(fts_stmt, 7, content.c_str(), -1, SQLITE_TRANSIENT);
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
        ids.push_back(res.value);
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
    
    const char* sql = "DELETE FROM chunks_vec WHERE path = ?";
    
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
        const char* fts_sql = "DELETE FROM chunks_fts WHERE path = ?";
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
    
    const char* sql = "DELETE FROM chunks_vec WHERE source = ?";
    
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
        const char* fts_sql = "DELETE FROM chunks_fts WHERE source = ?";
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
    if (!::hunterclaw::memory::exec_sql(db_, "DELETE FROM chunks_vec", &error)) {
        return core::Result<bool>::Err("Failed to clear chunks_vec: " + error);
    }
    
    if (fts_available_) {
        ::hunterclaw::memory::exec_sql(db_, "DELETE FROM chunks_fts");
    }
    
    ::hunterclaw::memory::exec_sql(db_, "DELETE FROM embedding_cache");
    
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
    
    if (!vec_available_) {
        return core::Result<std::vector<MemorySearchResult>>::Err("Vector search not available");
    }
    
    // TODO: Implement vector search using sqlite-vec
    // For now, return empty results
    return core::Result<std::vector<MemorySearchResult>>::Ok(std::vector<MemorySearchResult>());
}

core::Result<std::vector<MemorySearchResult>> SQLiteVectorStorage::vector_search_filtered(
    const std::vector<float>& query_embedding,
    int limit,
    double min_score,
    const std::string& source_filter) {
    
    // TODO: Implement filtered vector search
    return vector_search(query_embedding, limit, min_score);
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
        FROM chunks_fts f
        WHERE chunks_fts MATCH ?
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
        FROM chunks_fts f
        WHERE chunks_fts MATCH ? AND source = ?
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
    
    const char* sql = "SELECT embedding FROM embedding_cache WHERE content_hash = ? AND model = ?";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<std::optional<std::vector<float>>>::Err("Failed to prepare cache query: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, model.c_str(), -1, SQLITE_TRANSIENT);
    
    std::optional<std::vector<float>> result;
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int blob_size = sqlite3_column_bytes(stmt, 0);
        const char* blob_data = static_cast<const char*>(sqlite3_column_blob(stmt, 0));
        std::string blob(blob_data, blob_size);
        result = deserialize_embedding(blob);
    }
    
    sqlite3_finalize(stmt);
    
    return core::Result<std::optional<std::vector<float>>>::Ok(result);
}

core::Result<bool> SQLiteVectorStorage::cache_embedding(
    const std::string& content_hash,
    const std::vector<float>& embedding,
    const std::string& model) {
    
    if (!db_) {
        return core::Result<bool>::Err("Database not open");
    }
    
    const char* sql = R"(
        INSERT OR REPLACE INTO embedding_cache (content_hash, model, embedding, created_at)
        VALUES (?, ?, ?, ?)
    )";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return core::Result<bool>::Err("Failed to prepare cache insert: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, model.c_str(), -1, SQLITE_TRANSIENT);
    
    std::string blob = serialize_embedding(embedding);
    sqlite3_bind_blob(stmt, 3, blob.data(), blob.size(), SQLITE_TRANSIENT);
    
    int64_t created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, 4, created_at);
    
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
    
    const char* sql = "DELETE FROM embedding_cache WHERE created_at < ?";
    
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
    
    // Count total chunks
    const char* count_sql = "SELECT COUNT(*) FROM chunks_vec";
    sqlite3_stmt* stmt = prepare_stmt(db_, count_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_chunks = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    // Count by source
    const char* memory_sql = "SELECT COUNT(*) FROM chunks_vec WHERE source = 'memory'";
    stmt = prepare_stmt(db_, memory_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.memory_chunks = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    const char* sessions_sql = "SELECT COUNT(*) FROM chunks_vec WHERE source = 'sessions'";
    stmt = prepare_stmt(db_, sessions_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.sessions_chunks = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    // Count unique files
    const char* files_sql = "SELECT COUNT(DISTINCT path) FROM chunks_vec";
    stmt = prepare_stmt(db_, files_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_files = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    // Count cache entries
    const char* cache_sql = "SELECT COUNT(*) FROM embedding_cache";
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
    
    const char* sql = "SELECT source, COUNT(DISTINCT path), COUNT(*) FROM chunks_vec GROUP BY source";
    
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

// ============================================================================
// Transaction Support
// ============================================================================

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
