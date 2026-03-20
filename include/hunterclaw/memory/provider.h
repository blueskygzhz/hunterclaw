// DEPRECATED: MemoryProvider (CRUD interface) does not exist in openclaw.
// openclaw alignment: use MemorySearchManager from types.h instead.
// This file is kept as a compatibility shim only.
#pragma once
#include "hunterclaw/memory/types.h"

namespace hunterclaw {
namespace memory {

// Alias for backward compatibility: MemoryProvider = MemorySearchManager
// openclaw uses MemorySearchManager (types.ts:61-81) as the single interface.
using MemoryProvider = MemorySearchManager;

} // namespace memory
} // namespace hunterclaw
