#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/types.h"
#include <sqlite3.h>
#include <memory>
#include <string>
#include <vector>

namespace hunterclaw {
namespace memory {

// 严格对齐 OpenClaw src/memory/sqlite.ts

// ============================================================================
// SQLite Storage (sqlite.ts)
// ============================================================================
class SQLiteVectorStorage {
public:
    struct Config {
        std::string db_path;
        int dimensions = 1536;
        bool enable_fts = true;
        bool enable_vector = true;
        std::optional<std::string> vec_extension_path;
    };
    
    static core::Result<std::shared_ptr<SQLiteVectorStorage>> create(const Config& config);
    
    ~SQLiteVectorStorage();
    
    // ========================================================================
    // Chunk操作 (对齐 sqlite.ts 中的操作)
    // ========================================================================
    
    // 添加chunk到向量表和FTS表
    core::Result<std::string> add_chunk(
        const std::string& path,
        const std::string& content,
        const std::vector<float>& embedding,
        const ChunkMetadata& metadata
    );
    
    // 批量添加chunks
    core::Result<std::vector<std::string>> add_chunks_batch(
        const std::vector<std::tuple<std::string, std::string, std::vector<float>, ChunkMetadata>>& chunks
    );
    
    // 删除指定路径的所有chunks
    core::Result<int> delete_chunks_by_path(const std::string& path);
    
    // 删除指定source的所有chunks
    core::Result<int> delete_chunks_by_source(MemorySource source);
    
    // 清空所有chunks
    core::Result<bool> clear_all();
    
    // ========================================================================
    // 向量搜索 (使用sqlite-vec)
    // ========================================================================
    
    // 向量相似度搜索 (对齐 searchVector in manager-search.ts)
    core::Result<std::vector<MemorySearchResult>> vector_search(
        const std::vector<float>& query_embedding,
        int limit,
        double min_score
    );
    
    // 带过滤的向量搜索
    core::Result<std::vector<MemorySearchResult>> vector_search_filtered(
        const std::vector<float>& query_embedding,
        int limit,
        double min_score,
        const std::string& source_filter  // "memory" | "sessions"
    );
    
    // ========================================================================
    // 全文搜索 (使用FTS5)
    // ========================================================================
    
    // BM25全文搜索 (对齐 searchKeyword in manager-search.ts)
    core::Result<std::vector<MemorySearchResult>> fts_search(
        const std::string& query,
        int limit
    );
    
    // 带过滤的全文搜索
    core::Result<std::vector<MemorySearchResult>> fts_search_filtered(
        const std::string& query,
        int limit,
        const std::string& source_filter
    );
    
    // ========================================================================
    // Embedding缓存 (embedding_cache表)
    // ========================================================================
    
    // 查询缓存的embedding
    core::Result<std::optional<std::vector<float>>> get_cached_embedding(
        const std::string& content_hash,
        const std::string& model
    );
    
    // 缓存embedding
    core::Result<bool> cache_embedding(
        const std::string& content_hash,
        const std::vector<float>& embedding,
        const std::string& model
    );
    
    // 清理过期缓存
    core::Result<int> cleanup_old_cache(int64_t older_than_timestamp);
    
    // ========================================================================
    // 统计信息
    // ========================================================================
    
    struct Stats {
        int total_chunks;
        int memory_chunks;
        int sessions_chunks;
        int total_files;
        int cache_entries;
        int64_t db_size_bytes;
    };
    
    core::Result<Stats> get_stats();
    
    // 按source统计
    core::Result<std::vector<MemoryProviderStatus::SourceCount>> get_source_counts();
    
    // ========================================================================
    // 状态查询
    // ========================================================================
    
    // 检查FTS5是否可用
    bool is_fts_available() const { return fts_available_; }
    
    // 检查sqlite-vec是否可用
    bool is_vec_available() const { return vec_available_; }
    
    // 获取加载错误信息
    std::optional<std::string> get_fts_error() const { return fts_error_; }
    std::optional<std::string> get_vec_error() const { return vec_error_; }
    
    // 获取数据库路径
    std::string get_db_path() const { return config_.db_path; }
    
    // 获取维度
    int get_dimensions() const { return config_.dimensions; }
    
    // ========================================================================
    // 事务支持
    // ========================================================================
    
    core::Result<bool> begin_transaction();
    core::Result<bool> commit_transaction();
    core::Result<bool> rollback_transaction();
    
    // RAII事务辅助类
    class Transaction {
    public:
        explicit Transaction(SQLiteVectorStorage& storage);
        ~Transaction();
        core::Result<bool> commit();
        
    private:
        SQLiteVectorStorage& storage_;
        bool committed_ = false;
    };
    
    // ========================================================================
    // 辅助SQL方法 (public for manager_sync_ops)
    // ========================================================================
    
    // 执行SQL语句的辅助方法
    core::Result<bool> exec_sql(const std::string& sql);
    core::Result<sqlite3_stmt*> prepare_statement(const std::string& sql);
    
private:
    explicit SQLiteVectorStorage(const Config& config);
    
    Config config_;
    sqlite3* db_ = nullptr;
    bool fts_available_ = false;
    bool vec_available_ = false;
    std::optional<std::string> fts_error_;
    std::optional<std::string> vec_error_;
    
    // ========================================================================
    // 初始化方法 (对齐 sqlite.ts 的初始化逻辑)
    // ========================================================================
    
    core::Result<bool> open_database();
    core::Result<bool> ensure_schema();
    core::Result<bool> load_sqlite_vec_extension();
    core::Result<bool> try_enable_fts5();
    core::Result<bool> create_tables();
    core::Result<bool> create_indices();
    
    // ========================================================================
    // Schema定义 (对齐 memory-schema.ts)
    // ========================================================================
    
    static constexpr const char* SCHEMA_CHUNKS_VEC = R"(
        CREATE TABLE IF NOT EXISTS chunks_vec (
            id TEXT PRIMARY KEY,
            path TEXT NOT NULL,
            source TEXT NOT NULL,
            model TEXT NOT NULL,
            start_line INTEGER NOT NULL,
            end_line INTEGER NOT NULL,
            content TEXT NOT NULL,
            embedding BLOB,
            created_at INTEGER NOT NULL
        )
    )";
    
    static constexpr const char* SCHEMA_CHUNKS_FTS = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
            id UNINDEXED,
            path,
            source,
            model,
            start_line UNINDEXED,
            end_line UNINDEXED,
            content,
            tokenize='porter'
        )
    )";
    
    static constexpr const char* SCHEMA_EMBEDDING_CACHE = R"(
        CREATE TABLE IF NOT EXISTS embedding_cache (
            content_hash TEXT NOT NULL,
            model TEXT NOT NULL,
            embedding BLOB NOT NULL,
            created_at INTEGER NOT NULL,
            PRIMARY KEY (content_hash, model)
        )
    )";
    
    // ========================================================================
    // 辅助方法
    // ========================================================================
    
    // 生成唯一ID (对齐 generateId)
    std::string generate_id();
    
    // 序列化/反序列化embedding
    std::string serialize_embedding(const std::vector<float>& embedding);
    std::vector<float> deserialize_embedding(const std::string& blob);
    
    // 计算内容hash
    std::string compute_content_hash(const std::string& content);
    
    // 从row构造SearchResult
    MemorySearchResult result_from_row(
        sqlite3_stmt* stmt,
        double score
    );
    
    // BM25 rank转score (对齐 bm25RankToScore in hybrid.ts)
    double bm25_rank_to_score(double rank);
};

} // namespace memory
} // namespace hunterclaw
