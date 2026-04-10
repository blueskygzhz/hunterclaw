#pragma once

// 严格对齐 OpenClaw: sessions/session-chat-type-shared.ts (68行)
// Best-effort chat-type extraction from session keys.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace sessions {

// ============================================================================
// 对齐 OpenClaw session-chat-type-shared.ts:4
// ============================================================================

enum class SessionKeyChatType {
    DIRECT,
    GROUP,
    CHANNEL,
    UNKNOWN
};

std::string session_key_chat_type_to_string(SessionKeyChatType type);
std::optional<SessionKeyChatType> string_to_session_key_chat_type(const std::string& str);

// ============================================================================
// 对齐 OpenClaw session-chat-type-shared.ts:24-51
// ============================================================================

using LegacyChatTypeDeriver = std::function<std::optional<SessionKeyChatType>(const std::string&)>;

/**
 * Derive chat type from scoped session key.
 * Checks tokens (group/channel/direct/dm), then built-in legacy patterns,
 * then external legacy derivers.
 */
SessionKeyChatType derive_session_chat_type_from_scoped_key(
    const std::string& scoped_session_key,
    const std::vector<LegacyChatTypeDeriver>& legacy_derivers = {}
);

/**
 * Best-effort chat-type extraction from full session key.
 * Strips agent prefix, then delegates to scoped key derivation.
 */
SessionKeyChatType derive_session_chat_type_from_key(
    const std::string& session_key,
    const std::vector<LegacyChatTypeDeriver>& legacy_derivers = {}
);

}  // namespace sessions
}  // namespace hunterclaw
