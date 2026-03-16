#pragma once

#include "hunterclaw/memory/provider.h"  // Use complete provider definition
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>

namespace hunterclaw {
namespace memory {

// Memory Entry - simplified structure for adapter use
struct MemoryEntry {
    std::string id;
    std::string content;
    std::string file_path;
    int64_t timestamp = 0;
};

} // namespace memory
} // namespace hunterclaw
