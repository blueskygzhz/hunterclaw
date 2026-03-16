#pragma once

#include "hunterclaw/memory/memory_provider.h"
#include "hunterclaw/memory/memory_manager.h"
#include "hunterclaw/core/result.h"
#include <memory>
#include <string>
#include <vector>

namespace hunterclaw {
namespace memory {

/**
 * MemoryProviderAdapter - 适配器模式
 * 
 * ✅ 严格对齐 OpenClaw (P3 优先级实现)
 * 
 * 目的: 将 MemoryManager 包装为 MemoryProvider 接口,提供统一的 Memory 访问方式
 * 
 * OpenClaw 对应:
 * - MemoryProvider 接口 (src/memory/types.ts)
 * - 适配器模式: 包装不同的 memory backend (vector DB, SQLite, in-memory)
 * 
 * HunterClaw 实现:
 * - MemoryManager: 完整的 memory 管理器 (compaction, session, search)
 * - MemoryProvider: 简化的接口 (search, add_memory, reindex)
 * - MemoryProviderAdapter: 连接两者,使 AgentInstanceContext.memory 可以使用
 * 
 * 使用场景:
 * ```cpp
 * auto memory_mgr = std::make_shared<MemoryManager>(...);
 * ctx_.memory = std::make_shared<MemoryProviderAdapter>(memory_mgr);
 * 
 * // 通过统一接口搜索
 * auto results = ctx_.memory->search("user query", 10);
 * ```
 * 
 * 优势:
 * 1. 统一接口: AgentInstanceContext 只需要 MemoryProvider,不依赖 MemoryManager
 * 2. 解耦: MemoryManager 的复杂功能 (compaction, session) 隐藏在适配器后
 * 3. 兼容性: 未来可以切换不同的 Memory 实现 (如 QMD, remote memory)
 */
class MemoryProviderAdapter : public MemoryProvider {
public:
    /**
     * 构造函数
     * @param memory_mgr MemoryManager 实例 (shared_ptr 保持生命周期)
     */
    explicit MemoryProviderAdapter(std::shared_ptr<MemoryManager> memory_mgr);
    
    ~MemoryProviderAdapter() override = default;
    
    // ========================================
    // MemoryProvider 接口实现
    // ========================================
    
    /**
     * 搜索 memory
     * 
     * 对齐 OpenClaw: MemoryProvider.search()
     * 
     * @param query 查询字符串 (自然语言或关键词)
     * @param limit 返回结果数量限制
     * @return 搜索结果 (MemoryEntry 列表)
     */
    core::Result<std::vector<MemoryEntry>> search(
        const std::string& query,
        int limit = 10
    ) override;
    
    /**
     * 添加 memory
     * 
     * 对齐 OpenClaw: MemoryProvider.store()
     * 
     * @param entry Memory 条目 (id, content, file_path, timestamp)
     * @return 成功/失败
     */
    core::Result<void> add_memory(const MemoryEntry& entry) override;
    
    /**
     * 重建索引
     * 
     * 对齐 OpenClaw: MemoryProvider.reindex()
     * 
     * @return 成功/失败
     */
    core::Result<void> reindex() override;
    
    // ========================================
    // 扩展方法 (可选)
    // ========================================
    
    /**
     * 获取底层 MemoryManager (用于高级操作)
     * 
     * 使用场景:
     * - 需要访问 CompactionManager
     * - 需要访问 SessionMemoryProvider
     * - 需要执行复杂的 memory 操作
     */
    std::shared_ptr<MemoryManager> underlying_manager() const {
        return memory_mgr_;
    }
    
private:
    std::shared_ptr<MemoryManager> memory_mgr_;
};

} // namespace memory
} // namespace hunterclaw
