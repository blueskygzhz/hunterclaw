#pragma once

// channel.h: legacy adapter interface (kept for backward compatibility).
// New code should use plugin.h (ChannelPlugin / ChannelGatewayAdapter).
#include "hunterclaw/channels/plugin.h"

namespace hunterclaw {
namespace channels {

// ============================================================================
// Legacy ChannelAdapter interface
// (kept for backward compatibility with existing channel stubs)
// New channels should implement ChannelPlugin + ChannelGatewayAdapter instead.
// ============================================================================

enum class ChannelCapabilityLegacy {
    SendText,
    SendImage,
    SendFile,
    SendVoice,
    SendVideo,
    ReceiveText,
    ReceiveImage,
    ReceiveFile,
    ReceiveVoice,
    ReceiveVideo,
    GroupChat,
    PrivateChat,
    ThreadedConversation
};

struct ChannelAdapterMeta {
    std::string id;
    std::string name;
    std::string description;
    std::vector<ChannelCapabilityLegacy> capabilities;
};

struct ChannelAccount {
    std::string account_id;
    std::string channel_id;
    std::string token;
    std::map<std::string, std::string> credentials;
    std::map<std::string, core::JsonValue> config;
};

enum class MessageType {
    Text, Image, File, Voice, Video, Sticker, Location, Contact, System
};

struct ChannelMessage {
    std::string id;
    std::string channel_id;
    std::string account_id;
    std::string from;
    std::string sender_name;
    std::string to;
    MessageType type;
    std::string text;
    std::vector<std::string> attachments;
    core::Timestamp timestamp;
    std::map<std::string, core::JsonValue> metadata;
};

struct ChannelAdapterContext {
    std::shared_ptr<core::Config> config;
    ChannelAccount account;
    std::string workspace_dir;
    std::string plugin_dir;
    std::function<void(const std::string&)> log_info;
    std::function<void(const std::string&)> log_error;
    std::function<void(const ChannelMessage&)> on_message_received;
    std::shared_ptr<std::atomic<bool>> abort_signal;
};

/// Legacy ChannelAdapter interface.
/// @deprecated Use ChannelPlugin + ChannelGatewayAdapter instead.
class ChannelAdapter {
public:
    virtual ~ChannelAdapter() = default;
    virtual ChannelAdapterMeta meta() const = 0;
    virtual core::Result<bool> start_account(const ChannelAdapterContext& ctx) = 0;
    virtual core::Result<bool> stop_account() = 0;
    virtual core::Result<bool> send_text(const std::string& to, const std::string& text) = 0;
    virtual core::Result<bool> send_image(const std::string& to, const std::string& image_url) = 0;
    virtual core::Result<bool> send_file(const std::string& to, const std::string& file_path) = 0;
    virtual bool supports_voice() const { return false; }
    virtual bool supports_video() const { return false; }
    virtual bool supports_group_chat() const { return false; }
    virtual bool is_connected() const = 0;
    virtual core::Result<std::string> get_status() const = 0;
};

} // namespace channels
} // namespace hunterclaw
