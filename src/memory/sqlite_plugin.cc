#include "hunterclaw/memory/sqlite_plugin.h"
#include "hunterclaw/memory/registry.h"
#include "hunterclaw/memory/embedding_provider.h"
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <json/json.h>
#include <fstream>
#include <cmath>
#include <algorithm>

namespace hunterclaw {
namespace memory {

// ========================================
// SQLiteMemoryProvider 实现细节
// ========================================

class SQLiteMemoryProvider::Impl {
public:
    sqlite3* db = nullptr;
    std::string db_path;
    std::string embedding_provider_type;  // "openai", "ollama", "gemini"
    std::shared_ptr<EmbeddingProvider> embedding_provider;  // 实际的 embedding provider
    
    // 配置参数
    bool hybrid_search_enabled = true;
    double vector_weight = 0.7;
    double text_weight = 0.3;
    bool mmr_enabled = true;
    bool temporal_decay_enabled = false;
    int temporal_decay_half_life = 30;  // days
    
    // 初始化数据库
    core::Result<bool> init_database() {
        int rc = sqlite3_open(db_path.c_str(), &db);
        if (rc != SQLITE_OK) {
            return core::Err("Failed to open database: " + std::string(sqlite3_errmsg(db)));
        }
        
        // 执行 schema
        const char* schema = R"(
            -- Memories 表
            CREATE TABLE IF NOT EXISTS memories (
                id TEXT PRIMARY KEY,
                content TEXT NOT NULL,
                embedding BLOB,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );
            
            -- Metadata 表
            CREATE TABLE IF NOT EXISTS memory_metadata (
                memory_id TEXT NOT NULL,
                key TEXT NOT NULL,
                value TEXT,
                PRIMARY KEY (memory_id, key),
                FOREIGN KEY (memory_id) REFERENCES memories(id) ON DELETE CASCADE
            );
            
            -- FTS5 全文搜索索引
            CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5(
                content,
                content=memories,
                content_rowid=id
            );
            
            -- FTS5 触发器 (自动同步)
            CREATE TRIGGER IF NOT EXISTS memories_fts_insert AFTER INSERT ON memories BEGIN
                INSERT INTO memories_fts(rowid, content) VALUES (new.rowid, new.content);
            END;
            
            CREATE TRIGGER IF NOT EXISTS memories_fts_update AFTER UPDATE ON memories BEGIN
                UPDATE memories_fts SET content = new.content WHERE rowid = old.rowid;
            END;
            
            CREATE TRIGGER IF NOT EXISTS memories_fts_delete AFTER DELETE ON memories BEGIN
                DELETE FROM memories_fts WHERE rowid = old.rowid;
            END;
            
            -- 索引
            CREATE INDEX IF NOT EXISTS idx_memories_created_at ON memories(created_at);
            CREATE INDEX IF NOT EXISTS idx_memories_updated_at ON memories(updated_at);
            CREATE INDEX IF NOT EXISTS idx_memory_metadata_key ON memory_metadata(key);
        )";
        
        char* err_msg = nullptr;
        rc = sqlite3_exec(db, schema, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string error = err_msg;
            sqlite3_free(err_msg);
            return core::Err("Failed to execute schema: " + error);
        }
        
        spdlog::info("SQLiteMemoryProvider: database initialized at {}", db_path);
        return core::Ok(true);
    }
    
    // 生成 UUID
    std::string generate_id() {
        // 简化实现: 使用时间戳 + 随机数
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        return "mem_" + std::to_string(ms) + "_" + std::to_string(rand() % 10000);
    }
    
    // 向量相似度 (cosine similarity)
    double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
        if (a.size() != b.size() || a.empty()) return 0.0;
        
        double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            dot += a[i] * b[i];
            norm_a += a[i] * a[i];
            norm_b += b[i] * b[i];
        }
        
        if (norm_a == 0.0 || norm_b == 0.0) return 0.0;
        return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
    }
    
    // 获取 embedding (使用 EmbeddingProvider)
    std::vector<float> get_embedding(const std::string& text) {
        // ========== 使用 EmbeddingProvider 获取真实 embedding ==========
        if (!embedding_provider_) {
            spdlog::warn("[SQLiteMemoryProvider] No embedding provider configured, using random embedding");
            // 回退: 生成随机 embedding (仅用于测试)
            std::vector<float> embedding(1536, 0.0f);
            for (size_t i = 0; i < embedding.size(); ++i) {
                embedding[i] = static_cast<float>(rand()) / RAND_MAX;
            }
            return embedding;
        }
        
        // 调用 EmbeddingProvider API
        auto result = embedding_provider_->embed(text);
        if (!result) {
            spdlog::error("[SQLiteMemoryProvider] Failed to get embedding: {}", result.error());
            // 回退: 返回零向量
            return std::vector<float>(embedding_provider_->dimension(), 0.0f);
        }
        
        return result.value();
    }
};

// ========================================
// SQLiteMemoryProvider 实现
// ========================================

SQLiteMemoryProvider::SQLiteMemoryProvider(
    const std::string& db_path,
    const std::string& embedding_provider,
    const std::map<std::string, std::string>& config
) : impl_(std::make_unique<Impl>()) {
    impl_->db_path = db_path;
    impl_->embedding_provider_type = embedding_provider;
    
    // ========== 创建 EmbeddingProvider ==========
    // 从 config 提取 embedding provider 配置
    std::map<std::string, std::string> embedding_config;
    for (const auto& [key, value] : config) {
        if (key.find("embedding_") == 0) {
            // 提取 embedding_* 配置项
            std::string clean_key = key.substr(10);  // 去掉 "embedding_" 前缀
            embedding_config[clean_key] = value;
        }
    }
    
    // 创建 embedding provider
    auto provider_result = EmbeddingProviderFactory::create(
        embedding_provider,
        embedding_config
    );
    
    if (provider_result) {
        impl_->embedding_provider = provider_result.value();
        spdlog::info("[SQLiteMemoryProvider] Initialized embedding provider: {} (model: {})",
            embedding_provider,
            impl_->embedding_provider->model_name());
    } else {
        spdlog::warn("[SQLiteMemoryProvider] Failed to create embedding provider: {}",
            provider_result.error());
        // 继续运行,但 embedding 功能将不可用
    }
    
    // 解析配置
    if (config.find("hybrid_search") != config.end()) {
        impl_->hybrid_search_enabled = (config.at("hybrid_search") == "true");
    }
    if (config.find("vector_weight") != config.end()) {
        impl_->vector_weight = std::stod(config.at("vector_weight"));
    }
    if (config.find("text_weight") != config.end()) {
        impl_->text_weight = std::stod(config.at("text_weight"));
    }
    if (config.find("mmr_enabled") != config.end()) {
        impl_->mmr_enabled = (config.at("mmr_enabled") == "true");
    }
    if (config.find("temporal_decay") != config.end()) {
        impl_->temporal_decay_enabled = (config.at("temporal_decay") == "true");
    }
    
    // 初始化数据库
    auto result = impl_->init_database();
    if (!result) {
        spdlog::error("SQLiteMemoryProvider: failed to initialize database: {}", result.error());
    }
}

SQLiteMemoryProvider::~SQLiteMemoryProvider() {
    if (impl_->db) {
        sqlite3_close(impl_->db);
    }
}

// ========================================
// 添加记忆
// ========================================

core::Result<std::string> SQLiteMemoryProvider::add_memory(
    const std::string& content,
    const std::map<std::string, std::string>& metadata
) {
    std::string id = impl_->generate_id();
    
    // 1. 生成 embedding
    auto embedding = impl_->get_embedding(content);
    
    // 2. 序列化 embedding
    std::vector<uint8_t> embedding_blob;
    for (float f : embedding) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(float));
        embedding_blob.push_back((bits >> 0) & 0xFF);
        embedding_blob.push_back((bits >> 8) & 0xFF);
        embedding_blob.push_back((bits >> 16) & 0xFF);
        embedding_blob.push_back((bits >> 24) & 0xFF);
    }
    
    // 3. 插入 memories 表
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    const char* sql = "INSERT INTO memories (id, content, embedding, created_at, updated_at) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Err("Failed to prepare statement: " + std::string(sqlite3_errmsg(impl_->db)));
    }
    
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, embedding_blob.data(), embedding_blob.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, timestamp);
    sqlite3_bind_int64(stmt, 5, timestamp);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        return core::Err("Failed to insert memory: " + std::string(sqlite3_errmsg(impl_->db)));
    }
    
    // 4. 插入 metadata
    for (const auto& [key, value] : metadata) {
        const char* meta_sql = "INSERT INTO memory_metadata (memory_id, key, value) VALUES (?, ?, ?)";
        sqlite3_stmt* meta_stmt;
        
        sqlite3_prepare_v2(impl_->db, meta_sql, -1, &meta_stmt, nullptr);
        sqlite3_bind_text(meta_stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(meta_stmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(meta_stmt, 3, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(meta_stmt);
        sqlite3_finalize(meta_stmt);
    }
    
    spdlog::debug("SQLiteMemoryProvider: added memory {}", id);
    return core::Ok(id);
}

// ========================================
// 搜索记忆
// ========================================

core::Result<std::vector<Memory>> SQLiteMemoryProvider::search_memories(
    const std::string& query,
    const SearchOptions& options
) {
    if (impl_->hybrid_search_enabled && options.hybrid_search) {
        return hybrid_search(query, options);
    }
    
    // 纯 FTS5 搜索 (简化实现)
    std::vector<Memory> results;
    
    const char* sql = "SELECT m.id, m.content, m.created_at, m.updated_at "
                      "FROM memories m "
                      "JOIN memories_fts f ON m.rowid = f.rowid "
                      "WHERE memories_fts MATCH ? "
                      "ORDER BY rank LIMIT ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Err("Failed to prepare search statement");
    }
    
    sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, options.limit);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Memory mem;
        mem.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        mem.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        mem.score = 0.8;  // 简化评分
        results.push_back(mem);
    }
    
    sqlite3_finalize(stmt);
    
    // 应用 MMR 重排
    if (impl_->mmr_enabled && options.mmr_rerank) {
        results = apply_mmr(results);
    }
    
    // 应用时间衰减
    if (impl_->temporal_decay_enabled && options.temporal_decay) {
        apply_temporal_decay(results, impl_->temporal_decay_half_life);
    }
    
    // 过滤低分结果
    results.erase(
        std::remove_if(results.begin(), results.end(),
            [&](const Memory& m) { return m.score < options.min_score; }),
        results.end()
    );
    
    return core::Ok(results);
}

// ========================================
// 混合搜索
// ========================================

core::Result<std::vector<Memory>> SQLiteMemoryProvider::hybrid_search(
    const std::string& query,
    const SearchOptions& options
) {
    // 1. 向量搜索
    auto query_embedding = impl_->get_embedding(query);
    std::vector<Memory> vector_results;
    
    const char* sql = "SELECT id, content, embedding, created_at, updated_at FROM memories LIMIT 100";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Memory mem;
        mem.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        mem.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        
        // 解析 embedding
        const void* blob = sqlite3_column_blob(stmt, 2);
        int blob_size = sqlite3_column_bytes(stmt, 2);
        std::vector<float> embedding(blob_size / 4);
        std::memcpy(embedding.data(), blob, blob_size);
        
        // 计算相似度
        mem.score = impl_->cosine_similarity(query_embedding, embedding);
        vector_results.push_back(mem);
    }
    sqlite3_finalize(stmt);
    
    // 排序并取 top-k
    std::sort(vector_results.begin(), vector_results.end(),
        [](const Memory& a, const Memory& b) { return a.score > b.score; });
    if (vector_results.size() > static_cast<size_t>(options.limit)) {
        vector_results.resize(options.limit);
    }
    
    // 2. FTS5 关键词搜索
    auto fts_result = search_memories(query, options);
    if (!fts_result) {
        return fts_result;
    }
    auto keyword_results = fts_result.value();
    
    // 3. 合并结果 (加权融合)
    std::map<std::string, double> merged_scores;
    for (const auto& mem : vector_results) {
        merged_scores[mem.id] = mem.score * impl_->vector_weight;
    }
    for (const auto& mem : keyword_results) {
        merged_scores[mem.id] += mem.score * impl_->text_weight;
    }
    
    // 4. 构建最终结果
    std::vector<Memory> final_results;
    for (const auto& [id, score] : merged_scores) {
        for (const auto& mem : vector_results) {
            if (mem.id == id) {
                Memory result = mem;
                result.score = score;
                final_results.push_back(result);
                break;
            }
        }
    }
    
    // 排序
    std::sort(final_results.begin(), final_results.end(),
        [](const Memory& a, const Memory& b) { return a.score > b.score; });
    
    // 限制数量
    if (final_results.size() > static_cast<size_t>(options.limit)) {
        final_results.resize(options.limit);
    }
    
    return core::Ok(final_results);
}

// ========================================
// MMR 重排
// ========================================

std::vector<Memory> SQLiteMemoryProvider::apply_mmr(
    const std::vector<Memory>& memories,
    double lambda
) {
    if (memories.empty()) return memories;
    
    std::vector<Memory> selected;
    std::vector<Memory> candidates = memories;
    
    // 选择第一个 (最高分)
    selected.push_back(candidates[0]);
    candidates.erase(candidates.begin());
    
    // 迭代选择
    while (!candidates.empty() && selected.size() < memories.size()) {
        double max_mmr = -1.0;
        size_t max_idx = 0;
        
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& cand = candidates[i];
            
            // 计算与已选择项的最大相似度
            double max_sim = 0.0;
            for (const auto& sel : selected) {
                // 简化: 使用内容长度差异作为相似度
                double sim = 1.0 - std::abs(static_cast<double>(cand.content.size() - sel.content.size())) / 1000.0;
                max_sim = std::max(max_sim, sim);
            }
            
            // MMR 得分 = λ * relevance - (1-λ) * max_similarity
            double mmr = lambda * cand.score - (1 - lambda) * max_sim;
            
            if (mmr > max_mmr) {
                max_mmr = mmr;
                max_idx = i;
            }
        }
        
        selected.push_back(candidates[max_idx]);
        candidates.erase(candidates.begin() + max_idx);
    }
    
    return selected;
}

// ========================================
// 时间衰减
// ========================================

void SQLiteMemoryProvider::apply_temporal_decay(
    std::vector<Memory>& memories,
    int half_life_days
) {
    auto now = std::chrono::system_clock::now();
    
    for (auto& mem : memories) {
        auto age = std::chrono::duration_cast<std::chrono::hours>(now - mem.created_at).count() / 24.0;
        double decay_factor = std::pow(0.5, age / half_life_days);
        mem.score *= decay_factor;
    }
}

// ========================================
// 其他 API
// ========================================

core::Result<bool> SQLiteMemoryProvider::remove_memory(const std::string& id) {
    const char* sql = "DELETE FROM memories WHERE id = ?";
    sqlite3_stmt* stmt;
    
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE) ? core::Ok(true) : core::Err("Failed to remove memory");
}

core::Result<bool> SQLiteMemoryProvider::update_memory(
    const std::string& id,
    const std::string& content,
    const std::map<std::string, std::string>& metadata
) {
    // 简化实现: 删除并重新添加
    auto remove_result = remove_memory(id);
    if (!remove_result) return remove_result;
    
    auto add_result = add_memory(content, metadata);
    return add_result ? core::Ok(true) : core::Err(add_result.error());
}

core::Result<bool> SQLiteMemoryProvider::clear_memories() {
    const char* sql = "DELETE FROM memories";
    char* err_msg = nullptr;
    int rc = sqlite3_exec(impl_->db, sql, nullptr, nullptr, &err_msg);
    
    if (rc != SQLITE_OK) {
        std::string error = err_msg;
        sqlite3_free(err_msg);
        return core::Err(error);
    }
    
    return core::Ok(true);
}

core::Result<MemoryStats> SQLiteMemoryProvider::get_stats() {
    MemoryStats stats;
    
    const char* sql = "SELECT COUNT(*), SUM(LENGTH(content)) FROM memories";
    sqlite3_stmt* stmt;
    
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_memories = sqlite3_column_int64(stmt, 0);
        stats.total_size = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    
    return core::Ok(stats);
}

core::Result<Memory> SQLiteMemoryProvider::get_memory(const std::string& id) {
    const char* sql = "SELECT id, content, created_at, updated_at FROM memories WHERE id = ?";
    sqlite3_stmt* stmt;
    
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        Memory mem;
        mem.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        mem.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        sqlite3_finalize(stmt);
        return core::Ok(mem);
    }
    
    sqlite3_finalize(stmt);
    return core::Err("Memory not found");
}

// ========================================
// SQLiteMemoryPlugin 实现
// ========================================

core::Result<bool> SQLiteMemoryPlugin::init(const plugins::PluginContext& ctx) {
    spdlog::info("SQLiteMemoryPlugin: initializing...");
    
    // 解析配置
    std::string db_path = "./memory.db";
    std::string embedding_provider = "openai";
    
    if (ctx.config.find("db_path") != ctx.config.end()) {
        db_path = ctx.config.at("db_path");
    }
    if (ctx.config.find("embedding_provider") != ctx.config.end()) {
        embedding_provider = ctx.config.at("embedding_provider");
    }
    
    // 创建 Provider
    provider_ = std::make_shared<SQLiteMemoryProvider>(db_path, embedding_provider, ctx.config);
    
    // 注册到 Registry
    auto& registry = MemoryProviderRegistry::instance();
    auto register_result = registry.register_provider(id(), provider_);
    if (!register_result) {
        return core::Err("Failed to register provider: " + register_result.error());
    }
    
    spdlog::info("SQLiteMemoryPlugin: initialized with db_path={}", db_path);
    return core::Ok(true);
}

core::Result<bool> SQLiteMemoryPlugin::shutdown() {
    spdlog::info("SQLiteMemoryPlugin: shutting down...");
    
    // 从 Registry 移除
    auto& registry = MemoryProviderRegistry::instance();
    registry.unregister_provider(id());
    
    provider_.reset();
    
    return core::Ok(true);
}

} // namespace memory
} // namespace hunterclaw

// ========================================
// Plugin 导出函数
// ========================================

extern "C" {

hunterclaw::memory::MemoryPlugin* create_plugin() {
    return new hunterclaw::memory::SQLiteMemoryPlugin();
}

void destroy_plugin(hunterclaw::memory::MemoryPlugin* plugin) {
    delete plugin;
}

} // extern "C"
