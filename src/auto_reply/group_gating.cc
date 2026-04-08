// Aligned with openclaw: src/web/auto-reply/monitor/group-gating.ts
// Group message gating: allowlist filtering, activation mode (mention/always),
// /activation command parsing, pending history recording.

#include "hunterclaw/auto_reply/group_gating.h"
#include "hunterclaw/auto_reply/group_history.h"
#include "hunterclaw/auto_reply/command_detection.h"
#include "hunterclaw/core/logging.h"
#include <algorithm>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// GroupActivation helpers
// Aligned with openclaw: src/auto-reply/group-activation.ts
// ============================================================================

std::optional<GroupActivationMode> normalize_group_activation(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    // Trim
    size_t s = lower.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = lower.find_last_not_of(" \t\r\n");
    lower = lower.substr(s, e - s + 1);

    if (lower == "mention") return GroupActivationMode::Mention;
    if (lower == "always")  return GroupActivationMode::Always;
    return std::nullopt;
}

GroupActivationParseResult parse_activation_command(const std::string& raw) {
    if (raw.empty()) return {false, std::nullopt};
    std::string trimmed = raw;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return {false, std::nullopt};
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);

    // Aligned with openclaw: /activation(?:\s+([a-zA-Z]+))?\s*$
    std::string normalized = CommandDetection::normalize_command_body(trimmed);
    static const std::regex activation_re(
        R"(^/activation(?:\s+([a-zA-Z]+))?\s*$)",
        std::regex::icase
    );
    std::smatch match;
    if (!std::regex_match(normalized, match, activation_re)) {
        return {false, std::nullopt};
    }
    if (match[1].matched) {
        return {true, normalize_group_activation(match[1].str())};
    }
    return {true, std::nullopt};
}

// ============================================================================
// SendPolicy helpers
// Aligned with openclaw: src/auto-reply/send-policy.ts
// ============================================================================

std::optional<SendPolicyOverride> normalize_send_policy_override(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    size_t s = lower.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = lower.find_last_not_of(" \t\r\n");
    lower = lower.substr(s, e - s + 1);

    if (lower == "allow" || lower == "on")  return SendPolicyOverride::Allow;
    if (lower == "deny" || lower == "off")  return SendPolicyOverride::Deny;
    return std::nullopt;
}

SendPolicyParseResult parse_send_policy_command(const std::string& raw) {
    if (raw.empty()) return {false, std::nullopt};
    std::string trimmed = raw;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return {false, std::nullopt};
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);

    std::string normalized = CommandDetection::normalize_command_body(trimmed);
    static const std::regex send_re(
        R"(^/send(?:\s+([a-zA-Z]+))?\s*$)",
        std::regex::icase
    );
    std::smatch match;
    if (!std::regex_match(normalized, match, send_re)) {
        return {false, std::nullopt};
    }
    if (!match[1].matched) return {true, std::nullopt};

    std::string token = match[1].str();
    std::transform(token.begin(), token.end(), token.begin(), ::tolower);

    if (token == "inherit" || token == "default" || token == "reset") {
        return {true, SendPolicyOverride::Inherit};
    }
    auto mode = normalize_send_policy_override(token);
    return {true, mode};
}

// ============================================================================
// apply_group_gating
// Aligned with openclaw: applyGroupGating (group-gating.ts)
//
// Pipeline:
//   1. Check group allowlist (if enabled)
//   2. Record group member names
//   3. Resolve activation mode (mention/always)
//   4. Handle /activation command (owner-only)
//   5. Apply mention gating — skip if not mentioned (store in history)
// ============================================================================

GroupGatingResult apply_group_gating(const GroupGatingParams& params) {
    auto logger = core::get_logger("group_gating");

    // Step 1: Group allowlist check
    if (params.group_allowlist_enabled && !params.group_allowed) {
        logger->debug("Skipping group message {} (not in allowlist)",
                      params.conversation_id);
        return {false};
    }

    // Step 2: Record group member name (for display in history)
    // (caller is responsible for maintaining the member name map)

    // Step 3: Resolve activation mode
    GroupActivationMode activation = params.default_activation;
    bool require_mention = (activation == GroupActivationMode::Mention);

    // Step 4: Handle /activation command (owner-only)
    auto activation_cmd = parse_activation_command(params.msg_body);
    if (activation_cmd.has_command && params.is_owner_sender) {
        // Owner can change activation mode
        if (activation_cmd.mode.has_value()) {
            return {true, activation_cmd.mode};
        }
        return {true, std::nullopt};  // /activation without argument → show current
    }

    // Step 5: Mention gating
    if (require_mention && !params.was_mentioned) {
        // Not mentioned → store in group history for context, but skip processing
        logger->debug("Group message stored for context (no mention) in {}: {}",
                      params.conversation_id,
                      params.msg_body.substr(0, 80));

        // Record pending history entry
        if (params.history_map && params.history_limit > 0) {
            GroupHistoryEntry entry;
            entry.sender = params.sender_display;
            entry.body = params.msg_body;
            entry.timestamp = params.timestamp;
            entry.message_id = params.message_id;

            record_pending_history_entry(
                *params.history_map,
                params.group_history_key,
                entry,
                params.history_limit
            );
        }

        return {false};
    }

    // Passed all gating checks → should process
    return {true};
}

} // namespace auto_reply
} // namespace hunterclaw
