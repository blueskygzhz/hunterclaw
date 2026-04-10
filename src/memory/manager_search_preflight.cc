#include "hunterclaw/memory/manager_search_preflight.h"
#include <algorithm>

// 严格对齐 OpenClaw src/memory/manager-search-preflight.ts (33行)

namespace hunterclaw {
namespace memory {

// ============================================================================
// resolveMemorySearchPreflight (对齐 manager-search-preflight.ts:1-32)
// ============================================================================
SearchPreflightResult resolve_memory_search_preflight(
    const std::string& query,
    bool has_indexed_content
) {
    SearchPreflightResult result;

    // 对齐: const normalizedQuery = params.query.trim()
    std::string trimmed = query;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) {
        trimmed = "";
    } else {
        auto e = trimmed.find_last_not_of(" \t\n\r");
        trimmed = trimmed.substr(s, e - s + 1);
    }
    result.normalized_query = trimmed;

    // 对齐: if (!normalizedQuery) → don't search, don't init provider
    if (trimmed.empty()) {
        result.should_search = false;
        result.should_initialize_provider = false;
        return result;
    }

    // 对齐: if (!params.hasIndexedContent) → don't search, don't init provider
    if (!has_indexed_content) {
        result.should_search = false;
        result.should_initialize_provider = false;
        return result;
    }

    // 对齐: search and initialize provider
    result.should_search = true;
    result.should_initialize_provider = true;
    return result;
}

} // namespace memory
} // namespace hunterclaw
