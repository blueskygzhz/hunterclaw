#pragma once

#include <string>

// 严格对齐 OpenClaw src/memory/manager-search-preflight.ts (33行)

namespace hunterclaw {
namespace memory {

// ============================================================================
// SearchPreflightResult (对齐 manager-search-preflight.ts:1-11)
// ============================================================================
struct SearchPreflightResult {
    std::string normalized_query;
    bool should_initialize_provider = false;
    bool should_search = false;
};

// ============================================================================
// resolveMemorySearchPreflight (对齐 manager-search-preflight.ts:1-32)
// ============================================================================
SearchPreflightResult resolve_memory_search_preflight(
    const std::string& query,
    bool has_indexed_content
);

} // namespace memory
} // namespace hunterclaw
