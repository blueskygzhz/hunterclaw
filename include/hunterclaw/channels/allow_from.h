// Aligned with openclaw: src/channels/allow-from.ts
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <algorithm>

namespace hunterclaw {
namespace channels {

// ============================================================================
// AllowFrom utilities
// Aligned with OpenClaw: allow-from.ts
// ============================================================================

/**
 * Merge DM allow-from sources.
 * Aligned with OpenClaw: mergeDmAllowFromSources()
 */
inline std::vector<std::string> merge_dm_allow_from_sources(
    const std::vector<std::string>& allow_from,
    const std::vector<std::string>& store_allow_from,
    const std::string& dm_policy = "")
{
    std::vector<std::string> result;
    for (const auto& v : allow_from) {
        std::string trimmed = v;
        size_t s = trimmed.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        size_t e = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(s, e - s + 1);
        if (!trimmed.empty()) result.push_back(trimmed);
    }
    // store entries only when dm_policy != "allowlist"
    if (dm_policy != "allowlist") {
        for (const auto& v : store_allow_from) {
            std::string trimmed = v;
            size_t s = trimmed.find_first_not_of(" \t\r\n");
            if (s == std::string::npos) continue;
            size_t e = trimmed.find_last_not_of(" \t\r\n");
            trimmed = trimmed.substr(s, e - s + 1);
            if (!trimmed.empty()) result.push_back(trimmed);
        }
    }
    return result;
}

/**
 * Resolve group allow-from sources.
 * Aligned with OpenClaw: resolveGroupAllowFromSources()
 */
inline std::vector<std::string> resolve_group_allow_from_sources(
    const std::vector<std::string>& allow_from,
    const std::vector<std::string>& group_allow_from,
    bool fallback_to_allow_from = true)
{
    const std::vector<std::string>* scoped = nullptr;
    static const std::vector<std::string> empty;

    if (!group_allow_from.empty()) {
        scoped = &group_allow_from;
    } else if (!fallback_to_allow_from) {
        scoped = &empty;
    } else {
        scoped = &allow_from;
    }

    std::vector<std::string> result;
    for (const auto& v : *scoped) {
        std::string trimmed = v;
        size_t s = trimmed.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        size_t e = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(s, e - s + 1);
        if (!trimmed.empty()) result.push_back(trimmed);
    }
    return result;
}

/**
 * Check if a sender ID is in the allow list.
 * Aligned with OpenClaw: isSenderIdAllowed()
 */
struct AllowEntries {
    std::vector<std::string> entries;
    bool has_wildcard = false;
    bool has_entries = false;
};

inline bool is_sender_id_allowed(
    const AllowEntries& allow,
    const std::string& sender_id,
    bool allow_when_empty)
{
    if (!allow.has_entries) return allow_when_empty;
    if (allow.has_wildcard) return true;
    if (sender_id.empty()) return false;
    return std::find(allow.entries.begin(), allow.entries.end(), sender_id) != allow.entries.end();
}

} // namespace channels
} // namespace hunterclaw
