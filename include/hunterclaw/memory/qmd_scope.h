#ifndef HUNTERCLAW_MEMORY_QMD_SCOPE_H_
#define HUNTERCLAW_MEMORY_QMD_SCOPE_H_

#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// QMD Scope Configuration
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 对齐: openclaw-analysis/src/memory/qmd-scope.ts

/**
 * Chat type classification
 */
enum class ChatType {
    CHANNEL,
    GROUP,
    DIRECT
};

/**
 * Parsed session scope information
 */
struct ParsedQmdSessionScope {
    std::optional<std::string> channel;
    std::optional<ChatType> chat_type;
    std::optional<std::string> normalized_key;
};

/**
 * Scope rule match criteria
 */
struct QmdScopeMatch {
    std::optional<std::string> channel;
    std::optional<ChatType> chat_type;
    std::optional<std::string> key_prefix;
    std::optional<std::string> raw_key_prefix;
};

/**
 * Single scope rule
 */
struct QmdScopeRule {
    QmdScopeMatch match;
    std::string action;  // "allow" or "deny"
};

/**
 * Complete scope configuration
 */
struct QmdScopeConfig {
    std::vector<QmdScopeRule> rules;
    std::string default_action = "allow";  // "allow" or "deny"
};

/**
 * Check if session is allowed by scope rules
 * 
 * @param scope Scope configuration
 * @param session_key Session key to check
 * @return true if allowed, false if denied
 */
bool is_qmd_scope_allowed(
    const QmdScopeConfig& scope,
    const std::optional<std::string>& session_key = std::nullopt
);

/**
 * Extract channel from session key
 */
std::optional<std::string> derive_qmd_scope_channel(
    const std::optional<std::string>& key = std::nullopt
);

/**
 * Extract chat type from session key
 */
std::optional<ChatType> derive_qmd_scope_chat_type(
    const std::optional<std::string>& key = std::nullopt
);

/**
 * Parse session key into scope components
 */
ParsedQmdSessionScope parse_qmd_session_scope(
    const std::optional<std::string>& key = std::nullopt
);

/**
 * Normalize session key for matching
 * Removes agent: prefix and lowercases
 */
std::optional<std::string> normalize_qmd_session_key(
    const std::optional<std::string>& key = std::nullopt
);

/**
 * Convert ChatType to string
 */
std::string chat_type_to_string(ChatType type);

/**
 * Parse ChatType from string
 */
std::optional<ChatType> parse_chat_type(const std::string& str);

}  // namespace memory
}  // namespace hunterclaw

#endif  // HUNTERCLAW_MEMORY_QMD_SCOPE_H_
