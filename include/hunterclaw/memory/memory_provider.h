// DEPRECATED: MemoryProvider CRUD interface does not exist in openclaw.
// openclaw alignment: use MemorySearchManager from types.h instead.
// This file is kept as a compatibility shim only.
#pragma once
#include "hunterclaw/memory/types.h"

namespace hunterclaw {
namespace memory {

// Alias: MemoryProvider = MemorySearchManager (openclaw types.ts:61-81)
using MemoryProvider = MemorySearchManager;

// MemoryEntry: simplified structure for file-based memory entries
// (openclaw stores memory as .md files, not CRUD records)
struct MemoryEntry {
    std::string id;
    std::string content;
    std::string file_path;
    int64_t timestamp = 0;
};

} // namespace memory
} // namespace hunterclaw
