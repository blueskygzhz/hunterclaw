#include "hunterclaw/memory/sqlite_storage.h"
#include <sqlite3.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cmath>

namespace hunterclaw {
namespace memory {

namespace {

// Schema constants - 严格对齐 OpenClaw
const char* VECTOR_TABLE = "chunks_vec";
const char* FTS_TABLE = "chunks_fts";
const char* EMBEDDING_CACHE_TABLE = "embedding_cache";
const int SNIPPET_MAX_CHARS = 700;

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
// SQLiteStorage Implementation
// ============================================================================

SQLiteStorage::SQLiteStorage(const std::string& db_path)
    : db_(nullptr), db_path_(db_path) {}

SQLiteStorage::~SQLiteStorage() {
    close();
}

Result<void> SQLiteStorage::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (db_) {
        return Result<void>::ok();
    }
    
    // Ensure parent directory exists
    std::filesystem::path path(db_path_);
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return Result<void>::error("Failed to create directory: " + ec.message());
        }
    }
    
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return Result<void>::error("Failed to open database: " + err);
    }
    
    // Enable WAL mode for better concurrency
    exec_sql(db_, "PRAGMA journal_mode=WAL");
    
    return Result<void>::ok();
}

void SQLiteStorage::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

Result<void> SQLiteStorage::ensure_schema() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<void>::error("Database not open");
    }
    
    std::string error;
    
    // Create chunks_vec table (严格对齐 OpenClaw schema)
    std::string create_vec = R"(
        CREATE TABLE IF NOT EXISTS chunks_vec (
            id TEXT PRIMARY KEY,
            path TEXT NOT NULL,
            start_line INTEGER NOT NULL,
            end_line INTEGER NOT NULL,
            source TEXT NOT NULL,
            snippet TEXT NOT NULL,
            embedding BLOB,
            provider_key TEXT,
            modified_ms INTEGER,
            UNIQUE(path, start_line, end_line, source)
        )
    )";
    
    if (!exec_sql(db_, create_vec, &error)) {
        return Result<void>::error("Failed to create chunks_vec: " + error);
    }
    
    // Create FTS table (严格对齐 OpenClaw FTS5 schema)
    std::string create_fts = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
            id UNINDEXED,
            path UNINDEXED,
            start_line UNINDEXED,
            end_line UNINDEXED,
            source UNINDEXED,
            snippet,
            content='chunks_vec',
            content_rowid='rowid'
        )
    )";
    
    if (!exec_sql(db_, create_fts, &error)) {
        return Result<void>::error("Failed to create chunks_fts: " + error);
    }
    
    // Create embedding_cache table
    std::string create_cache = R"(
        CREATE TABLE IF NOT EXISTS embedding_cache (
            text TEXT NOT NULL,
            provider_key TEXT NOT NULL,
            embedding BLOB NOT NULL,
            created_ms INTEGER NOT NULL,
            PRIMARY KEY (text, provider_key)
        )
    )";
    
    if (!exec_sql(db_, create_cache, &error)) {
        return Result<void>::error("Failed to create embedding_cache: " + error);
    }
    
    // Create indices
    exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_vec_path ON chunks_vec(path)");
    exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_chunks_vec_source ON chunks_vec(source)");
    
    return Result<void>::ok();
}

Result<bool> SQLiteStorage::load_vector_extension(const std::string& extension_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<bool>::error("Database not open");
    }
    
    // Enable extension loading
    int rc = sqlite3_enable_load_extension(db_, 1);
    if (rc != SQLITE_OK) {
        return Result<bool>::error("Failed to enable extensions: " + std::string(sqlite3_errmsg(db_)));
    }
    
    // Load extension
    char* err_msg = nullptr;
    rc = sqlite3_load_extension(db_, extension_path.c_str(), nullptr, &err_msg);
    
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : sqlite3_errmsg(db_);
        sqlite3_free(err_msg);
        return Result<bool>::ok_value(false); // Extension not available, not an error
    }
    
    return Result<bool>::ok_value(true);
}

Result<void> SQLiteStorage::create_vector_index(int dimensions) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<void>::error("Database not open");
    }
    
    std::string sql = "CREATE VIRTUAL TABLE IF NOT EXISTS vec_index USING vec0("
                      "embedding float[" + std::to_string(dimensions) + "], "
                      "+id TEXT)";
    
    std::string error;
    if (!exec_sql(db_, sql, &error)) {
        return Result<void>::error("Failed to create vector index: " + error);
    }
    
    return Result<void>::ok();
}

Result<void> SQLiteStorage::insert_chunk(const ChunkRecord& chunk) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<void>::error("Database not open");
    }
    
    const char* sql = R"(
        INSERT OR REPLACE INTO chunks_vec 
        (id, path, start_line, end_line, source, snippet, embedding, provider_key, modified_ms)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return Result<void>::error("Failed to prepare insert: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, chunk.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, chunk.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, chunk.start_line);
    sqlite3_bind_int(stmt, 4, chunk.end_line);
    sqlite3_bind_text(stmt, 5, to_string(chunk.source).c_str(), -1, SQLITE_TRANSIENT);
    
    std::string snippet = chunk.snippet.size() > SNIPPET_MAX_CHARS 
        ? chunk.snippet.substr(0, SNIPPET_MAX_CHARS) 
        : chunk.snippet;
    sqlite3_bind_text(stmt, 6, snippet.c_str(), -1, SQLITE_TRANSIENT);
    
    if (chunk.embedding.has_value()) {
        const auto& emb = chunk.embedding.value();
        sqlite3_bind_blob(stmt, 7, emb.data(), emb.size() * sizeof(float), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 7);
    }
    
    if (chunk.provider_key.has_value()) {
        sqlite3_bind_text(stmt, 8, chunk.provider_key.value().c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 8);
    }
    
    if (chunk.modified_ms.has_value()) {
        sqlite3_bind_int64(stmt, 9, chunk.modified_ms.value());
    } else {
        sqlite3_bind_null(stmt, 9);
    }
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        return Result<void>::error("Failed to insert chunk: " + std::string(sqlite3_errmsg(db_)));
    }
    
    // Update FTS index
    update_fts_for_chunk(chunk);
    
    return Result<void>::ok();
}

Result<void> SQLiteStorage::insert_chunks_batch(const std::vector<ChunkRecord>& chunks) {
    if (chunks.empty()) {
        return Result<void>::ok();
    }
    
    Transaction tx(this);
    auto tx_res = tx.begin();
    if (!tx_res.is_ok()) {
        return tx_res;
    }
    
    for (const auto& chunk : chunks) {
        auto res = insert_chunk(chunk);
        if (!res.is_ok()) {
            return res;
        }
    }
    
    return tx.commit();
}

Result<std::vector<ChunkRecord>> SQLiteStorage::get_chunks_by_path(
    const std::string& path, 
    MemorySource source) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<std::vector<ChunkRecord>>::error("Database not open");
    }
    
    const char* sql = "SELECT id, path, start_line, end_line, source, snippet, "
                      "embedding, provider_key, modified_ms FROM chunks_vec "
                      "WHERE path = ? AND source = ?";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return Result<std::vector<ChunkRecord>>::error("Failed to prepare query: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, to_string(source).c_str(), -1, SQLITE_TRANSIENT);
    
    std::vector<ChunkRecord> results;
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChunkRecord chunk;
        chunk.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        chunk.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        chunk.start_line = sqlite3_column_int(stmt, 2);
        chunk.end_line = sqlite3_column_int(stmt, 3);
        
        std::string source_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        chunk.source = (source_str == "memory") ? MemorySource::Memory : MemorySource::Sessions;
        
        chunk.snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        
        if (sqlite3_column_type(stmt, 6) == SQLITE_BLOB) {
            int blob_size = sqlite3_column_bytes(stmt, 6);
            const float* blob_data = static_cast<const float*>(sqlite3_column_blob(stmt, 6));
            chunk.embedding = std::vector<float>(blob_data, blob_data + blob_size / sizeof(float));
        }
        
        if (sqlite3_column_type(stmt, 7) == SQLITE_TEXT) {
            chunk.provider_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        }
        
        if (sqlite3_column_type(stmt, 8) == SQLITE_INTEGER) {
            chunk.modified_ms = sqlite3_column_int64(stmt, 8);
        }
        
        results.push_back(std::move(chunk));
    }
    
    sqlite3_finalize(stmt);
    
    return Result<std::vector<ChunkRecord>>::ok_value(std::move(results));
}

Result<void> SQLiteStorage::delete_chunks_by_path(const std::string& path, MemorySource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<void>::error("Database not open");
    }
    
    const char* sql = "DELETE FROM chunks_vec WHERE path = ? AND source = ?";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return Result<void>::error("Failed to prepare delete: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, to_string(source).c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        return Result<void>::error("Failed to delete chunks: " + std::string(sqlite3_errmsg(db_)));
    }
    
    // FTS will be updated via triggers or manually
    update_fts_after_delete(path, source);
    
    return Result<void>::ok();
}

Result<std::vector<VectorSearchResult>> SQLiteStorage::search_vector(
    const std::vector<float>& query_embedding,
    int limit,
    double min_score) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<std::vector<VectorSearchResult>>::error("Database not open");
    }
    
    // Query vector index (using sqlite-vec)
    const char* sql = R"(
        SELECT v.id, v.path, v.start_line, v.end_line, v.source, v.snippet, 
               vec_distance_cosine(i.embedding, ?) as distance,
               v.modified_ms
        FROM vec_index i
        JOIN chunks_vec v ON v.id = i.id
        WHERE vec_distance_cosine(i.embedding, ?) < ?
        ORDER BY distance
        LIMIT ?
    )";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return Result<std::vector<VectorSearchResult>>::error("Failed to prepare vector search: " + error);
    }
    
    // Bind query embedding as blob
    sqlite3_bind_blob(stmt, 1, query_embedding.data(), 
                      query_embedding.size() * sizeof(float), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, query_embedding.data(), 
                      query_embedding.size() * sizeof(float), SQLITE_TRANSIENT);
    
    // Convert cosine similarity threshold
    double distance_threshold = 1.0 - min_score;
    sqlite3_bind_double(stmt, 3, distance_threshold);
    sqlite3_bind_int(stmt, 4, limit);
    
    std::vector<VectorSearchResult> results;
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        VectorSearchResult result;
        result.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        result.start_line = sqlite3_column_int(stmt, 2);
        result.end_line = sqlite3_column_int(stmt, 3);
        
        std::string source_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result.source = (source_str == "memory") ? MemorySource::Memory : MemorySource::Sessions;
        
        result.snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        
        double distance = sqlite3_column_double(stmt, 6);
        result.vector_score = 1.0 - distance; // Convert distance to similarity
        
        if (sqlite3_column_type(stmt, 7) == SQLITE_INTEGER) {
            result.modified_ms = sqlite3_column_int64(stmt, 7);
        }
        
        results.push_back(std::move(result));
    }
    
    sqlite3_finalize(stmt);
    
    return Result<std::vector<VectorSearchResult>>::ok_value(std::move(results));
}

Result<std::vector<KeywordSearchResult>> SQLiteStorage::search_keyword(
    const std::string& query,
    int limit) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<std::vector<KeywordSearchResult>>::error("Database not open");
    }
    
    // Use FTS5 for keyword search
    const char* sql = R"(
        SELECT f.id, f.path, f.start_line, f.end_line, f.source, 
               f.snippet, bm25(f) as rank, v.modified_ms
        FROM chunks_fts f
        JOIN chunks_vec v ON v.id = f.id
        WHERE chunks_fts MATCH ?
        ORDER BY rank
        LIMIT ?
    )";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return Result<std::vector<KeywordSearchResult>>::error("Failed to prepare FTS search: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    
    std::vector<KeywordSearchResult> results;
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        KeywordSearchResult result;
        result.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        result.start_line = sqlite3_column_int(stmt, 2);
        result.end_line = sqlite3_column_int(stmt, 3);
        
        std::string source_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result.source = (source_str == "memory") ? MemorySource::Memory : MemorySource::Sessions;
        
        result.snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        
        double rank = sqlite3_column_double(stmt, 6);
        result.text_score = bm25_rank_to_score(rank);
        
        if (sqlite3_column_type(stmt, 7) == SQLITE_INTEGER) {
            result.modified_ms = sqlite3_column_int64(stmt, 7);
        }
        
        results.push_back(std::move(result));
    }
    
    sqlite3_finalize(stmt);
    
    return Result<std::vector<KeywordSearchResult>>::ok_value(std::move(results));
}

Result<std::optional<std::vector<float>>> SQLiteStorage::get_cached_embedding(
    const std::string& text,
    const std::string& provider_key) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<std::optional<std::vector<float>>>::error("Database not open");
    }
    
    const char* sql = "SELECT embedding FROM embedding_cache WHERE text = ? AND provider_key = ?";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return Result<std::optional<std::vector<float>>>::error("Failed to prepare cache query: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, provider_key.c_str(), -1, SQLITE_TRANSIENT);
    
    std::optional<std::vector<float>> result;
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int blob_size = sqlite3_column_bytes(stmt, 0);
        const float* blob_data = static_cast<const float*>(sqlite3_column_blob(stmt, 0));
        result = std::vector<float>(blob_data, blob_data + blob_size / sizeof(float));
    }
    
    sqlite3_finalize(stmt);
    
    return Result<std::optional<std::vector<float>>>::ok_value(result);
}

Result<void> SQLiteStorage::put_cached_embedding(
    const std::string& text,
    const std::string& provider_key,
    const std::vector<float>& embedding,
    int64_t created_ms) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<void>::error("Database not open");
    }
    
    const char* sql = R"(
        INSERT OR REPLACE INTO embedding_cache (text, provider_key, embedding, created_ms)
        VALUES (?, ?, ?, ?)
    )";
    
    std::string error;
    sqlite3_stmt* stmt = prepare_stmt(db_, sql, &error);
    if (!stmt) {
        return Result<void>::error("Failed to prepare cache insert: " + error);
    }
    
    sqlite3_bind_text(stmt, 1, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, provider_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, embedding.data(), embedding.size() * sizeof(float), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, created_ms);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        return Result<void>::error("Failed to cache embedding: " + std::string(sqlite3_errmsg(db_)));
    }
    
    return Result<void>::ok();
}

Result<StorageStats> SQLiteStorage::get_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) {
        return Result<StorageStats>::error("Database not open");
    }
    
    StorageStats stats;
    
    // Count total chunks
    const char* count_sql = "SELECT COUNT(*) FROM chunks_vec";
    sqlite3_stmt* stmt = prepare_stmt(db_, count_sql);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_chunks = sqlite3_column_int(stmt, 0);
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
    auto file_size = std::filesystem::file_size(db_path_, ec);
    if (!ec) {
        stats.db_size_bytes = file_size;
    }
    
    return Result<StorageStats>::ok_value(stats);
}

// Helper methods

double SQLiteStorage::bm25_rank_to_score(double rank) {
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

void SQLiteStorage::update_fts_for_chunk(const ChunkRecord& chunk) {
    // Insert into FTS table
    const char* sql = R"(
        INSERT INTO chunks_fts(id, path, start_line, end_line, source, snippet)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(rowid) DO UPDATE SET snippet = excluded.snippet
    )";
    
    sqlite3_stmt* stmt = prepare_stmt(db_, sql);
    if (!stmt) return;
    
    sqlite3_bind_text(stmt, 1, chunk.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, chunk.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, chunk.start_line);
    sqlite3_bind_int(stmt, 4, chunk.end_line);
    sqlite3_bind_text(stmt, 5, to_string(chunk.source).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, chunk.snippet.c_str(), -1, SQLITE_TRANSIENT);
    
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SQLiteStorage::update_fts_after_delete(const std::string& path, MemorySource source) {
    const char* sql = "DELETE FROM chunks_fts WHERE path = ? AND source = ?";
    
    sqlite3_stmt* stmt = prepare_stmt(db_, sql);
    if (!stmt) return;
    
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, to_string(source).c_str(), -1, SQLITE_TRANSIENT);
    
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

Result<void> SQLiteStorage::begin_transaction() {
    std::string error;
    if (!exec_sql(db_, "BEGIN TRANSACTION", &error)) {
        return Result<void>::error("Failed to begin transaction: " + error);
    }
    return Result<void>::ok();
}

Result<void> SQLiteStorage::commit_transaction() {
    std::string error;
    if (!exec_sql(db_, "COMMIT", &error)) {
        return Result<void>::error("Failed to commit transaction: " + error);
    }
    return Result<void>::ok();
}

Result<void> SQLiteStorage::rollback_transaction() {
    std::string error;
    if (!exec_sql(db_, "ROLLBACK", &error)) {
        return Result<void>::error("Failed to rollback transaction: " + error);
    }
    return Result<void>::ok();
}

// ============================================================================
// Transaction RAII Helper
// ============================================================================

Transaction::Transaction(SQLiteStorage* storage) 
    : storage_(storage), active_(false) {}

Transaction::~Transaction() {
    if (active_) {
        storage_->rollback_transaction();
    }
}

Result<void> Transaction::begin() {
    auto res = storage_->begin_transaction();
    if (res.is_ok()) {
        active_ = true;
    }
    return res;
}

Result<void> Transaction::commit() {
    if (!active_) {
        return Result<void>::error("No active transaction");
    }
    auto res = storage_->commit_transaction();
    if (res.is_ok()) {
        active_ = false;
    }
    return res;
}

} // namespace memory
} // namespace hunterclaw
