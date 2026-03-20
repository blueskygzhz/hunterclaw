// DEPRECATED: MemoryProviderAdapter wrapping MemoryManager does not exist in openclaw.
// openclaw alignment: MemoryIndexManager IS the MemorySearchManager directly.
// Use MemoryIndexManager::get() to obtain a MemorySearchManager instance.
//
// Migration guide:
//   Before: auto adapter = std::make_shared<MemoryProviderAdapter>(memory_mgr);
//   After:  auto mgr = MemoryIndexManager::get(params).get();
//           // mgr is already a MemorySearchManager
#pragma once
#include "hunterclaw/memory/memory_index_manager.h"

namespace hunterclaw {
namespace memory {

// MemoryProviderAdapter is no longer needed.
// MemoryIndexManager directly implements MemorySearchManager.
// This alias is provided for source compatibility only.
using MemoryProviderAdapter = MemoryIndexManager;

} // namespace memory
} // namespace hunterclaw
