#pragma once

#include "hunterclaw/memory/plugin.h"
#include "hunterclaw/memory/provider.h"
#include "hunterclaw/core/result.h"
#include <memory>

namespace hunterclaw {
namespace memory {

/**
 * 🎯 100% 对齐 OpenClaw: SQLite Memory Plugin
 * 
 * 提供本地 SQLite 数据库作为记忆存储后端。
 * 
 * 对齐 OpenClaw 内置 SQLite Memory Manager:
 * - ✅ FTS5 全文搜索 (BM25 关键词匹配)
 * - ✅ sqlite-vec 向量搜索 (cosine similarity)
 * - ✅ 混合搜索 (Hybrid Search: vector + keyword)
 * - ✅ MMR 重排 (最大边际相关性去重)
 * - ✅ 时间衰减 (Temporal Decay)
 * - ✅ Embedding Cache (避免重复计算)
 * - ✅ 增量索引 (Incremental Sync)
 * 
 * Plugin 清单 (.plugin.json):
 * {
 *   "id": "sqlite-memory",
 *   "name": "SQLite Memory",
 *   "version": "1.0.0",
 *   "type": "memory",
 *   "author": "HunterClaw Team",
 *   "description": "Local SQLite-based memory storage with FTS5 and vector search",
 *   "main": "libsqlite_memory_plugin.so",
 *   "config": {
 *     "db_path": {
 *       "type": "string",
 *       "default": "./memory.db",
 *       "description": "SQLite database file path"
 *     },
 *     "embedding_provider": {
 *       "type": "string",
 *       "default": "openai",
 *       "description": "Embedding provider (openai, ollama, gemini, voyage)"
 *     },
 *     "hybrid_search": {
 *       "type": "boolean",
 *       "default": true,
 *       "description": "Enable hybrid search (vector + keyword)"
 *     },
 *     "vector_weight": {
 *       "type": "number",
 *       "default": 0.7,
 *       "description": "Weight for vector search in hybrid mode"
 *     },
 *     "text_weight": {
 *       "type": "number",
 *       "default": 0.3,
 *       "description": "Weight for keyword search in hybrid mode"
 *     },
 *     "mmr_enabled": {
 *       "type": "boolean",
 *       "default": true,
 *       "description": "Enable MMR reranking"
 *     },
 *     "temporal_decay": {
 *       "type": "boolean",
 *       "default": false,
 *       "description": "Enable temporal decay for old memories"
 *     }
 *   }
 * }
 * 
 * 使用示例:
 * 
 * // 1. 插件会自动加载并注册
 * auto& pm = MemoryPluginManager::instance();
 * pm.discover_and_load_all();
 * 
 * // 2. Agent 自动使用 (config.yaml)
 * // memory:
 * //   provider: sqlite-memory
 * //   config:
 * //     db_path: ./memory.db
 * 
 * // 3. 手动使用
 * auto& registry = MemoryProviderRegistry::instance();
 * auto provider = registry.get_provider("sqlite-memory");
 * 
 * provider->add_memory("用户最喜欢的颜色是蓝色", {{"category", "preference"}});
 * auto results = provider->search_memories("我喜欢什么颜色?");
 */
class SQLiteMemoryPlugin : public MemoryPlugin {
public:
    SQLiteMemoryPlugin() = default;
    ~SQLiteMemoryPlugin() override = default;
    
    // ========================================
    // 🎯 Plugin 生命周期
    // ========================================
    
    /**
     * 初始化插件
     * 
     * 流程:
     * 1. 解析配置 (db_path, embedding_provider, etc.)
     * 2. 创建 SQLiteMemoryProvider
     * 3. 初始化数据库 (schema, FTS5, sqlite-vec)
     * 4. 注册到 Provider Registry
     */
    core::Result<bool> init(const plugins::PluginContext& ctx) override;
    
    /**
     * 关闭插件
     * 
     * 流程:
     * 1. 保存未保存的数据
     * 2. 关闭数据库连接
     * 3. 从 Registry 移除
     */
    core::Result<bool> shutdown() override;
    
    // ========================================
    // 🎯 Plugin 元信息
    // ========================================
    
    std::string id() const override { return "sqlite-memory"; }
    std::string name() const override { return "SQLite Memory"; }
    std::string version() const override { return "1.0.0"; }
    
    // ========================================
    // 🎯 MemoryPlugin 核心 API
    // ========================================
    
    /**
     * 获取 Memory Provider 实例
     */
    std::shared_ptr<MemoryProvider> get_provider() override {
        return provider_;
    }
    
private:
    std::shared_ptr<MemoryProvider> provider_;
};

// ========================================
// 🎯 对齐 OpenClaw: SQLite Memory Provider 实现
// ========================================

/**
 * SQLite Memory Provider
 * 
 * 对齐 OpenClaw src/memory/manager.ts (803 行)
 */
class SQLiteMemoryProvider : public MemoryProvider {
public:
    /**
     * 构造函数
     * 
     * @param db_path SQLite 数据库文件路径
     * @param embedding_provider Embedding 提供者 (openai, ollama, gemini, voyage)
     * @param config 额外配置
     */
    explicit SQLiteMemoryProvider(
        const std::string& db_path,
        const std::string& embedding_provider = "openai",
        const std::map<std::string, std::string>& config = {}
    );
    
    ~SQLiteMemoryProvider() override;
    
    // ========================================
    // 🎯 OpenClaw 核心 API (1:1 对齐)
    // ========================================
    
    core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) override;
    
    core::Result<std::vector<Memory>> search_memories(
        const std::string& query,
        const SearchOptions& options = SearchOptions()
    ) override;
    
    core::Result<bool> remove_memory(const std::string& id) override;
    
    core::Result<bool> update_memory(
        const std::string& id,
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) override;
    
    core::Result<bool> clear_memories() override;
    
    core::Result<MemoryStats> get_stats() override;
    
    // ========================================
    // 🎯 OpenClaw 扩展 API
    // ========================================
    
    core::Result<Memory> get_memory(const std::string& id) override;
    
    // ========================================
    // Provider 元信息
    // ========================================
    
    std::string provider_id() const override { return "sqlite-memory"; }
    std::string provider_name() const override { return "SQLite Memory Provider"; }
    std::string provider_version() const override { return "1.0.0"; }
    
    // ========================================
    // 🎯 OpenClaw 特有功能
    // ========================================
    
    /**
     * 混合搜索 (Hybrid Search)
     * 
     * 对齐 OpenClaw src/memory/hybrid.ts
     * 
     * 同时执行:
     * - Vector Search (通过 embedding + cosine similarity)
     * - Keyword Search (FTS5 BM25)
     * 
     * 然后按权重合并结果。
     */
    core::Result<std::vector<Memory>> hybrid_search(
        const std::string& query,
        const SearchOptions& options
    );
    
    /**
     * MMR 重排 (Maximal Marginal Relevance)
     * 
     * 对齐 OpenClaw src/memory/mmr.ts
     * 
     * 去除相似结果,保持多样性。
     */
    std::vector<Memory> apply_mmr(
        const std::vector<Memory>& memories,
        double lambda = 0.7
    );
    
    /**
     * 时间衰减 (Temporal Decay)
     * 
     * 对齐 OpenClaw src/memory/temporal-decay.ts
     * 
     * 降低旧记忆的权重。
     */
    void apply_temporal_decay(
        std::vector<Memory>& memories,
        int half_life_days = 30
    );
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace memory
} // namespace hunterclaw

// ========================================
// 🎯 Plugin 导出函数 (C ABI)
// ========================================

extern "C" {

/**
 * 创建插件实例
 * 
 * Plugin Manager 会调用此函数创建插件。
 */
hunterclaw::memory::MemoryPlugin* create_plugin();

/**
 * 销毁插件实例 (可选)
 */
void destroy_plugin(hunterclaw::memory::MemoryPlugin* plugin);

} // extern "C"
