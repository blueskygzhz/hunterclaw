#ifndef HUNTERCLAW_SESSIONS_LEVEL_OVERRIDES_H
#define HUNTERCLAW_SESSIONS_LEVEL_OVERRIDES_H

#include <string>
#include <optional>
#include "hunterclaw/react/verbose_reasoning_config.h"
#include "hunterclaw/sessions/session_manager.h"

namespace hunterclaw {
namespace sessions {

/**
 * Parse a verbose level override value.
 * Aligned with OpenClaw: parseVerboseOverride()
 *
 * Returns:
 *   ok=true, value=nullopt  → raw was undefined/not provided
 *   ok=true, value=null     → raw was null (clear override)
 *   ok=true, value=level    → valid level
 *   ok=false, error=...     → invalid value
 */
struct ParseVerboseOverrideResult {
    bool ok = false;
    std::string error;
    // value semantics:
    //   has_value=false, is_null=false → undefined (not provided)
    //   has_value=false, is_null=true  → null (clear override)
    //   has_value=true                 → valid level
    bool is_null = false;
    std::optional<react::VerboseLevel> value;
};

inline ParseVerboseOverrideResult parse_verbose_override(const std::string* raw) {
    // raw == nullptr means "undefined" (not provided)
    if (raw == nullptr) {
        return {true, "", false, std::nullopt};
    }

    // Empty string treated as null (clear override)
    if (raw->empty()) {
        return {true, "", true, std::nullopt};
    }

    auto level = react::parse_verbose_level(*raw);
    if (!level) {
        return {false, "invalid verboseLevel (use \"on\"|\"off\")", false, std::nullopt};
    }
    return {true, "", false, level};
}

/**
 * Apply a verbose level override to a session entry.
 * Aligned with OpenClaw: applyVerboseOverride()
 *
 * - level == nullopt (undefined) → no-op
 * - level == null (is_null=true) → clear entry.verbose_level
 * - level == valid level         → set entry.verbose_level
 */
inline void apply_verbose_override(
    SessionEntry& entry,
    const ParseVerboseOverrideResult& result
) {
    if (!result.ok) return;
    if (!result.value && !result.is_null) {
        // undefined → no-op
        return;
    }
    if (result.is_null) {
        // null → clear
        entry.verbose_level = std::nullopt;
        return;
    }
    // Set level
    if (result.value) {
        entry.verbose_level = react::to_string(*result.value);
    }
}

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_LEVEL_OVERRIDES_H
