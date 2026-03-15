#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <cstdint>

namespace hunterclaw {
namespace memory {

// Forward declarations from types.h
enum class MemorySource;
struct MemorySearchResult;
struct SearchOptions;
struct MemoryProviderStatus;

// ============================================================================
// Memory - 基础记忆单元
// ============================================================================
struct Memory {
    std::string id;                                    // 唯一ID
    std::string content;                               // 记忆内容
    std::map<std::string, std::string> metadata;       // 元数据
    int64_t created_at;                                // 创建时间戳
    int64_t updated_at;                                // 更新时间戳
    std::optional<std::vector<float>> embedding;       // 向量嵌入(可选)
};

// ============================================================================
// MemoryStats - 记忆统计信息
// ============================================================================
struct MemoryStats {
    int64_t total_memories;                            // 总记忆数
    int64_t total_size;                                // 总大小(字节)
    std::optional<int64_t> last_updated;               // 最后更新时间
    std::optional<std::map<std::string, int64_t>> metadata_counts;  // 元数据统计
};

// ============================================================================
// SessionMemoryContext - Session作用域的Memory上下文
// ============================================================================
struct SessionMemoryContext {
    std::string session_id;                            // Session ID
    std::string agent_id;                              // Agent ID
    std::optional<std::string> workspace_dir;          // 工作目录
    std::map<std::string, std::string> metadata;       // 额外元数据
};

// ============================================================================
// MemoryProvider - 统一的Memory Provider接口
// 
// 对齐OpenClaw Memory Provider接口设计
// 支持多种实现: SQLite, MemOS Cloud, Supabase, etc.
// ============================================================================
class MemoryProvider {
public:
    virtual ~MemoryProvider() = default;
    
    // ========================================
    // Provider Identification
    // ========================================
    
    // Provider的唯一ID
    virtual std::string provider_id() const = 0;
    
    // Provider的显示名称
    virtual std::string provider_name() const = 0;
    
    // Provider的版本号
    virtual std::string provider_version() const = 0;
    
    // ========================================
    // Memory CRUD Operations
    // ========================================
    
    // 添加记忆
    // @param content 记忆内容
    // @param metadata 元数据 (例如: {"tag": "work", "priority": "high"})
    // @return 记忆ID
    virtual core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) = 0;
    
    // 搜索记忆
    // @param query 查询字符串
    // @param options 搜索选项 (最大结果数、最小分数等)
    // @return 匹配的记忆搜索结果列表
    virtual core::Result<std::vector<MemorySearchResult>> search_memories(
        const std::string& query,
        const SearchOptions& options = {}
    ) = 0;
    
    // 删除记忆
    // @param id 记忆ID
    // @return 是否成功删除
    virtual core::Result<bool> remove_memory(const std::string& id) = 0;
    
    // 更新记忆
    // @param id 记忆ID
    // @param content 新内容
    // @param metadata 新元数据
    // @return 是否成功更新
    virtual core::Result<bool> update_memory(
        const std::string& id,
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) = 0;
    
    // 清空所有记忆 (危险操作!)
    // @return 是否成功清空
    virtual core::Result<bool> clear_memories() = 0;
    
    // ========================================
    // Memory Retrieval
    // ========================================
    
    // 根据ID获取记忆
    // @param id 记忆ID
    // @return 记忆对象
    virtual core::Result<Memory> get_memory(const std::string& id) = 0;
    
    // 获取所有记忆 (分页)
    // @param limit 最大数量 (可选)
    // @param offset 偏移量 (可选)
    // @return 记忆列表
    virtual core::Result<std::vector<Memory>> list_memories(
        std::optional<int> limit = std::nullopt,
        std::optional<int> offset = std::nullopt
    ) {
        // 默认实现: 返回空列表
        return core::Result<std::vector<Memory>>::ok({});
    }
    
    // ========================================
    // Statistics & Status
    // ========================================
    
    // 获取统计信息
    // @return 统计数据
    virtual core::Result<MemoryStats> get_stats() = 0;
    
    // 获取Provider状态
    // @return 状态信息
    virtual core::Result<MemoryProviderStatus> get_status() {
        // 默认实现: 返回基础状态
        MemoryProviderStatus status;
        status.backend = "custom";
        status.provider = provider_id();
        return core::Result<MemoryProviderStatus>::ok(status);
    }
    
    // ========================================
    // Lifecycle Hooks (Optional)
    // ========================================
    
    // 初始化Provider
    // @param config 配置参数
    // @return 是否成功初始化
    virtual core::Result<bool> initialize(
        const std::map<std::string, std::string>& config = {}
    ) {
        // 默认实现: 无需初始化
        return core::Result<bool>::ok(true);
    }
    
    // 关闭Provider (清理资源)
    virtual void shutdown() {
        // 默认实现: 无需清理
    }
    
    // 健康检查
    // @return 是否健康
    virtual bool is_healthy() const {
        return true;
    }
};

} // namespace memory
} // namespace hunterclaw
