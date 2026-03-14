#include "hunterclaw/memory/memory.h"
#include <sqlite3.h>
#include <stdexcept>
#include <sstream>
#include <ctime>

namespace hunterclaw {
namespace memory {

// ========================================
// SQLiteVectorStorage Implementation
// ========================================

struct SQLiteVectorStorage::Impl {
    sqlite3* db = nullptr;
    int dimensions = 1536;
    
    // FTS5 支持
    bool fts_available = false;
    
    ~Impl() {
        if (db) {
            sqlite3_close(db);
        }
    }
};

std::shared_ptr<SQLiteVectorStorage> SQLiteVectorStorage::create(
    const std::string& db_path,
    int dimensions
) {
    auto storage = std::make_shared<SQLiteVectorStorage>();
    storage->impl_ = std::make_unique<Impl>();
    storage->impl_->dimensions = dimensions;
    
    // 打开数据库
    int rc = sqlite3_open(db_path.c_str(), &storage->impl_->db);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to open SQLite database: " + std::string(sqlite3_errmsg(storage->impl_->db)));
    }
    
    // 🎯 对齐 OpenClaw: 创建 Schema
    storage->ensure_schema();
    
    return storage;
}

void SQLiteVectorStorage::ensure_schema() {
    const char* schema_sql = R"(
        -- Meta table
        CREATE TABLE IF NOT EXISTS meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        
        -- Files table (对齐 OpenClaw)
        CREATE TABLE IF NOT EXISTS files (
            path TEXT PRIMARY KEY,
            source TEXT NOT NULL DEFAULT 'memory',
            hash TEXT NOT NULL,
            mtime INTEGER NOT NULL,
            size INTEGER NOT NULL
        );
        
        -- Chunks table (对齐 OpenClaw)
        CREATE TABLE IF NOT EXISTS chunks (
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
        );
        
        CREATE INDEX IF NOT EXISTS idx_chunks_path ON chunks(path);
        CREATE INDEX IF NOT EXISTS idx_chunks_source ON chunks(source);
        
        -- Embedding cache (对齐 OpenClaw)
        CREATE TABLE IF NOT EXISTS embedding_cache (
            provider TEXT NOT NULL,
            model TEXT NOT NULL,
            provider_key TEXT NOT NULL,
            hash TEXT NOT NULL,
            embedding TEXT NOT NULL,
            dims INTEGER,
            updated_at INTEGER NOT NULL,
            PRIMARY KEY (provider, model, provider_key, hash)
        );
        
        CREATE INDEX IF NOT EXISTS idx_embedding_cache_updated_at ON embedding_cache(updated_at);
    )";
    
    char* err_msg = nullptr;
    int rc = sqlite3_exec(impl_->db, schema_sql, nullptr, nullptr, &err_msg);
    
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        sqlite3_free(err_msg);
        throw std::runtime_error("Failed to create schema: " + error);
    }
    
    // 🎯 对齐 OpenClaw: 尝试启用 FTS5
    impl_->fts_available = try_enable_fts5();
}

bool SQLiteVectorStorage::try_enable_fts5() {
    const char* fts_sql = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
            text,
            id UNINDEXED,
            path UNINDEXED,
            source UNINDEXED,
            model UNINDEXED,
            start_line UNINDEXED,
            end_line UNINDEXED
        );
    )";
    
    char* err_msg = nullptr;
    int rc = sqlite3_exec(impl_->db, fts_sql, nullptr, nullptr, &err_msg);
    
    if (rc != SQLITE_OK) {
        // FTS5 不可用 (未编译进 SQLite)
        sqlite3_free(err_msg);
        return false;
    }
    
    return true;
}

std::string SQLiteVectorStorage::add(
    const std::string& content,
    const std::vector<float>& embedding,
    const std::map<std::string, std::string>& metadata
) {
    // 生成 ID
    std::string id = generate_id();
    
    // 序列化 embedding 为 JSON array
    std::string embedding_json = serialize_embedding(embedding);
    
    // 提取 metadata 字段 (对齐 OpenClaw)
    std::string path = metadata.count("path") ? metadata.at("path") : "unknown";
    std::string source = metadata.count("source") ? metadata.at("source") : "memory";
    int start_line = metadata.count("start_line") ? std::stoi(metadata.at("start_line")) : 1;
    int end_line = metadata.count("end_line") ? std::stoi(metadata.at("end_line")) : 1;
    std::string hash = metadata.count("hash") ? metadata.at("hash") : "";
    std::string model = metadata.count("model") ? metadata.at("model") : "unknown";
    int64_t updated_at = std::time(nullptr);
    
    // 插入 chunks 表
    const char* insert_sql = R"(
        INSERT INTO chunks (id, path, source, start_line, end_line, hash, model, text, embedding, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(impl_->db, insert_sql, -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare insert statement");
    }
    
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, start_line);
    sqlite3_bind_int(stmt, 5, end_line);
    sqlite3_bind_text(stmt, 6, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, embedding_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 10, updated_at);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert chunk");
    }
    
    // 🎯 对齐 OpenClaw: 插入 FTS5 (如果可用)
    if (impl_->fts_available) {
        insert_fts5(id, path, source, model, start_line, end_line, content);
    }
    
    return id;
}

void SQLiteVectorStorage::insert_fts5(
    const std::string& id,
    const std::string& path,
    const std::string& source,
    const std::string& model,
    int start_line,
    int end_line,
    const std::string& text
) {
    const char* fts_sql = R"(
        INSERT INTO chunks_fts (text, id, path, source, model, start_line, end_line)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    )";
    
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(impl_->db, fts_sql, -1, &stmt, nullptr);
    
    sqlite3_bind_text(stmt, 1, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, start_line);
    sqlite3_bind_int(stmt, 7, end_line);
    
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<SearchResult> SQLiteVectorStorage::text_search(
    const std::string& query,
    int limit
) {
    std::vector<SearchResult> results;
    
    // 🎯 对齐 OpenClaw: 使用 FTS5 BM25
    if (impl_->fts_available) {
        const char* fts_query = R"(
            SELECT 
                c.id, c.text, c.source, c.updated_at, c.embedding,
                c.path, c.start_line, c.end_line, c.model,
                chunks_fts.rank AS bm25_score
            FROM chunks_fts
            JOIN chunks c ON chunks_fts.id = c.id
            WHERE chunks_fts MATCH ?
            ORDER BY bm25_score DESC
            LIMIT ?
        )";
        
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(impl_->db, fts_query, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SearchResult result;
            result.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            result.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            result.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            result.timestamp = sqlite3_column_int64(stmt, 3);
            
            // Metadata
            result.metadata["path"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            result.metadata["start_line"] = std::to_string(sqlite3_column_int(stmt, 6));
            result.metadata["end_line"] = std::to_string(sqlite3_column_int(stmt, 7));
            result.metadata["model"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            result.metadata["source"] = result.source;
            
            // BM25 score (FTS5 的 rank 是负数,取绝对值)
            result.score = std::abs(sqlite3_column_double(stmt, 9));
            
            // 解析 embedding (用于后续 MMR)
            std::string embedding_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            result.embedding = deserialize_embedding(embedding_json);
            
            results.push_back(result);
        }
        
        sqlite3_finalize(stmt);
    } else {
        // Fallback: 使用 LIKE 查询 (性能较差)
        const char* like_query = R"(
            SELECT id, text, source, updated_at, embedding, path, start_line, end_line, model
            FROM chunks
            WHERE text LIKE ?
            LIMIT ?
        )";
        
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(impl_->db, like_query, -1, &stmt, nullptr);
        
        std::string pattern = "%" + query + "%";
        sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SearchResult result;
            result.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            result.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            result.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            result.timestamp = sqlite3_column_int64(stmt, 3);
            result.score = 1.0;  // LIKE 无法提供 BM25 分数
            
            // Metadata
            result.metadata["path"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            result.metadata["start_line"] = std::to_string(sqlite3_column_int(stmt, 6));
            result.metadata["end_line"] = std::to_string(sqlite3_column_int(stmt, 7));
            result.metadata["model"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            result.metadata["source"] = result.source;
            
            std::string embedding_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            result.embedding = deserialize_embedding(embedding_json);
            
            results.push_back(result);
        }
        
        sqlite3_finalize(stmt);
    }
    
    return results;
}

// 辅助函数
std::string SQLiteVectorStorage::generate_id() {
    // 简化: 使用时间戳 + 随机数
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int random = rand();
    
    std::ostringstream oss;
    oss << "chunk_" << timestamp << "_" << random;
    return oss.str();
}

std::string SQLiteVectorStorage::serialize_embedding(const std::vector<float>& embedding) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < embedding.size(); ++i) {
        oss << embedding[i];
        if (i < embedding.size() - 1) {
            oss << ",";
        }
    }
    oss << "]";
    return oss.str();
}

std::vector<float> SQLiteVectorStorage::deserialize_embedding(const std::string& json) {
    // 简化: 手动解析 JSON array
    std::vector<float> embedding;
    
    std::string trimmed = json;
    if (trimmed.front() == '[') trimmed = trimmed.substr(1);
    if (trimmed.back() == ']') trimmed = trimmed.substr(0, trimmed.size() - 1);
    
    std::istringstream stream(trimmed);
    std::string token;
    
    while (std::getline(stream, token, ',')) {
        embedding.push_back(std::stof(token));
    }
    
    return embedding;
}

// 其他方法 (remove, clear, vector_search) 保持不变...

bool SQLiteVectorStorage::remove(const std::string& id) {
    const char* delete_sql = "DELETE FROM chunks WHERE id = ?";
    
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(impl_->db, delete_sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    // 同时删除 FTS5
    if (impl_->fts_available) {
        const char* fts_delete = "DELETE FROM chunks_fts WHERE id = ?";
        sqlite3_prepare_v2(impl_->db, fts_delete, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    return rc == SQLITE_DONE;
}

void SQLiteVectorStorage::clear() {
    sqlite3_exec(impl_->db, "DELETE FROM chunks", nullptr, nullptr, nullptr);
    
    if (impl_->fts_available) {
        sqlite3_exec(impl_->db, "DELETE FROM chunks_fts", nullptr, nullptr, nullptr);
    }
}

} // namespace memory
} // namespace hunterclaw
