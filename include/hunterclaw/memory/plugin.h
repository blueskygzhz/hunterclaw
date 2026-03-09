#pragma once

#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>

namespace hunterclaw {
namespace memory {

// 前向声明
struct MemoryRecord;
struct SearchOptions;
struct MemoryStats;

/**
 * Memory Provider 接口 (100% 对齐 OpenClaw)
 * 
 * 提供统一的记忆存储和检索接口，支持多种后端实现：
 * - 本地 SQLite
 * - 云端服务 (MemOS Cloud, Supabase, etc.)
 * - 自定义实现
 */
class MemoryProvider {
public:
    virtual ~MemoryProvider() = default;
    
    /**
     * 添加记忆
     * @param content 记忆内容
     * @param metadata 元数据 (可选)
     * @return 记忆 ID
     */
    virtual core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) = 0;
    
    /**
     * 搜索相关记忆
     * @param query 查询文本
     * @param options 搜索选项
     * @return 记忆记录列表 (按相关性降序)
     */
    virtual core::Result<std::vector<MemoryRecord>> search_memories(
        const std::string& query,
        const SearchOptions& options = {}
    ) = 0;
    
    /**
     * 删除记忆
     * @param id 记忆 ID
     * @return 是否成功
     */
    virtual core::Result<bool> remove_memory(const std::string& id) = 0;
    
    /**
     * 更新记忆
     * @param id 记忆 ID
     * @param content 新内容
     * @param metadata 新元数据 (可选)
     * @return 是否成功
     */
    virtual core::Result<bool> update_memory(
        const std::string& id,
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) = 0;
    
    /**
     * 清空所有记忆
     */
    virtual core::Result<void> clear_memories() = 0;
    
    /**
     * 获取记忆统计
     */
    virtual core::Result<MemoryStats> get_stats() = 0;
    
    /**
     * Provider ID (唯一标识)
     */
    virtual std::string id() const = 0;
    
    /**
     * Provider 名称 (显示名称)
     */
    virtual std::string name() const = 0;
};

/**
 * 记忆记录 (对齐 OpenClaw Memory Interface)
 */
struct MemoryRecord {
    std::string id;                     // 记忆 ID
    std::string content;                // 记忆内容
    std::map<std::string, std::string> metadata;  // 元数据
    std::vector<float> embedding;       // 向量 (可选)
    double score = 0.0;                 // 相关性得分 (0-1)
    std::chrono::system_clock::time_point created_at;   // 创建时间
    std::chrono::system_clock::time_point updated_at;   // 更新时间
};

/**
 * 搜索选项 (对齐 OpenClaw SearchOptions)
 */
struct SearchOptions {
    int limit = 10;                     // 最大结果数
    double min_score = 0.7;             // 最小相关性得分
    std::map<std::string, std::string> filters;  // 元数据过滤器
    bool include_embedding = false;     // 是否包含向量
};

/**
 * 记忆统计 (对齐 OpenClaw MemoryStats)
 */
struct MemoryStats {
    int total_memories = 0;             // 总记忆数
    size_t total_size = 0;              // 总大小 (字节)
    std::chrono::system_clock::time_point last_updated;  // 最后更新时间
};

/**
 * Memory Plugin 接口 (100% 对齐 OpenClaw)
 * 
 * Memory Plugin 是提供 MemoryProvider 实现的插件。
 * 
 * 示例:
 * - SQLiteMemoryPlugin (本地存储)
 * - MemOSCloudPlugin (云端存储)
 * - SupabaseMemoryPlugin (Supabase PGVector)
 */
class MemoryPlugin : public plugins::Plugin {
public:
    virtual ~MemoryPlugin() = default;
    
    /**
     * 获取 Memory Provider 实例
     */
    virtual std::shared_ptr<MemoryProvider> get_provider() = 0;
    
    /**
     * Plugin 类型 (固定为 "memory")
     */
    std::string type() const override { return "memory"; }
};

} // namespace memory
} // namespace hunterclaw
