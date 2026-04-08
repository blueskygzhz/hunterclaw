// 🎯 对齐 OpenClaw: src/slack/monitor/room-context.ts
//                    src/discord/monitor/inbound-context.ts
// 实现 channel 上下文中对 build_untrusted_channel_metadata 的调用

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace channels {

// Forward declaration from security/channel_metadata.cc
namespace security {
    std::optional<std::string> build_untrusted_channel_metadata(
        const std::string& source,
        const std::string& label,
        const std::vector<std::string>& entries,
        int max_chars = 800
    );
}

// ============================================================================
// resolve_slack_room_context_hints
// 严格对齐 OpenClaw: src/slack/monitor/room-context.ts
// resolveSlackRoomContextHints(params)
// ============================================================================

struct SlackRoomContextHints {
    std::optional<std::string> untrusted_channel_metadata;
    std::optional<std::string> group_system_prompt;
};

SlackRoomContextHints resolve_slack_room_context_hints(
    bool is_roomish,
    const std::optional<std::string>& channel_topic,
    const std::optional<std::string>& channel_purpose,
    const std::optional<std::string>& channel_system_prompt
) {
    SlackRoomContextHints result;
    if (!is_roomish) return result;

    // 🎯 对齐 OpenClaw: buildUntrustedChannelMetadata({
    //   source: "slack",
    //   label: "Slack channel description",
    //   entries: [params.channelInfo?.topic, params.channelInfo?.purpose],
    // })
    std::vector<std::string> entries;
    if (channel_topic.has_value()) entries.push_back(*channel_topic);
    if (channel_purpose.has_value()) entries.push_back(*channel_purpose);

    result.untrusted_channel_metadata = security::build_untrusted_channel_metadata(
        "slack", "Slack channel description", entries
    );

    // 🎯 对齐 OpenClaw: systemPromptParts
    if (channel_system_prompt.has_value() && !channel_system_prompt->empty()) {
        std::string trimmed = *channel_system_prompt;
        // Trim
        size_t s = trimmed.find_first_not_of(" \t\r\n");
        size_t e = trimmed.find_last_not_of(" \t\r\n");
        if (s != std::string::npos) {
            trimmed = trimmed.substr(s, e - s + 1);
            if (!trimmed.empty()) {
                result.group_system_prompt = trimmed;
            }
        }
    }

    return result;
}

// ============================================================================
// build_discord_untrusted_context
// 严格对齐 OpenClaw: src/discord/monitor/inbound-context.ts
// buildDiscordUntrustedContext(params)
// ============================================================================

std::optional<std::vector<std::string>> build_discord_untrusted_context(
    bool is_guild,
    const std::optional<std::string>& channel_topic
) {
    if (!is_guild) return std::nullopt;

    // 🎯 对齐 OpenClaw: buildUntrustedChannelMetadata({
    //   source: "discord",
    //   label: "Discord channel topic",
    //   entries: [params.channelTopic],
    // })
    std::vector<std::string> entries;
    if (channel_topic.has_value()) entries.push_back(*channel_topic);

    auto metadata = security::build_untrusted_channel_metadata(
        "discord", "Discord channel topic", entries
    );

    if (!metadata.has_value()) return std::nullopt;
    return std::vector<std::string>{*metadata};
}

// ============================================================================
// build_discord_group_system_prompt
// 严格对齐 OpenClaw: buildDiscordGroupSystemPrompt
// ============================================================================

std::optional<std::string> build_discord_group_system_prompt(
    const std::optional<std::string>& channel_system_prompt
) {
    if (!channel_system_prompt.has_value()) return std::nullopt;
    std::string trimmed = *channel_system_prompt;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return std::nullopt;
    return trimmed;
}

}  // namespace channels
}  // namespace hunterclaw
