#pragma once

#include <string>
#include <vector>
#include <optional>

// 严格对齐 OpenClaw extensions/memory-core/src/memory/qmd-compat.ts (8行)
// QMD collection pattern flag 兼容性处理

namespace hunterclaw {
namespace memory {

// 对齐 QmdCollectionPatternFlag type (qmd-compat.ts:1)
// "--glob" = modern qmd, "--mask" = legacy qmd
using QmdCollectionPatternFlag = std::string;

// 对齐 resolveQmdCollectionPatternFlags (qmd-compat.ts:3-7)
// preferredFlag == "--mask" → ["--mask", "--glob"]
// else (null or "--glob") → ["--glob", "--mask"]
std::vector<QmdCollectionPatternFlag> resolve_qmd_collection_pattern_flags(
    const std::optional<QmdCollectionPatternFlag>& preferred_flag
);

}  // namespace memory
}  // namespace hunterclaw
