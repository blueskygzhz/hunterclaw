#pragma once

// 严格对齐 OpenClaw config/sessions/group.ts (149行)
// Group session key 解析: surface推导、legacy group、implicit group

#include <optional>
#include <string>

namespace hunterclaw {
namespace sessions {

// ============================================================================
// Types (对齐 group.ts + types.ts GroupKeyResolution)
// ============================================================================

struct GroupKeyResolution {
  std::string key;                          // Resolved group session key
  std::optional<std::string> display_name;  // Human-readable group name
  std::optional<std::string> channel;       // Originating channel
  std::optional<std::string> chat_type;     // "group" | "channel"
};

// ============================================================================
// Functions (对齐 group.ts:18-149)
// ============================================================================

/// Resolve group session key from message context.
/// Returns nullopt if the message is not a group message.
std::optional<GroupKeyResolution> resolve_group_session_key(
    const std::string& from,
    const std::optional<std::string>& channel = std::nullopt,
    const std::optional<std::string>& chat_type = std::nullopt,
    const std::optional<std::string>& group_id = std::nullopt,
    const std::optional<std::string>& group_name = std::nullopt
);

/// Build group display name from channel + group info.
std::string build_group_display_name(
    const std::string& channel,
    const std::optional<std::string>& group_name = std::nullopt,
    const std::optional<std::string>& group_id = std::nullopt
);

}  // namespace sessions
}  // namespace hunterclaw
