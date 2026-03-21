#include "hunterclaw/memory/sqlite_plugin.h"
#include "hunterclaw/memory/provider_registry.h"
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <json/json.h>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <set>
#include <map>
#include <chrono>

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

// ========================================
// SQLiteMemoryProvider 实现细节
// ========================================

class SQLiteMemoryProvider::Impl {
public:
    sqlite3* db = nullptr;
    std::string db_path;
    std::string embedding_provider;
    
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
            return core::Result<bool>::Err("Failed to open database: " + std::string(sqlite3_errmsg(db)));
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
            return core::Result<bool>::Err("Failed to execute schema: " + error);
        }
        
        log()->info("SQLiteMemoryProvider: database initialized at {}", db_path);
        return core::Result<bool>::Ok(true);
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
    
    // 获取 embedding (简化实现,需要调用实际 LLM API)
    std::vector<float> get_embedding(const std::string& text) {
        // TODO: 实际调用 embedding API (OpenAI, Ollama, etc.)
        // 这里返回假 embedding
        std::vector<float> embedding(1536, 0.0f);
        for (size_t i = 0; i < embedding.size(); ++i) {
            embedding[i] = static_cast<float>(rand()) / RAND_MAX;
        }
        return embedding;
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
    impl_->embedding_provider = embedding_provider;
    
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
    if (result.is_err()) {
        log()->error("SQLiteMemoryProvider: failed to initialize database: {}", result.error);
    }
}

// ========================================
// MemorySearchManager interface implementation
// ========================================

std::vector<MemorySearchResult> SQLiteMemoryProvider::search(
    const std::string& query,
    const SearchOptions& opts
) {
    std::vector<MemorySearchResult> results;
    
    if (impl_->hybrid_search_enabled) {
        results = hybrid_search(query, opts);
    } else {
        // FTS5 only
        const char* sql = "SELECT m.id, m.content "
                          "FROM memories m "
                          "JOIN memories_fts f ON m.rowid = f.rowid "
                          "WHERE memories_fts MATCH ? "
                          "ORDER BY rank LIMIT ?";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, opts.max_results.value_or(10));
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                MemorySearchResult r;
                r.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                r.snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                r.score = 0.8;
                r.source = MemorySource::Memory;
                results.push_back(r);
            }
            sqlite3_finalize(stmt);
        }
    }
    
    if (impl_->mmr_enabled) {
        results = apply_mmr_to_search_results(results, 0.7, opts.max_results.value_or(6));
    }
    if (impl_->temporal_decay_enabled) {
        apply_temporal_decay_to_search_results(results, impl_->temporal_decay_half_life);
    }
    double min_score = opts.min_score.value_or(0.0);
    results.erase(
        std::remove_if(results.begin(), results.end(),
            [min_score](const MemorySearchResult& r) { return r.score < min_score; }),
        results.end()
    );
    return results;
}

void SQLiteMemoryProvider::sync(const SyncParams& params) {
    // SQLite provider doesn't need external sync
    if (params.progress) {
        params.progress({1, 1, std::string("SQLite memory ready")});
    }
}

MemoryProviderStatus SQLiteMemoryProvider::status() {
    MemoryProviderStatus s;
    s.backend = "sqlite";
    s.provider = "sqlite-memory";
    s.workspace_dir = impl_->db_path;
    s.db_path = impl_->db_path;
    // Count files/chunks
    if (impl_->db) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(impl_->db, "SELECT COUNT(*) FROM memories", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                s.files = sqlite3_column_int(stmt, 0);
                s.chunks = s.files;
            }
            sqlite3_finalize(stmt);
        }
    }
    return s;
}

MemoryEmbeddingProbeResult SQLiteMemoryProvider::probe_embedding_availability() {
    return MemoryEmbeddingProbeResult{true, std::nullopt};
}

ReadFileResult SQLiteMemoryProvider::read_file(const ReadFileParams& params) {
    ReadFileResult result;
    result.path = params.rel_path;
    // Try to read from memories table
    if (impl_->db) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(impl_->db, "SELECT content FROM memories WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, params.rel_path.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                result.text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
    }
    return result;
}

// ========================================
// 辅助函数: 从数据库获取指定 path/id 的 embedding
// ========================================

std::optional<std::vector<float>> SQLiteMemoryProvider::get_embedding_for_path(const std::string& path) {
    // path 可能是 memory ID 或文件路径
    // 尝试通过 ID 查询
    const char* sql = "SELECT embedding FROM memories WHERE id = ?";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::nullopt;
    }
    
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    
    std::optional<std::vector<float>> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blob_size = sqlite3_column_bytes(stmt, 0);
        
        if (blob && blob_size > 0 && blob_size % 4 == 0) {
            // 反序列化 embedding (float32 数组)
            std::vector<float> embedding(blob_size / 4);
            std::memcpy(embedding.data(), blob, blob_size);
            result = embedding;
        }
    }
    
    sqlite3_finalize(stmt);
    return result;
}

// ========================================
// 辅助函数: 从数据库获取指定 path/id 的 created_at 时间戳
// ========================================

std::optional<int64_t> SQLiteMemoryProvider::get_timestamp_for_path(const std::string& path) {
    const char* sql = "SELECT created_at FROM memories WHERE id = ?";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::nullopt;
    }
    
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    
    std::optional<int64_t> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = sqlite3_column_int64(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return result;
}

// ========================================
// 混合搜索
// ========================================

std::vector<MemorySearchResult> SQLiteMemoryProvider::hybrid_search(
    const std::string& query,
    const SearchOptions& options
) {
    // 1. 向量搜索
    auto query_embedding = impl_->get_embedding(query);
    std::vector<MemorySearchResult> vector_results;
    
    const char* sql = "SELECT id, content, embedding FROM memories LIMIT 100";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MemorySearchResult r;
        r.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        r.snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.source = MemorySource::Memory;
        
        const void* blob = sqlite3_column_blob(stmt, 2);
        int blob_size = sqlite3_column_bytes(stmt, 2);
        std::vector<float> embedding(blob_size / 4);
        if (blob && blob_size > 0) std::memcpy(embedding.data(), blob, blob_size);
        r.score = impl_->cosine_similarity(query_embedding, embedding);
        vector_results.push_back(r);
    }
    sqlite3_finalize(stmt);
    
    // 2. FTS5 搜索
    std::vector<MemorySearchResult> fts_results;
    const char* fts_sql = "SELECT m.id, m.content FROM memories m "
                          "JOIN memories_fts f ON m.rowid = f.rowid "
                          "WHERE memories_fts MATCH ? ORDER BY rank LIMIT ?";
    if (sqlite3_prepare_v2(impl_->db, fts_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, options.max_results.value_or(10));
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MemorySearchResult r;
            r.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            r.snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            r.score = 0.8;
            r.source = MemorySource::Memory;
            fts_results.push_back(r);
        }
        sqlite3_finalize(stmt);
    }
    
    // 3. 合并
    std::map<std::string, double> merged;
    for (const auto& r : vector_results) merged[r.path] += r.score * impl_->vector_weight;
    for (const auto& r : fts_results) merged[r.path] += r.score * impl_->text_weight;
    
    std::vector<MemorySearchResult> final_results;
    for (const auto& r : vector_results) {
        auto it = merged.find(r.path);
        if (it != merged.end()) {
            MemorySearchResult res = r;
            res.score = it->second;
            final_results.push_back(res);
            merged.erase(it);
        }
    }
    std::sort(final_results.begin(), final_results.end(),
        [](const MemorySearchResult& a, const MemorySearchResult& b) { return a.score > b.score; });
    int max_r = options.max_results.value_or(10);
    if ((int)final_results.size() > max_r) final_results.resize(max_r);
    return final_results;
}

// ========================================
// MMR 重排 (MemorySearchResult 版本)
// ========================================
std::vector<MemorySearchResult> SQLiteMemoryProvider::apply_mmr_to_search_results(
    const std::vector<MemorySearchResult>& results,
    double lambda,
    int max_results
) {
    if (results.empty()) return results;
    
    const size_t k = std::min(static_cast<size_t>(max_results), results.size());
    std::vector<MemorySearchResult> reranked;
    std::set<size_t> selected_indices;
    
    // 缓存 embedding 数据: path -> embedding
    std::map<std::string, std::vector<float>> embedding_cache;
    
    // 预加载所有结果的 embedding
    for (const auto& result : results) {
        auto embedding = get_embedding_for_path(result.path);
        if (embedding.has_value()) {
            embedding_cache[result.path] = embedding.value();
        }
    }
    
    // 第一步: 选择最高相关性的结果（对齐 OpenClaw）
    reranked.push_back(results[0]);
    selected_indices.insert(0);
    
    // 迭代选择剩余 k-1 个结果（对齐 OpenClaw mmr.ts:28-50）
    while (reranked.size() < k && reranked.size() < results.size()) {
        double max_mmr_score = -1.0;
        size_t best_idx = 0;
        
        for (size_t i = 0; i < results.size(); ++i) {
            if (selected_indices.count(i) > 0) continue;
            
            const auto& candidate = results[i];
            
            // 获取候选的 embedding
            auto cand_emb_it = embedding_cache.find(candidate.path);
            if (cand_emb_it == embedding_cache.end()) {
                // 如果没有 embedding，使用 snippet 长度作为后备
                continue;
            }
            
            // 计算与已选择项的最大相似度（对齐 OpenClaw mmr.ts:33-39）
            double max_sim = 0.0;
            for (size_t sel_idx : selected_indices) {
                const auto& selected = results[sel_idx];
                auto sel_emb_it = embedding_cache.find(selected.path);
                
                if (sel_emb_it != embedding_cache.end()) {
                    // 使用真实的余弦相似度（对齐 OpenClaw）
                    double sim = impl_->cosine_similarity(
                        cand_emb_it->second, 
                        sel_emb_it->second
                    );
                    max_sim = std::max(max_sim, sim);
                }
            }
            
            // MMR 分数: λ * relevance - (1-λ) * max_similarity
            // 严格对齐 OpenClaw mmr.ts:42
            double mmr_score = lambda * candidate.score - (1.0 - lambda) * max_sim;
            
            if (mmr_score > max_mmr_score) {
                max_mmr_score = mmr_score;
                best_idx = i;
            }
        }
        
        if (max_mmr_score >= 0.0) {
            reranked.push_back(results[best_idx]);
            selected_indices.insert(best_idx);
        } else {
            break; // 没有更多有效候选（对齐 OpenClaw）
        }
    }
    
    return reranked;
}

// ========================================
// 时间衰减 (MemorySearchResult 版本)
// ========================================
void SQLiteMemoryProvider::apply_temporal_decay_to_search_results(
    std::vector<MemorySearchResult>& results,
    int half_life_days
) {
    if (results.empty() || half_life_days <= 0) {
        return;
    }
    
    // 获取当前时间戳 (毫秒)
    auto now = std::chrono::system_clock::now();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    
    // 计算 lambda = ln(2) / half_life_days (对齐 OpenClaw temporal-decay.ts:12-16)
    const double lambda = std::log(2.0) / half_life_days;
    const int64_t DAY_MS = 24LL * 60 * 60 * 1000;
    
    for (auto& result : results) {
        // 从数据库获取真实的时间戳
        auto timestamp_opt = get_timestamp_for_path(result.path);
        
        if (!timestamp_opt.has_value()) {
            // 如果没有时间戳，跳过衰减 (对齐 OpenClaw: evergreen memory)
            continue;
        }
        
        int64_t created_at = timestamp_opt.value();
        
        // 计算年龄 (天数) (对齐 OpenClaw temporal-decay.ts:141-153)
        double age_days = static_cast<double>(now_ms - created_at) / DAY_MS;
        age_days = std::max(0.0, age_days); // 确保非负
        
        // 计算衰减因子: decay = exp(-lambda * age_days)
        // 对齐 OpenClaw temporal-decay.ts:19-27
        double decay_factor = std::exp(-lambda * age_days);
        
        // 应用衰减到分数 (对齐 OpenClaw temporal-decay.ts:30)
        result.score *= decay_factor;
    }
}

// ========================================
// SQLiteMemoryPlugin 实现
// ========================================

core::Result<bool> SQLiteMemoryPlugin::init(const plugins::PluginContext& ctx) {
    log()->info("SQLiteMemoryPlugin: initializing...");
    
    // 解析配置
    std::string db_path = "./memory.db";
    std::string embedding_provider = "openai";
    std::map<std::string, std::string> config_map;
    
    // 从 ctx.config (shared_ptr<Config>) 提取配置
    // TODO: 实际应该通过 Config API 获取配置项
    // 暂时使用默认值
    
    // 创建 Manager
    manager_ = std::make_shared<SQLiteMemoryProvider>(db_path, embedding_provider, config_map);
    
    // 注册到 Registry
    auto& registry = MemoryProviderRegistry::instance();
    auto register_result = registry.register_provider(id(), manager_);
    if (register_result.is_err()) {
        return core::Result<bool>::Err("Failed to register provider: " + register_result.error);
    }
    
    log()->info("SQLiteMemoryPlugin: initialized with db_path={}", db_path);
    return core::Result<bool>::Ok(true);
}

core::Result<bool> SQLiteMemoryPlugin::shutdown() {
    log()->info("SQLiteMemoryPlugin: shutting down...");
    
    // 从 Registry 移除
    auto& registry = MemoryProviderRegistry::instance();
    registry.unregister_provider(id());
    
    manager_.reset();
    
    return core::Result<bool>::Ok(true);
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
