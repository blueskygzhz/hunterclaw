#pragma once

// 严格对齐 OpenClaw: channels/targets.ts (149行)
// Messaging target parsing framework — prefix/mention/@user/channel target resolution

#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace hunterclaw {
namespace channels {

// ============================================================================
// MessagingTargetKind (对齐 OC: MessagingTargetKind)
// ============================================================================

enum class MessagingTargetKind { USER, CHANNEL };

inline std::string messaging_target_kind_string(MessagingTargetKind kind) {
    return kind == MessagingTargetKind::USER ? "user" : "channel";
}

// ============================================================================
// MessagingTarget (对齐 OC: MessagingTarget)
// ============================================================================

struct MessagingTarget {
    MessagingTargetKind kind;
    std::string id;
    std::string raw;
    std::string normalized;
};

// ============================================================================
// MessagingTargetParseOptions (对齐 OC)
// ============================================================================

struct MessagingTargetParseOptions {
    std::optional<MessagingTargetKind> default_kind;
    std::optional<std::string> ambiguous_message;
};

// ============================================================================
// Target construction helpers (对齐 OC)
// ============================================================================

/// Normalize target id: "kind:id" lowercased
std::string normalize_target_id(MessagingTargetKind kind, const std::string& id);

/// Build a MessagingTarget
MessagingTarget build_messaging_target(MessagingTargetKind kind,
                                        const std::string& id,
                                        const std::string& raw);

/// Validate candidate id matches pattern, throw on mismatch (对齐 OC: ensureTargetId)
std::string ensure_target_id(const std::string& candidate,
                              const std::regex& pattern,
                              const std::string& error_message);

// ============================================================================
// Target parsing functions (对齐 OC)
// ============================================================================

/// Parse a @mention target (对齐 OC: parseTargetMention)
std::optional<MessagingTarget> parse_target_mention(
    const std::string& raw,
    const std::regex& mention_pattern,
    MessagingTargetKind kind);

/// Parse a prefix target e.g. "user:123" (对齐 OC: parseTargetPrefix)
std::optional<MessagingTarget> parse_target_prefix(
    const std::string& raw,
    const std::string& prefix,
    MessagingTargetKind kind);

/// Parse from multiple prefixes (对齐 OC: parseTargetPrefixes)
struct PrefixEntry {
    std::string prefix;
    MessagingTargetKind kind;
};

std::optional<MessagingTarget> parse_target_prefixes(
    const std::string& raw,
    const std::vector<PrefixEntry>& prefixes);

/// Parse @user target (对齐 OC: parseAtUserTarget)
std::optional<MessagingTarget> parse_at_user_target(
    const std::string& raw,
    const std::regex& pattern,
    const std::string& error_message);

/// Combined: mention → prefixes → @user (对齐 OC: parseMentionPrefixOrAtUserTarget)
std::optional<MessagingTarget> parse_mention_prefix_or_at_user_target(
    const std::string& raw,
    const std::regex& mention_pattern,
    const std::vector<PrefixEntry>& prefixes,
    const std::regex& at_user_pattern,
    const std::string& at_user_error_message);

/// Require a specific target kind, throw if missing/wrong (对齐 OC: requireTargetKind)
std::string require_target_kind(
    const std::string& platform,
    const std::optional<MessagingTarget>& target,
    MessagingTargetKind kind);

} // namespace channels
} // namespace hunterclaw
