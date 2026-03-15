#include <optional>
#include "hunterclaw/memory/qmd_scope.h"
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Helper Functions
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static std::string trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

static std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

static bool starts_with(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() && 
           str.compare(0, prefix.size(), prefix) == 0;
}

static std::vector<std::string> split(const std::string& str, char delim) {
    std::vector<std::string> parts;
    std::istringstream iss(str);
    std::string part;
    while (std::getline(iss, part, delim)) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ChatType Conversion
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::string chat_type_to_string(ChatType type) {
    switch (type) {
        case ChatType::CHANNEL: return "channel";
        case ChatType::GROUP: return "group";
        case ChatType::DIRECT: return "direct";
    }
    return "direct";
}

std::optional<ChatType> parse_chat_type(const std::string& str) {
    std::string lower = to_lower(trim(str));
    if (lower == "channel") return ChatType::CHANNEL;
    if (lower == "group") return ChatType::GROUP;
    if (lower == "direct" || lower == "dm") return ChatType::DIRECT;
    return std::nullopt;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Session Key Normalization
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::optional<std::string> normalize_qmd_session_key(
    const std::optional<std::string>& key
) {
    if (!key.has_value()) {
        return std::nullopt;
    }
    
    std::string trimmed = trim(key.value());
    if (trimmed.empty()) {
        return std::nullopt;
    }
    
    // Simple normalization: parse agent session key format
    // Full implementation would use parseAgentSessionKey from sessions module
    std::string normalized = to_lower(trimmed);
    
    // Remove "agent:" prefix if present
    if (starts_with(normalized, "agent:")) {
        normalized = normalized.substr(6);
    }
    
    // Skip subagent keys
    if (starts_with(normalized, "subagent:")) {
        return std::nullopt;
    }
    
    return normalized;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Session Scope Parsing
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

ParsedQmdSessionScope parse_qmd_session_scope(
    const std::optional<std::string>& key
) {
    auto normalized = normalize_qmd_session_key(key);
    if (!normalized.has_value()) {
        return {};
    }
    
    auto parts = split(normalized.value(), ':');
    
    // Pattern: channel:type:...
    if (parts.size() >= 2) {
        auto maybe_type = parse_chat_type(parts[1]);
        if (maybe_type.has_value()) {
            ParsedQmdSessionScope result;
            result.normalized_key = normalized;
            result.channel = to_lower(parts[0]);
            result.chat_type = maybe_type.value();
            return result;
        }
    }
    
    // Check for embedded type markers
    std::string norm = normalized.value();
    if (norm.find(":group:") != std::string::npos) {
        return {std::nullopt, ChatType::GROUP, normalized};
    }
    if (norm.find(":channel:") != std::string::npos) {
        return {std::nullopt, ChatType::CHANNEL, normalized};
    }
    
    // Default to direct
    return {std::nullopt, ChatType::DIRECT, normalized};
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Derived Properties
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::optional<std::string> derive_qmd_scope_channel(
    const std::optional<std::string>& key
) {
    return parse_qmd_session_scope(key).channel;
}

std::optional<ChatType> derive_qmd_scope_chat_type(
    const std::optional<std::string>& key
) {
    return parse_qmd_session_scope(key).chat_type;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Scope Rule Matching
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool is_qmd_scope_allowed(
    const QmdScopeConfig& scope,
    const std::optional<std::string>& session_key
) {
    // Parse session components
    auto parsed = parse_qmd_session_scope(session_key);
    auto channel = parsed.channel;
    auto chat_type = parsed.chat_type;
    auto normalized_key = parsed.normalized_key.value_or("");
    auto raw_key = to_lower(trim(session_key.value_or("")));
    
    // Check each rule
    for (const auto& rule : scope.rules) {
        const auto& match = rule.match;
        
        // Check channel match
        if (match.channel.has_value() && match.channel != channel) {
            continue;
        }
        
        // Check chat type match
        if (match.chat_type.has_value() && match.chat_type != chat_type) {
            continue;
        }
        
        // Check raw key prefix
        if (match.raw_key_prefix.has_value()) {
            std::string raw_prefix = to_lower(trim(match.raw_key_prefix.value()));
            if (!starts_with(raw_key, raw_prefix)) {
                continue;
            }
        }
        
        // Check normalized key prefix
        if (match.key_prefix.has_value()) {
            std::string norm_prefix = to_lower(trim(match.key_prefix.value()));
            
            // Backward compat: legacy configs used "agent:<id>:..." for raw keys
            bool is_legacy_raw = starts_with(norm_prefix, "agent:");
            if (is_legacy_raw) {
                if (!starts_with(raw_key, norm_prefix)) {
                    continue;
                }
            } else {
                if (!starts_with(normalized_key, norm_prefix)) {
                    continue;
                }
            }
        }
        
        // Rule matched
        return rule.action == "allow";
    }
    
    // No rule matched, use default
    return scope.default_action == "allow";
}

}  // namespace memory
}  // namespace hunterclaw
