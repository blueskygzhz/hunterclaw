#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <chrono>

namespace hunterclaw {
namespace memory {

// 🎯 对齐 OpenClaw: Memory 数据结构
struct Memory {
    std::string id;
    std::string content;
    std::map<std::string, std::string> metadata;
    std::vector<float> embedding;  // 可选
    double score = 0.0;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    
    // 便利构造函数
    Memory() : score(0.0) {
        created_at = std::chrono::system_clock::now();
        updated_at = created_at;
    }
};

// 🎯 对齐 OpenClaw: SearchOptions
struct SearchOptions {
    int limit = 10;
    double min_score = 0.7;
    std::map<std::string, std::string> filters;
    bool include_embedding = false;
    
    // 扩展选项 (HunterClaw 特有)
    bool hybrid_search = true;
    bool mmr_rerank = true;
    bool temporal_decay = false;
};

// 🎯 对齐 OpenClaw: MemoryStats
struct MemoryStats {
    int64_t total_memories = 0;
    int64_t total_size = 0;
    std::chrono::system_clock::time_point last_updated;
    
    // 扩展统计 (HunterClaw 特有)
    std::map<std::string, int64_t> source_counts;  // 按 source 统计
    double avg_embedding_time_ms = 0.0;
    double avg_search_time_ms = 0.0;
};

// 🎯 100% 对齐 OpenClaw: MemoryProvider 接口
// 这是所有 Memory Provider 必须实现的标准接口
class MemoryProvider {
public:
    virtual ~MemoryProvider() = default;
    
    // ========================================
    // 🎯 OpenClaw 核心 API (1:1 对齐)
    // ========================================
    
    // 添加记忆
    // OpenClaw: async addMemory(content: string, metadata?: Record<string, any>): Promise<string>
    virtual core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) = 0;
    
    // 搜索记忆
    // OpenClaw: async searchMemories(query: string, options?: SearchOptions): Promise<Memory[]>
    virtual core::Result<std::vector<Memory>> search_memories(
        const std::string& query,
        const SearchOptions& options = SearchOptions()
    ) = 0;
    
    // 删除记忆
    // OpenClaw: async removeMemory(id: string): Promise<boolean>
    virtual core::Result<bool> remove_memory(const std::string& id) = 0;
    
    // 更新记忆
    // OpenClaw: async updateMemory(id: string, content: string, metadata?: Record<string, any>): Promise<boolean>
    virtual core::Result<bool> update_memory(
        const std::string& id,
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) = 0;
    
    // 清空记忆
    // OpenClaw: async clearMemories(): Promise<void>
    virtual core::Result<bool> clear_memories() = 0;
    
    // 获取统计
    // OpenClaw: async getStats(): Promise<MemoryStats>
    virtual core::Result<MemoryStats> get_stats() = 0;
    
    // ========================================
    // 🎯 OpenClaw 扩展 API (对齐新功能)
    // ========================================
    
    // 批量添加记忆 (OpenClaw v2.0+)
    virtual core::Result<std::vector<std::string>> add_memories_batch(
        const std::vector<std::string>& contents,
        const std::vector<std::map<std::string, std::string>>& metadata_list = {}
    ) {
        // 默认实现: 逐个添加
        std::vector<std::string> ids;
        for (size_t i = 0; i < contents.size(); ++i) {
            auto metadata = (i < metadata_list.size()) ? metadata_list[i] : std::map<std::string, std::string>{};
            auto result = add_memory(contents[i], metadata);
            if (!result) {
                return core::Err(result.error());
            }
            ids.push_back(result.value());
        }
        return core::Ok(ids);
    }
    
    // 获取单个记忆 (by ID)
    virtual core::Result<Memory> get_memory(const std::string& id) {
        // 默认实现: 通过搜索查找
        // 子类应该实现更高效的版本
        return core::Err("Not implemented");
    }
    
    // ========================================
    // Provider 元信息
    // ========================================
    
    virtual std::string provider_id() const = 0;
    virtual std::string provider_name() const = 0;
    virtual std::string provider_version() const = 0;
};

// ========================================
// 🎯 对齐 OpenClaw: MemoryProviderFactory
// ========================================

class MemoryProviderFactory {
public:
    // 创建本地 SQLite Provider
    static core::Result<std::shared_ptr<MemoryProvider>> create_sqlite(
        const std::string& db_path,
        const std::string& embedding_provider = "openai",
        const std::map<std::string, std::string>& config = {}
    );
    
    // 创建 MemOS Cloud Provider (对齐 OpenClaw 官方插件)
    static core::Result<std::shared_ptr<MemoryProvider>> create_memos_cloud(
        const std::string& api_key,
        const std::string& endpoint = "https://api.memos.cloud"
    );
    
    // 创建 Supabase Provider
    static core::Result<std::shared_ptr<MemoryProvider>> create_supabase(
        const std::string& url,
        const std::string& api_key
    );
    
    // 从配置创建 Provider
    static core::Result<std::shared_ptr<MemoryProvider>> create_from_config(
        const std::string& provider_type,
        const std::map<std::string, std::string>& config
    );
};

// ========================================
// 🎯 对齐 OpenClaw: Session-Memory 集成
// ========================================

// Session Memory Context (会话相关的记忆)
struct SessionMemoryContext {
    std::string session_id;
    std::string agent_id;
    std::map<std::string, std::string> filters;
    
    // 转换为 metadata
    std::map<std::string, std::string> to_metadata() const {
        std::map<std::string, std::string> m;
        m["session_id"] = session_id;
        m["agent_id"] = agent_id;
        for (const auto& [k, v] : filters) {
            m[k] = v;
        }
        return m;
    }
    
    // 转换为搜索过滤器
    std::map<std::string, std::string> to_filters() const {
        return to_metadata();
    }
};

// Session-scoped Memory Provider (会话隔离的记忆)
// 对齐 OpenClaw 的 session-memory 机制
class SessionMemoryProvider : public MemoryProvider {
public:
    explicit SessionMemoryProvider(
        std::shared_ptr<MemoryProvider> underlying,
        const SessionMemoryContext& context
    ) : underlying_(underlying), context_(context) {}
    
    // 自动注入 session metadata
    core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) override {
        auto enriched_metadata = context_.to_metadata();
        enriched_metadata.insert(metadata.begin(), metadata.end());
        return underlying_->add_memory(content, enriched_metadata);
    }
    
    // 自动过滤 session
    core::Result<std::vector<Memory>> search_memories(
        const std::string& query,
        const SearchOptions& options = SearchOptions()
    ) override {
        auto filtered_options = options;
        auto session_filters = context_.to_filters();
        filtered_options.filters.insert(session_filters.begin(), session_filters.end());
        return underlying_->search_memories(query, filtered_options);
    }
    
    core::Result<bool> remove_memory(const std::string& id) override {
        return underlying_->remove_memory(id);
    }
    
    core::Result<bool> update_memory(
        const std::string& id,
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) override {
        auto enriched_metadata = context_.to_metadata();
        enriched_metadata.insert(metadata.begin(), metadata.end());
        return underlying_->update_memory(id, content, enriched_metadata);
    }
    
    core::Result<bool> clear_memories() override {
        // 只清空当前 session 的记忆
        // 实现需要 provider 支持按 filters 删除
        return core::Err("Session-scoped clear not implemented");
    }
    
    core::Result<MemoryStats> get_stats() override {
        return underlying_->get_stats();
    }
    
    std::string provider_id() const override {
        return underlying_->provider_id() + "-session";
    }
    
    std::string provider_name() const override {
        return underlying_->provider_name() + " (Session-scoped)";
    }
    
    std::string provider_version() const override {
        return underlying_->provider_version();
    }

private:
    std::shared_ptr<MemoryProvider> underlying_;
    SessionMemoryContext context_;
};

} // namespace memory
} // namespace hunterclaw
