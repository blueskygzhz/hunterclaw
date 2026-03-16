#include "hunterclaw/memory/memory_provider_adapter.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/manager_search.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace hunterclaw {
namespace memory {

MemoryProviderAdapter::MemoryProviderAdapter(
    std::shared_ptr<MemoryManager> memory_mgr
) : memory_mgr_(memory_mgr) {
    if (!memory_mgr_) {
        throw std::invalid_argument("MemoryManager cannot be null");
    }
    spdlog::debug("[MemoryProviderAdapter] Created adapter for MemoryManager");
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
        
        // 调用 MemoryManager 的 search (返回 SearchRowResult)
        auto search_results = memory_mgr_->search(query, opts);
        
        // 转换 SearchRowResult -> MemoryEntry
        std::vector<MemoryEntry> entries;
        entries.reserve(search_results.size());
        
        for (const auto& result : search_results) {
            MemoryEntry entry;
            entry.id = result.id;
            entry.content = result.snippet;  // 使用 snippet 作为 content
            entry.file_path = result.path;
            entry.timestamp = 0;  // SearchRowResult 没有 timestamp,使用当前时间
            
            entries.push_back(entry);
        }
        
        spdlog::debug("[MemoryProviderAdapter] Search returned {} results for query: {}",
                    entries.size(), query.substr(0, 50));
        
        return core::Result<std::vector<MemoryEntry>>::Ok(entries);
        
    } catch (const std::exception& e) {
        std::string error_msg = "Search failed: " + std::string(e.what());
        spdlog::error("[MemoryProviderAdapter] {}", error_msg);
        return core::Result<std::vector<MemoryEntry>>::Err(error_msg);
    }
}

core::Result<void> MemoryProviderAdapter::add_memory(const MemoryEntry& entry) {
    // ✅ P4 优先级完成: 使用 MemoryManager 的 add_entry() 方法
    // 
    // OpenClaw 实现 (src/memory/builtin-manager.ts):
    // 1. 调用底层 provider 的 store() 方法
    // 2. 自动生成 embedding (如果配置了)
    // 3. 保存到 vector DB
    
    try {
        // ✅ 使用 MemoryManager 的 add_entry() 方法
        auto result = memory_mgr_->add_entry(entry);
        
        if (result.is_ok()) {
            spdlog::debug("[MemoryProviderAdapter] Added memory: id={}, content_len={}",
                        entry.id, entry.content.length());
        } else {
            spdlog::error("[MemoryProviderAdapter] Failed to add memory: {}", result.error);
        }
        
        return result;
        
    } catch (const std::exception& e) {
        std::string error_msg = "Add memory failed: " + std::string(e.what());
        spdlog::error("[MemoryProviderAdapter] {}", error_msg);
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
        
        if (result.is_ok()) {
            spdlog::info("[MemoryProviderAdapter] Reindex completed successfully");
        } else {
            spdlog::error("[MemoryProviderAdapter] Reindex failed: {}", result.error);
        }
        
        return result;
        
    } catch (const std::exception& e) {
        std::string error_msg = "Reindex failed: " + std::string(e.what());
        spdlog::error("[MemoryProviderAdapter] {}", error_msg);
        return core::Result<void>::Err(error_msg);
    }
}

} // namespace memory
} // namespace hunterclaw
