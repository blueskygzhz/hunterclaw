// Aligned with openclaw: src/channels/allowlist-match.ts
#pragma once

#include <string>
#include <vector>
#include <set>
#include <optional>
#include <algorithm>

namespace hunterclaw {
namespace channels {

// ============================================================================
// AllowlistMatch
// Aligned with OpenClaw: allowlist-match.ts
// ============================================================================

// Aligned with OpenClaw: AllowlistMatchSource
enum class AllowlistMatchSource {
    Wildcard,     // "wildcard"
    Id,           // "id"
    Name,         // "name"
    Tag,          // "tag"
    Username,     // "username"
    PrefixedId,   // "prefixed-id"
    PrefixedUser, // "prefixed-user"
    PrefixedName, // "prefixed-name"
    Slug,         // "slug"
    Localpart     // "localpart"
};

inline std::string to_string(AllowlistMatchSource source) {
    switch (source) {
        case AllowlistMatchSource::Wildcard:     return "wildcard";
        case AllowlistMatchSource::Id:           return "id";
        case AllowlistMatchSource::Name:         return "name";
        case AllowlistMatchSource::Tag:          return "tag";
        case AllowlistMatchSource::Username:     return "username";
        case AllowlistMatchSource::PrefixedId:   return "prefixed-id";
        case AllowlistMatchSource::PrefixedUser: return "prefixed-user";
        case AllowlistMatchSource::PrefixedName: return "prefixed-name";
        case AllowlistMatchSource::Slug:         return "slug";
        case AllowlistMatchSource::Localpart:    return "localpart";
    }
    return "id";
}

// Aligned with OpenClaw: AllowlistMatch
struct AllowlistMatch {
    bool allowed = false;
    std::optional<std::string> match_key;
    std::optional<AllowlistMatchSource> match_source;
};

// Aligned with OpenClaw: CompiledAllowlist
struct CompiledAllowlist {
    std::set<std::string> set;
    bool wildcard = false;
};

// Aligned with OpenClaw: formatAllowlistMatchMeta()
inline std::string format_allowlist_match_meta(const AllowlistMatch& match) {
    std::string key = match.match_key.value_or("none");
    std::string source = match.match_source
        ? to_string(*match.match_source)
        : "none";
    return "matchKey=" + key + " matchSource=" + source;
}

// Aligned with OpenClaw: compileAllowlist()
inline CompiledAllowlist compile_allowlist(const std::vector<std::string>& entries) {
    CompiledAllowlist result;
    for (const auto& e : entries) {
        if (!e.empty()) result.set.insert(e);
    }
    result.wildcard = result.set.count("*") > 0;
    return result;
}

// Aligned with OpenClaw: resolveAllowlistCandidates()
struct AllowlistCandidate {
    std::optional<std::string> value;
    AllowlistMatchSource source;
};

inline AllowlistMatch resolve_allowlist_candidates(
    const CompiledAllowlist& compiled,
    const std::vector<AllowlistCandidate>& candidates)
{
    for (const auto& candidate : candidates) {
        if (!candidate.value) continue;
        if (compiled.set.count(*candidate.value)) {
            return {true, *candidate.value, candidate.source};
        }
    }
    return {false, std::nullopt, std::nullopt};
}

// Aligned with OpenClaw: resolveCompiledAllowlistMatch()
inline AllowlistMatch resolve_compiled_allowlist_match(
    const CompiledAllowlist& compiled,
    const std::vector<AllowlistCandidate>& candidates)
{
    if (compiled.set.empty()) return {false, std::nullopt, std::nullopt};
    if (compiled.wildcard) {
        return {true, "*", AllowlistMatchSource::Wildcard};
    }
    return resolve_allowlist_candidates(compiled, candidates);
}

// Aligned with OpenClaw: resolveAllowlistMatchByCandidates()
inline AllowlistMatch resolve_allowlist_match_by_candidates(
    const std::vector<std::string>& allow_list,
    const std::vector<AllowlistCandidate>& candidates)
{
    return resolve_compiled_allowlist_match(
        compile_allowlist(allow_list),
        candidates
    );
}

// Aligned with OpenClaw: resolveAllowlistMatchSimple()
inline AllowlistMatch resolve_allowlist_match_simple(
    const std::vector<std::string>& allow_from,
    const std::string& sender_id,
    const std::string& sender_name = "",
    bool allow_name_matching = false)
{
    // Compile with lowercase
    std::vector<std::string> normalized;
    for (const auto& e : allow_from) {
        std::string lower = e;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        size_t s = lower.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        size_t e2 = lower.find_last_not_of(" \t\r\n");
        lower = lower.substr(s, e2 - s + 1);
        if (!lower.empty()) normalized.push_back(lower);
    }
    auto compiled = compile_allowlist(normalized);

    if (compiled.set.empty()) return {false, std::nullopt, std::nullopt};
    if (compiled.wildcard) return {true, "*", AllowlistMatchSource::Wildcard};

    std::string lower_id = sender_id;
    std::transform(lower_id.begin(), lower_id.end(), lower_id.begin(), ::tolower);
    std::string lower_name = sender_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    std::vector<AllowlistCandidate> candidates;
    candidates.push_back({lower_id, AllowlistMatchSource::Id});
    if (allow_name_matching && !lower_name.empty()) {
        candidates.push_back({lower_name, AllowlistMatchSource::Name});
    }

    return resolve_allowlist_candidates(compiled, candidates);
}

} // namespace channels
} // namespace hunterclaw
