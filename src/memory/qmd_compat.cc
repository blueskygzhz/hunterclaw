#include "hunterclaw/memory/qmd_compat.h"

namespace hunterclaw {
namespace memory {

// 对齐 resolveQmdCollectionPatternFlags (qmd-compat.ts:3-7)
// export function resolveQmdCollectionPatternFlags(
//   preferredFlag: QmdCollectionPatternFlag | null,
// ): QmdCollectionPatternFlag[] {
//   return preferredFlag === "--mask" ? ["--mask", "--glob"] : ["--glob", "--mask"];
// }

std::vector<QmdCollectionPatternFlag> resolve_qmd_collection_pattern_flags(
    const std::optional<QmdCollectionPatternFlag>& preferred_flag
) {
    if (preferred_flag.has_value() && *preferred_flag == "--mask") {
        return {"--mask", "--glob"};
    }
    return {"--glob", "--mask"};
}

}  // namespace memory
}  // namespace hunterclaw
