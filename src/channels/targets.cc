#include "hunterclaw/channels/targets.h"
#include <algorithm>
#include <stdexcept>

namespace hunterclaw {
namespace channels {

namespace {
    std::string to_lower(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }

    std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\n\r");
        return s.substr(start, end - start + 1);
    }
} // namespace

// ============================================================================
// normalize_target_id (对齐 OC: normalizeTargetId)
// ============================================================================

std::string normalize_target_id(MessagingTargetKind kind, const std::string& id) {
    return to_lower(messaging_target_kind_string(kind) + ":" + id);
}

// ============================================================================
// build_messaging_target (对齐 OC: buildMessagingTarget)
// ============================================================================

MessagingTarget build_messaging_target(MessagingTargetKind kind,
                                        const std::string& id,
                                        const std::string& raw) {
    return MessagingTarget{
        kind,
        id,
        raw,
        normalize_target_id(kind, id)
    };
}

// ============================================================================
// ensure_target_id (对齐 OC: ensureTargetId)
// ============================================================================

std::string ensure_target_id(const std::string& candidate,
                              const std::regex& pattern,
                              const std::string& error_message) {
    if (!std::regex_match(candidate, pattern)) {
        throw std::runtime_error(error_message);
    }
    return candidate;
}

// ============================================================================
// parse_target_mention (对齐 OC: parseTargetMention)
// ============================================================================

std::optional<MessagingTarget> parse_target_mention(
    const std::string& raw,
    const std::regex& mention_pattern,
    MessagingTargetKind kind) {
    std::smatch match;
    if (!std::regex_match(raw, match, mention_pattern) || match.size() < 2) {
        return std::nullopt;
    }
    std::string id = match[1].str();
    if (id.empty()) return std::nullopt;
    return build_messaging_target(kind, id, raw);
}

// ============================================================================
// parse_target_prefix (对齐 OC: parseTargetPrefix)
// ============================================================================

std::optional<MessagingTarget> parse_target_prefix(
    const std::string& raw,
    const std::string& prefix,
    MessagingTargetKind kind) {
    if (raw.size() < prefix.size()) return std::nullopt;
    if (raw.substr(0, prefix.size()) != prefix) return std::nullopt;
    std::string id = trim(raw.substr(prefix.size()));
    if (id.empty()) return std::nullopt;
    return build_messaging_target(kind, id, raw);
}

// ============================================================================
// parse_target_prefixes (对齐 OC: parseTargetPrefixes)
// ============================================================================

std::optional<MessagingTarget> parse_target_prefixes(
    const std::string& raw,
    const std::vector<PrefixEntry>& prefixes) {
    for (const auto& entry : prefixes) {
        auto parsed = parse_target_prefix(raw, entry.prefix, entry.kind);
        if (parsed) return parsed;
    }
    return std::nullopt;
}

// ============================================================================
// parse_at_user_target (对齐 OC: parseAtUserTarget)
// ============================================================================

std::optional<MessagingTarget> parse_at_user_target(
    const std::string& raw,
    const std::regex& pattern,
    const std::string& error_message) {
    if (raw.empty() || raw[0] != '@') return std::nullopt;
    std::string candidate = trim(raw.substr(1));
    std::string id = ensure_target_id(candidate, pattern, error_message);
    return build_messaging_target(MessagingTargetKind::USER, id, raw);
}

// ============================================================================
// parse_mention_prefix_or_at_user_target (对齐 OC: parseMentionPrefixOrAtUserTarget)
// ============================================================================

std::optional<MessagingTarget> parse_mention_prefix_or_at_user_target(
    const std::string& raw,
    const std::regex& mention_pattern,
    const std::vector<PrefixEntry>& prefixes,
    const std::regex& at_user_pattern,
    const std::string& at_user_error_message) {
    auto mention = parse_target_mention(raw, mention_pattern, MessagingTargetKind::USER);
    if (mention) return mention;
    auto prefixed = parse_target_prefixes(raw, prefixes);
    if (prefixed) return prefixed;
    return parse_at_user_target(raw, at_user_pattern, at_user_error_message);
}

// ============================================================================
// require_target_kind (对齐 OC: requireTargetKind)
// ============================================================================

std::string require_target_kind(
    const std::string& platform,
    const std::optional<MessagingTarget>& target,
    MessagingTargetKind kind) {
    std::string kind_label = messaging_target_kind_string(kind);
    if (!target) {
        throw std::runtime_error(platform + " " + kind_label + " id is required.");
    }
    if (target->kind != kind) {
        throw std::runtime_error(platform + " " + kind_label + " id is required (use " + kind_label + ":<id>).");
    }
    return target->id;
}

} // namespace channels
} // namespace hunterclaw
