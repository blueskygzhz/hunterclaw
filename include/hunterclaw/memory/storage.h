#pragma once

#include <string>
#include <vector>
#include <memory>
#include "hunterclaw/core/result.h"
#include "hunterclaw/memory/memory_provider.h"  // For MemoryEntry definition

namespace hunterclaw {
namespace memory {

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
