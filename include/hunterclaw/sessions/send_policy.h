#ifndef HUNTERCLAW_SESSIONS_SEND_POLICY_H
#define HUNTERCLAW_SESSIONS_SEND_POLICY_H

#include <string>
#include <optional>
#include <vector>
#include <algorithm>
#include "hunterclaw/sessions/session_key_utils.h"
#include "hunterclaw/sessions/session_manager.h"

namespace hunterclaw {
namespace sessions {

// Send policy decision (aligned with OpenClaw: SessionSendPolicyDecision)
enum class SendPolicyDecision {
    Allow,  // "allow"
    Deny    // "deny"
};

// Send policy rule match criteria (aligned with OpenClaw send-policy rules)
struct SendPolicyRuleMatch {
    std::optional<std::string> channel;      // match by channel
    std::optional<std::string> chat_type;    // match by chat type
    std::optional<std::string> key_prefix;   // match by session key prefix (stripped)
    std::optional<std::string> raw_key_prefix; // match by raw session key prefix
};

// Send policy rule (aligned with OpenClaw send-policy rule)
struct SendPolicyRule {
    std::string action;  // "allow" | "deny"
    SendPolicyRuleMatch match;
};

// Send policy config (aligned with OpenClaw session.sendPolicy)
struct SendPolicyConfig {
    std::string default_action;              // "allow" | "deny", default "allow"
    std::vector<SendPolicyRule> rules;
};

/**
 * Normalize a send policy string to a decision.
 * Aligned with OpenClaw: normalizeSendPolicy()
 */
inline std::optional<SendPolicyDecision> normalize_send_policy(const std::string& raw) {
    std::string value = raw;
    // Trim and lowercase
    size_t s = value.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = value.find_last_not_of(" \t\r\n");
    value = value.substr(s, e - s + 1);
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);

    if (value == "allow") return SendPolicyDecision::Allow;
    if (value == "deny") return SendPolicyDecision::Deny;
    return std::nullopt;
}

/**
 * Normalize a match value (trim + lowercase).
 */
inline std::optional<std::string> normalize_match_value(const std::string& raw) {
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = raw.find_last_not_of(" \t\r\n");
    std::string value = raw.substr(s, e - s + 1);
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    return value.empty() ? std::nullopt : std::optional<std::string>{value};
}

/**
 * Strip agent session key prefix (agent:<agentId>:<rest> → <rest>).
 * Aligned with OpenClaw: stripAgentSessionKeyPrefix()
 */
inline std::optional<std::string> strip_agent_session_key_prefix(const std::string& key) {
    if (key.empty()) return std::nullopt;
    auto parsed = parse_agent_session_key(key);
    if (parsed) return parsed->rest;
    return key;
}

/**
 * Derive channel from session key.
 * Aligned with OpenClaw: deriveChannelFromKey()
 */
inline std::optional<std::string> derive_channel_from_key(const std::string& key) {
    auto normalized_key = strip_agent_session_key_prefix(key);
    if (!normalized_key) return std::nullopt;

    // Split by ':'
    std::vector<std::string> parts;
    std::string tok;
    for (char c : *normalized_key) {
        if (c == ':') {
            if (!tok.empty()) parts.push_back(tok);
            tok.clear();
        } else {
            tok += c;
        }
    }
    if (!tok.empty()) parts.push_back(tok);

    if (parts.size() >= 3 && (parts[1] == "group" || parts[1] == "channel")) {
        return normalize_match_value(parts[0]);
    }
    return std::nullopt;
}

// Parameters for resolve_send_policy (aligned with OpenClaw resolveSendPolicy params)
struct ResolveSendPolicyParams {
    std::optional<SendPolicyConfig> policy_config;  // from config: session.sendPolicy
    std::optional<std::string> entry_send_policy;   // from session entry
    std::optional<std::string> session_key;
    std::optional<std::string> channel;
    std::optional<std::string> entry_channel;
    std::optional<std::string> entry_last_channel;
    std::optional<std::string> chat_type;
    std::optional<std::string> entry_chat_type;
};

/**
 * Resolve the send policy decision for a session.
 * Aligned with OpenClaw: resolveSendPolicy()
 */
inline SendPolicyDecision resolve_send_policy(const ResolveSendPolicyParams& params) {
    // Check session entry override first
    if (params.entry_send_policy) {
        auto override_decision = normalize_send_policy(*params.entry_send_policy);
        if (override_decision) return *override_decision;
    }

    // No policy config → allow
    if (!params.policy_config) return SendPolicyDecision::Allow;
    const auto& policy = *params.policy_config;

    // Derive channel
    std::optional<std::string> channel;
    if (params.channel) channel = normalize_match_value(*params.channel);
    if (!channel && params.entry_channel) channel = normalize_match_value(*params.entry_channel);
    if (!channel && params.entry_last_channel) channel = normalize_match_value(*params.entry_last_channel);
    if (!channel && params.session_key) channel = derive_channel_from_key(*params.session_key);

    // Derive chat type
    std::optional<std::string> chat_type;
    if (params.chat_type) {
        std::string ct = *params.chat_type;
        std::transform(ct.begin(), ct.end(), ct.begin(), ::tolower);
        if (ct == "direct" || ct == "group" || ct == "channel") chat_type = ct;
    }
    if (!chat_type && params.entry_chat_type) {
        std::string ct = *params.entry_chat_type;
        std::transform(ct.begin(), ct.end(), ct.begin(), ::tolower);
        if (ct == "direct" || ct == "group" || ct == "channel") chat_type = ct;
    }
    if (!chat_type && params.session_key) {
        auto derived = derive_session_chat_type(*params.session_key);
        if (derived != SessionKeyChatType::Unknown) {
            switch (derived) {
                case SessionKeyChatType::Direct:  chat_type = "direct"; break;
                case SessionKeyChatType::Group:   chat_type = "group"; break;
                case SessionKeyChatType::Channel: chat_type = "channel"; break;
                default: break;
            }
        }
    }

    // Build normalized session key strings
    std::string raw_session_key = params.session_key.value_or("");
    std::string stripped_session_key = strip_agent_session_key_prefix(raw_session_key).value_or(raw_session_key);
    std::string raw_session_key_norm = raw_session_key;
    std::transform(raw_session_key_norm.begin(), raw_session_key_norm.end(), raw_session_key_norm.begin(), ::tolower);
    std::string stripped_session_key_norm = stripped_session_key;
    std::transform(stripped_session_key_norm.begin(), stripped_session_key_norm.end(), stripped_session_key_norm.begin(), ::tolower);

    bool allowed_match = false;
    for (const auto& rule : policy.rules) {
        auto action = normalize_send_policy(rule.action).value_or(SendPolicyDecision::Allow);
        const auto& match = rule.match;

        // Check channel match
        if (match.channel) {
            auto match_channel = normalize_match_value(*match.channel);
            if (match_channel && match_channel != channel) continue;
        }

        // Check chat type match
        if (match.chat_type) {
            std::string match_ct = *match.chat_type;
            std::transform(match_ct.begin(), match_ct.end(), match_ct.begin(), ::tolower);
            if (!match_ct.empty() && (!chat_type || match_ct != *chat_type)) continue;
        }

        // Check raw key prefix
        if (match.raw_key_prefix) {
            auto match_raw_prefix = normalize_match_value(*match.raw_key_prefix);
            if (match_raw_prefix && raw_session_key_norm.substr(0, match_raw_prefix->size()) != *match_raw_prefix) continue;
        }

        // Check key prefix (stripped or raw)
        if (match.key_prefix) {
            auto match_prefix = normalize_match_value(*match.key_prefix);
            if (match_prefix) {
                bool raw_starts = raw_session_key_norm.size() >= match_prefix->size() &&
                                  raw_session_key_norm.substr(0, match_prefix->size()) == *match_prefix;
                bool stripped_starts = stripped_session_key_norm.size() >= match_prefix->size() &&
                                       stripped_session_key_norm.substr(0, match_prefix->size()) == *match_prefix;
                if (!raw_starts && !stripped_starts) continue;
            }
        }

        if (action == SendPolicyDecision::Deny) return SendPolicyDecision::Deny;
        allowed_match = true;
    }

    if (allowed_match) return SendPolicyDecision::Allow;

    // Fallback to default
    auto fallback = normalize_send_policy(policy.default_action);
    return fallback.value_or(SendPolicyDecision::Allow);
}

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_SEND_POLICY_H
