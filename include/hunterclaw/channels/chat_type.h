#ifndef HUNTERCLAW_CHANNELS_CHAT_TYPE_H
#define HUNTERCLAW_CHANNELS_CHAT_TYPE_H

#include <string>
#include <optional>
#include <algorithm>

namespace hunterclaw {
namespace channels {

// Chat type (aligned with OpenClaw: ChatType = "direct" | "group" | "channel")
enum class ChatType {
    Direct,   // "direct" / "dm"
    Group,    // "group"
    Channel   // "channel"
};

/**
 * Normalize a chat type string.
 * Aligned with OpenClaw: normalizeChatType()
 */
inline std::optional<ChatType> normalize_chat_type(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string value = raw;
    size_t s = value.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = value.find_last_not_of(" \t\r\n");
    value = value.substr(s, e - s + 1);
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);

    if (value == "direct" || value == "dm") return ChatType::Direct;
    if (value == "group") return ChatType::Group;
    if (value == "channel") return ChatType::Channel;
    return std::nullopt;
}

/**
 * Convert ChatType to string.
 */
inline std::string to_string(ChatType type) {
    switch (type) {
        case ChatType::Direct:  return "direct";
        case ChatType::Group:   return "group";
        case ChatType::Channel: return "channel";
    }
    return "direct";
}

} // namespace channels
} // namespace hunterclaw

#endif // HUNTERCLAW_CHANNELS_CHAT_TYPE_H
