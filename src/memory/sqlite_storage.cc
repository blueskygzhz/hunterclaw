#include "hunterclaw/memory/memory.h"
#include <sqlite3.h>
#include <uuid/uuid.h>
#include <stdexcept>
#include <sstream>
#include <chrono>
#include <cstring>

namespace hunterclaw {
namespace memory {

// SQLite实现细节
struct SQLiteVectorStorage::Impl {
    sqlite3* db = nullptr;
    int dimensions = 1536;
    
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
    
    // 加载sqlite-vec扩展 (如果可用)
    // 注意: 需要预先编译sqlite-vec扩展
    // sqlite3_enable_load_extension(storage->impl_->db, 1);
    // sqlite3_load_extension(storage->impl_->db, "libsqlite_vec.so", nullptr, nullptr);
    
    // 创建表结构
    const char* create_table_sql = R"(
        CREATE TABLE IF NOT EXISTS memories (
            id TEXT PRIMARY KEY,
            content TEXT NOT NULL,
            embedding BLOB,
            source TEXT,
            timestamp INTEGER,
            metadata TEXT
        );
        
        CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5(
            id UNINDEXED,
            content,
            content=memories,
            content_rowid=rowid
        );
        
        CREATE TRIGGER IF NOT EXISTS memories_fts_insert AFTER INSERT ON memories BEGIN
            INSERT INTO memories_fts(rowid, id, content) VALUES (new.rowid, new.id, new.content);
        END;
        
        CREATE TRIGGER IF NOT EXISTS memories_fts_delete AFTER DELETE ON memories BEGIN
            DELETE FROM memories_fts WHERE rowid = old.rowid;
        END;
    )";
    
    char* err_msg = nullptr;
    rc = sqlite3_exec(storage->impl_->db, create_table_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = std::string("Failed to create tables: ") + err_msg;
        sqlite3_free(err_msg);
        throw std::runtime_error(error);
    }
    
    return storage;
}

std::string SQLiteVectorStorage::add(
    const std::string& content,
    const std::vector<float>& embedding,
    const std::map<std::string, std::string>& metadata
) {
    // 生成UUID
    uuid_t uuid;
    uuid_generate(uuid);
    char uuid_str[37];
    uuid_unparse(uuid, uuid_str);
    std::string id = uuid_str;
    
    // 序列化embedding为BLOB
    std::vector<unsigned char> blob(embedding.size() * sizeof(float));
    std::memcpy(blob.data(), embedding.data(), blob.size());
    
    // 获取当前时间戳
    auto now = std::chrono::system_clock::now();
    int64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    
    // 序列化metadata为JSON字符串 (简化实现)
    std::stringstream metadata_json;
    metadata_json << "{";
    bool first = true;
    for (const auto& [key, value] : metadata) {
        if (!first) metadata_json << ",";
        metadata_json << "\"" << key << "\":\"" << value << "\"";
        first = false;
    }
    metadata_json << "}";
    
    // 插入数据
    const char* insert_sql = "INSERT INTO memories (id, content, embedding, source, timestamp, metadata) VALUES (?, ?, ?, ?, ?, ?)";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(impl_->db, insert_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare statement");
    }
    
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, blob.data(), blob.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, metadata.count("source") ? metadata.at("source").c_str() : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, timestamp);
    sqlite3_bind_text(stmt, 6, metadata_json.str().c_str(), -1, SQLITE_TRANSIENT);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert memory");
    }
    
    return id;
}

std::vector<SearchResult> SQLiteVectorStorage::vector_search(
    const std::vector<float>& query_embedding,
    int limit,
    double min_score
) {
    // 注意: 这是简化实现,使用线性扫描计算余弦相似度
    // 生产环境应使用sqlite-vec扩展或其他向量索引
    
    const char* select_sql = "SELECT id, content, embedding, source, timestamp, metadata FROM memories";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(impl_->db, select_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare statement");
    }
    
    std::vector<SearchResult> results;
    
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        SearchResult result;
        
        result.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        
        // 反序列化embedding
        const void* blob = sqlite3_column_blob(stmt, 2);
        int blob_size = sqlite3_column_bytes(stmt, 2);
        
        if (blob && blob_size > 0) {
            int float_count = blob_size / sizeof(float);
            result.embedding.resize(float_count);
            std::memcpy(result.embedding.data(), blob, blob_size);
            
            // 计算余弦相似度
            result.score = similarity::cosine(query_embedding, result.embedding);
        } else {
            result.score = 0.0;
        }
        
        result.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result.timestamp = sqlite3_column_int64(stmt, 4);
        
        // 过滤低分结果
        if (result.score >= min_score) {
            results.push_back(result);
        }
    }
    
    sqlite3_finalize(stmt);
    
    // 排序
    std::sort(results.begin(), results.end(),
             [](const SearchResult& a, const SearchResult& b) {
                 return a.score > b.score;
             });
    
    // 截断
    if (results.size() > static_cast<size_t>(limit)) {
        results.resize(limit);
    }
    
    return results;
}

std::vector<SearchResult> SQLiteVectorStorage::text_search(
    const std::string& query,
    int limit
) {
    // 使用FTS5全文检索
    const char* fts_sql = R"(
        SELECT m.id, m.content, m.embedding, m.source, m.timestamp, m.metadata, 
               bm25(memories_fts) as score
        FROM memories_fts
        JOIN memories m ON memories_fts.rowid = m.rowid
        WHERE memories_fts MATCH ?
        ORDER BY score DESC
        LIMIT ?
    )";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(impl_->db, fts_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare FTS statement");
    }
    
    sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    
    std::vector<SearchResult> results;
    
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        SearchResult result;
        
        result.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        
        // 反序列化embedding (用于后续MMR)
        const void* blob = sqlite3_column_blob(stmt, 2);
        int blob_size = sqlite3_column_bytes(stmt, 2);
        
        if (blob && blob_size > 0) {
            int float_count = blob_size / sizeof(float);
            result.embedding.resize(float_count);
            std::memcpy(result.embedding.data(), blob, blob_size);
        }
        
        result.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result.timestamp = sqlite3_column_int64(stmt, 4);
        result.score = sqlite3_column_double(stmt, 6);  // BM25 score
        
        results.push_back(result);
    }
    
    sqlite3_finalize(stmt);
    
    return results;
}

bool SQLiteVectorStorage::remove(const std::string& id) {
    const char* delete_sql = "DELETE FROM memories WHERE id = ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(impl_->db, delete_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE);
}

void SQLiteVectorStorage::clear() {
    const char* clear_sql = "DELETE FROM memories";
    
    char* err_msg = nullptr;
    int rc = sqlite3_exec(impl_->db, clear_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = std::string("Failed to clear memories: ") + err_msg;
        sqlite3_free(err_msg);
        throw std::runtime_error(error);
    }
}

} // namespace memory
} // namespace hunterclaw
