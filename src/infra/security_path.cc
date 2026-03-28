// 🎯 对齐 OpenClaw: src/gateway/security-path.ts
// Implements: canonicalizePathForSecurity, buildCanonicalPathCandidates,
//             isPathProtectedByPrefixes, isProtectedPluginRoutePath

#include "hunterclaw/security/security_path.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace hunterclaw {
namespace security {

// ============================================================================
// Internal helpers
// ============================================================================

// 🎯 对齐 OpenClaw: normalizePathSeparators(pathname)
std::string normalize_path_separators(const std::string& pathname) {
    if (pathname.empty()) return "/";

    // Collapse consecutive slashes
    std::string result;
    result.reserve(pathname.size());
    bool last_slash = false;
    for (char c : pathname) {
        if (c == '/') {
            if (!last_slash) result += c;
            last_slash = true;
        } else {
            result += c;
            last_slash = false;
        }
    }

    // Strip trailing slash (unless length <= 1)
    if (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }
    return result.empty() ? "/" : result;
}

// 🎯 对齐 OpenClaw: resolveDotSegments(pathname) via URL parsing
static std::string resolve_dot_segments(const std::string& pathname) {
    // Resolve . and .. segments (POSIX path normalization)
    std::vector<std::string> parts;
    std::istringstream ss(pathname);
    std::string token;
    bool absolute = (!pathname.empty() && pathname[0] == '/');

    while (std::getline(ss, token, '/')) {
        if (token.empty() || token == ".") {
            continue;
        } else if (token == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(token);
        }
    }

    std::string result = absolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "/";
        result += parts[i];
    }
    return result.empty() ? "/" : result;
}

// 🎯 对齐 OpenClaw: normalizePathForSecurity(pathname)
std::string normalize_path_for_security(const std::string& pathname) {
    // lowercase + resolve dot segments + normalize separators
    std::string lower = pathname;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string resolved = resolve_dot_segments(lower);
    std::string normalized = normalize_path_separators(resolved);
    return normalized.empty() ? "/" : normalized;
}

// 🎯 对齐 OpenClaw: pushNormalizedCandidate(candidates, seen, value)
static void push_normalized_candidate(
    std::vector<std::string>& candidates,
    std::set<std::string>& seen,
    const std::string& value
) {
    std::string normalized = normalize_path_for_security(value);
    if (seen.count(normalized)) return;
    seen.insert(normalized);
    candidates.push_back(normalized);
}

// 🎯 对齐 OpenClaw: URL decodeURIComponent (single pass)
std::string url_decode_once(const std::string& input, bool& ok) {
    ok = true;
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%') {
            if (i + 2 >= input.size()) {
                ok = false;
                return input;
            }
            char h1 = input[i + 1];
            char h2 = input[i + 2];
            if (!std::isxdigit(static_cast<unsigned char>(h1)) ||
                !std::isxdigit(static_cast<unsigned char>(h2))) {
                ok = false;
                return input;
            }
            auto hex_val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            char decoded = static_cast<char>((hex_val(h1) << 4) | hex_val(h2));
            result += decoded;
            i += 2;
        } else {
            result += input[i];
        }
    }
    return result;
}

// ============================================================================
// Public API
// ============================================================================

// 🎯 对齐 OpenClaw: buildCanonicalPathCandidates(pathname, maxDecodePasses)
CanonicalPathCandidatesResult build_canonical_path_candidates(
    const std::string& pathname,
    int max_decode_passes
) {
    CanonicalPathCandidatesResult result;
    std::set<std::string> seen;

    push_normalized_candidate(result.candidates, seen, pathname);

    std::string decoded = pathname;
    bool malformed = false;
    int passes = 0;

    for (int pass = 0; pass < max_decode_passes; ++pass) {
        bool ok = true;
        std::string next_decoded = url_decode_once(decoded, ok);
        if (!ok) {
            malformed = true;
            break;
        }
        if (next_decoded == decoded) {
            break;
        }
        passes += 1;
        decoded = next_decoded;
        push_normalized_candidate(result.candidates, seen, decoded);
    }

    // Check if decode pass limit was reached
    bool limit_reached = false;
    if (!malformed) {
        bool ok = true;
        std::string further = url_decode_once(decoded, ok);
        if (!ok) {
            malformed = true;
        } else {
            limit_reached = (further != decoded);
        }
    }

    result.decode_passes = passes;
    result.decode_pass_limit_reached = limit_reached;
    result.malformed_encoding = malformed;
    return result;
}

// 🎯 对齐 OpenClaw: canonicalizePathVariant(pathname)
std::string canonicalize_path_variant(const std::string& pathname) {
    auto res = build_canonical_path_candidates(pathname);
    if (res.candidates.empty()) return "/";
    return res.candidates.back();
}

// 🎯 对齐 OpenClaw: canonicalizePathForSecurity(pathname)
SecurityPathCanonicalization canonicalize_path_for_security(const std::string& pathname) {
    auto res = build_canonical_path_candidates(pathname);
    SecurityPathCanonicalization out;
    out.canonical_path = res.candidates.empty() ? "/" : res.candidates.back();
    out.candidates = res.candidates;
    out.decode_passes = res.decode_passes;
    out.decode_pass_limit_reached = res.decode_pass_limit_reached;
    out.malformed_encoding = res.malformed_encoding;
    out.raw_normalized_path = normalize_path_separators(
        // lowercase only, no dot-segment resolution
        [&]() {
            std::string lower = pathname;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            return normalize_path_separators(lower);
        }()
    );
    if (out.raw_normalized_path.empty()) out.raw_normalized_path = "/";
    return out;
}

// 🎯 对齐 OpenClaw: hasSecurityPathCanonicalizationAnomaly(pathname)
bool has_security_path_canonicalization_anomaly(const std::string& pathname) {
    auto canonical = canonicalize_path_for_security(pathname);
    return canonical.malformed_encoding || canonical.decode_pass_limit_reached;
}

// 🎯 对齐 OpenClaw: prefixMatch(pathname, prefix)
static bool prefix_match(const std::string& pathname, const std::string& prefix) {
    // pathname === prefix
    if (pathname == prefix) return true;
    // pathname.startsWith(`${prefix}/`)
    if (pathname.size() > prefix.size() &&
        pathname.substr(0, prefix.size()) == prefix &&
        pathname[prefix.size()] == '/') {
        return true;
    }
    // Fail closed: pathname.startsWith(`${prefix}%`)
    if (pathname.size() > prefix.size() &&
        pathname.substr(0, prefix.size()) == prefix &&
        pathname[prefix.size()] == '%') {
        return true;
    }
    return false;
}

// 🎯 对齐 OpenClaw: normalizeProtectedPrefix(prefix)
static std::string normalize_protected_prefix(const std::string& prefix) {
    std::string lower = prefix;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string normalized = normalize_path_separators(lower);
    return normalized.empty() ? "/" : normalized;
}

// 🎯 对齐 OpenClaw: isPathProtectedByPrefixes(pathname, prefixes)
bool is_path_protected_by_prefixes(
    const std::string& pathname,
    const std::vector<std::string>& prefixes
) {
    auto canonical = canonicalize_path_for_security(pathname);

    // Normalize prefixes
    std::vector<std::string> normalized_prefixes;
    normalized_prefixes.reserve(prefixes.size());
    for (const auto& p : prefixes) {
        normalized_prefixes.push_back(normalize_protected_prefix(p));
    }

    // Check all candidates against all prefixes
    for (const auto& candidate : canonical.candidates) {
        for (const auto& prefix : normalized_prefixes) {
            if (prefix_match(candidate, prefix)) {
                return true;
            }
        }
    }

    // Fail closed when canonicalization depth cannot be fully resolved
    if (canonical.decode_pass_limit_reached) {
        return true;
    }

    if (!canonical.malformed_encoding) {
        return false;
    }

    // Malformed encoding: fall back to rawNormalizedPath
    for (const auto& prefix : normalized_prefixes) {
        if (prefix_match(canonical.raw_normalized_path, prefix)) {
            return true;
        }
    }
    return false;
}

// 🎯 对齐 OpenClaw: isProtectedPluginRoutePath(pathname)
bool is_protected_plugin_route_path(const std::string& pathname) {
    return is_path_protected_by_prefixes(pathname, PROTECTED_PLUGIN_ROUTE_PREFIXES);
}

}  // namespace security
}  // namespace hunterclaw
