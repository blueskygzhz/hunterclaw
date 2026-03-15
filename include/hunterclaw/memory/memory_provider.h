#pragma once

#include <string>
#include <vector>
#include "hunterclaw/core/result.h"

namespace hunterclaw {
namespace memory {

// Forward declarations
struct AgentMessage;
struct MemoryEntry;

/**
 * Memory Provider 接口
 * 提供 memory 存储和检索功能
 */
class MemoryProvider {
public:
    virtual ~MemoryProvider() = default;
    
    // 搜索 memory
    virtual core::Result<std::vector<MemoryEntry>> search(
        const std::string& query,
        int limit = 10
    ) = 0;
    
    // 添加 memory
    virtual core::Result<void> add_memory(const MemoryEntry& entry) = 0;
    
    // 重建索引
    virtual core::Result<void> reindex() = 0;
};

// Memory Entry
struct MemoryEntry {
    std::string id;
    std::string content;
    std::string file_path;
    int64_t timestamp = 0;
};

} // namespace memory
} // namespace hunterclaw
