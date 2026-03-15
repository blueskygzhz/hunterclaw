#pragma once

#include <string>
#include <vector>
#include <memory>
#include "hunterclaw/core/result.h"

namespace hunterclaw {
namespace memory {

// Memory Storage 接口
struct MemoryEntry {
    std::string id;
    std::string content;
    std::string source;
    int64_t timestamp = 0;
};

class MemoryStorage {
public:
    virtual ~MemoryStorage() = default;
    
    virtual core::Result<void> store(const MemoryEntry& entry) = 0;
    virtual core::Result<std::vector<MemoryEntry>> search(
        const std::string& query, 
        int limit = 10
    ) = 0;
};

} // namespace memory
} // namespace hunterclaw
