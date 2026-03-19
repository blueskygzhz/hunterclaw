#pragma once

#include "hunterclaw/channels/plugin.h"
#include <string>
#include <vector>
#include <optional>
#include <map>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Channel Registry (100% 对齐 OpenClaw registry.ts)
// ============================================================================

/// Core channel order (对齐 OpenClaw CHAT_CHANNEL_ORDER)
inline const std::vector<std::string> CHAT_CHANNEL_ORDER = {
    "telegram",
    "whatsapp",
    "discord",
    "irc",
    "googlechat",
    "slack",
    "signal",
    "imessage",
    "line",
};

/// Channel aliases (对齐 OpenClaw CHAT_CHANNEL_ALIASES)
inline const std::map<std::string, std::string> CHAT_CHANNEL_ALIASES = {
    {"imsg",                "imessage"},
    {"internet-relay-chat", "irc"},
    {"google-chat",         "googlechat"},
    {"gchat",               "googlechat"},
};

/// Normalize a raw channel key to lowercase trimmed string
std::optional<std::string> normalize_channel_key(const std::string& raw);

/// Normalize a core chat channel id (对齐 OpenClaw normalizeChatChannelId)
std::optional<std::string> normalize_chat_channel_id(const std::string& raw);

/// Normalize any channel id including plugin-registered channels
/// (对齐 OpenClaw normalizeAnyChannelId)
std::optional<std::string> normalize_any_channel_id(const std::string& raw);

/// List all core chat channel metas (对齐 OpenClaw listChatChannels)
std::vector<ChannelMeta> list_chat_channels();

/// Get meta for a core chat channel
std::optional<ChannelMeta> get_chat_channel_meta(const std::string& id);

// ============================================================================
// ChannelDock (100% 对齐 OpenClaw dock.ts)
// ============================================================================

/// ChannelDock: lightweight channel metadata/behavior for shared code paths.
/// Rules (对齐 OpenClaw):
///   - keep this light (no monitors, probes, web login, etc)
///   - OK: config readers, allowFrom formatting, mention stripping, threading defaults
struct ChannelDock {
    ChannelId id;
    ChannelCapabilities capabilities;
    std::optional<ChannelCommandAdapter> commands;
    struct Outbound {
        std::optional<int> text_chunk_limit;
    };
    std::optional<Outbound> outbound;
    std::optional<ChannelStreamingAdapter> streaming;
    std::optional<ChannelElevatedAdapter> elevated;
    struct Config {
        std::function<std::optional<std::vector<std::string>>(const core::Config&, const std::string&)> resolve_allow_from;
        std::function<std::vector<std::string>(const core::Config&, const std::string&, const std::vector<std::string>&)> format_allow_from;
        std::function<std::optional<std::string>(const core::Config&, const std::string&)> resolve_default_to;
    };
    std::optional<Config> config;
    std::optional<ChannelGroupAdapter> groups;
    std::optional<ChannelMentionAdapter> mentions;
    std::optional<ChannelThreadingAdapter> threading;
    std::optional<ChannelAgentPromptAdapter> agent_prompt;
};

/// Build a ChannelDock from a ChannelPlugin (对齐 OpenClaw buildDockFromPlugin)
ChannelDock build_dock_from_plugin(const ChannelPlugin& plugin);

/// List all channel docks (core + plugin-registered), sorted by order
/// (对齐 OpenClaw listChannelDocks)
std::vector<ChannelDock> list_channel_docks();

/// Get dock for a specific channel id (对齐 OpenClaw getChannelDock)
std::optional<ChannelDock> get_channel_dock(const ChannelId& id);

} // namespace channels
} // namespace hunterclaw
