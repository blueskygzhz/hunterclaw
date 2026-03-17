#include "hunterclaw/memory/memory_provider_adapter.h"
#include "hunterclaw/memory/types.h"
#include <chrono>

namespace hunterclaw {
namespace memory {

MemoryProviderAdapter::MemoryProviderAdapter(
    std::shared_ptr<MemoryManager> memory_mgr
) : memory_mgr_(memory_mgr) {
    if (!memory_mgr_) {
        throw std::invalid_argument("MemoryManager cannot be null");
    }
    // TODO: Add logging
}

core::Result<std::vector<MemoryEntry>> MemoryProviderAdapter::search(
    const std::string& query,
    int limit
) {
    // ✅ 严格对齐 OpenClaw: 通过 MemoryManager 搜索
    // 
    // OpenClaw 实现 (src/memory/builtin-manager.ts):
    // 1. 调用底层 provider 的 search() 方法
    // 2. 返回 Memory[] 结果
    // 3. 支持 filters, min_score, hybrid search
    
    try {
        // 构建 SearchOptions
        SearchOptions opts;
        opts.max_results = limit;
        opts.min_score = 0.0;  // 不过滤相似度 (由调用者决定)
        
        // 调用 MemoryManager 的 search_memories (返回 Memory[])
        auto result = memory_mgr_->search_memories(query, opts);
        
        if (!result.is_ok()) {
            return core::Result<std::vector<MemoryEntry>>::Err(result.error);
        }
        
        // 转换 Memory -> MemoryEntry
        std::vector<MemoryEntry> entries;
        entries.reserve(result.value.size());
        
        for (const auto& memory : result.value) {
            MemoryEntry entry;
            entry.id = memory.id;
            entry.content = memory.content;
            entry.file_path = memory.metadata.count("file_path") ? memory.metadata.at("file_path") : "";
            entry.timestamp = memory.created_at;
            
            entries.push_back(entry);
        }
        
        // TODO: Add logging
        return core::Result<std::vector<MemoryEntry>>::Ok(entries);
        
    } catch (const std::exception& e) {
        std::string error_msg = "Search failed: " + std::string(e.what());
        // TODO: Add logging
        return core::Result<std::vector<MemoryEntry>>::Err(error_msg);
    }
}

core::Result<void> MemoryProviderAdapter::add_memory_entry(const MemoryEntry& entry) {
    // ✅ P4 优先级完成: 使用 MemoryManager 的 add_entry() 方法
    // 
    // OpenClaw 实现 (src/memory/builtin-manager.ts):
    // 1. 调用底层 provider 的 store() 方法
    // 2. 自动生成 embedding (如果配置了)
    // 3. 保存到 vector DB
    
    try {
        // ✅ 使用 MemoryManager 的 add_entry() 方法
        auto result = memory_mgr_->add_entry(entry);
        
        // TODO: Add logging
        return result;
        
    } catch (const std::exception& e) {
        std::string error_msg = "Add memory failed: " + std::string(e.what());
        // TODO: Add logging
        return core::Result<void>::Err(error_msg);
    }
}

core::Result<void> MemoryProviderAdapter::reindex() {
    // ✅ P4 优先级完成: 使用 MemoryManager 的 reindex() 方法
    // 
    // OpenClaw 实现 (src/memory/builtin-manager.ts):
    // 1. 清空现有索引
    // 2. 重新索引所有文件
    // 3. 生成新的 embeddings
    
    try {
        // ✅ 使用 MemoryManager 的 reindex() 方法
        auto result = memory_mgr_->reindex();
        
        // TODO: Add logging
        return result;
        
    } catch (const std::exception& e) {
        std::string error_msg = "Reindex failed: " + std::string(e.what());
        // TODO: Add logging
        return core::Result<void>::Err(error_msg);
    }
}

} // namespace memory
} // namespace hunterclaw
