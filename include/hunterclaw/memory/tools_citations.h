#pragma once

// ============================================================================
// Memory Tools Citations — 严格对齐 OpenClaw tools.citations.ts (91 行)
//
// Citation 装饰、模式解析、结果裁剪。
// ============================================================================

#include "hunterclaw/memory/types.h"
#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace memory {

/**
 * Citation mode: "on" | "off" | "auto".
 * (对齐 tools.citations.ts:9-15)
 */
enum class MemoryCitationsMode { On, Off, Auto };

/**
 * Resolve citations mode from config.
 */
MemoryCitationsMode resolve_memory_citations_mode(const std::string& mode_str);

/**
 * Format a citation string for a search result.
 * (对齐 tools.citations.ts:31-37 formatCitation)
 */
std::string format_memory_citation(const MemorySearchResult& entry);

/**
 * Decorate results with citations if enabled.
 * (对齐 tools.citations.ts:17-29 decorateCitations)
 */
std::vector<MemorySearchResult> decorate_citations(
    const std::vector<MemorySearchResult>& results,
    bool include
);

/**
 * Clamp results by injected character budget.
 * (对齐 tools.citations.ts:39-63 clampResultsByInjectedChars)
 */
std::vector<MemorySearchResult> clamp_results_by_injected_chars(
    const std::vector<MemorySearchResult>& results,
    int budget = 0
);

/**
 * Should citations be included based on mode and session key?
 * (对齐 tools.citations.ts:65-76 shouldIncludeCitations)
 */
bool should_include_citations(
    MemoryCitationsMode mode,
    const std::string& session_key = ""
);

}  // namespace memory
}  // namespace hunterclaw
