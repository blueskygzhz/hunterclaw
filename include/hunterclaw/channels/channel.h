#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/plugins/plugin.h"
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Channel Types
// ============================================================================

enum class ChannelCapability {
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

struct ChannelMeta {
    std::string id;          // telegram, discord, slack
    std::string name;
    std::string description;
    std::vector<ChannelCapability> capabilities;
};

// ============================================================================
// Channel Account
// ============================================================================

struct ChannelAccount {
    std::string account_id;  // default, or specific account
    std::string channel_id;
    
    // Credentials
    std::string token;
    std::map<std::string, std::string> credentials;
    
    // Configuration
    std::map<std::string, core::JsonValue> config;
};

// ============================================================================
// Channel Message
// ============================================================================

enum class MessageType {
    Text,
    Image,
    File,
    Voice,
    Video,
    Sticker,
    Location,
    Contact,
    System
};

struct ChannelMessage {
    std::string id;
    std::string channel_id;
    std::string account_id;
    
    std::string from;          // Sender ID
    std::string sender_name;   // Sender display name (optional)
    std::string to;            // Recipient ID (user/group/channel)
    
    MessageType type;
    std::string text;
    std::vector<std::string> attachments;  // URLs or file paths
    
    core::Timestamp timestamp;
    std::map<std::string, core::JsonValue> metadata;
};

// ============================================================================
// Channel Context
// ============================================================================

struct ChannelContext {
    std::shared_ptr<core::Config> config;
    ChannelAccount account;
    
    std::string workspace_dir;
    std::string plugin_dir;
    
    // Callbacks
    std::function<void(const std::string& log)> log_info;
    std::function<void(const std::string& log)> log_error;
    
    // Message delivery (to core)
    std::function<void(const ChannelMessage& msg)> on_message_received;
    
    // Abort signal (for graceful shutdown)
    std::shared_ptr<std::atomic<bool>> abort_signal;
};

// ============================================================================
// Channel Adapter Interface
// ============================================================================

class ChannelAdapter {
public:
    virtual ~ChannelAdapter() = default;
    
    // Metadata
    virtual ChannelMeta meta() const = 0;
    
    // Lifecycle
    virtual core::Result<bool> start_account(const ChannelContext& ctx) = 0;
    virtual core::Result<bool> stop_account() = 0;
    
    // Message sending (outbound)
    virtual core::Result<bool> send_text(
        const std::string& to,
        const std::string& text
    ) = 0;
    
    virtual core::Result<bool> send_image(
        const std::string& to,
        const std::string& image_url
    ) = 0;
    
    virtual core::Result<bool> send_file(
        const std::string& to,
        const std::string& file_path
    ) = 0;
    
    // Optional capabilities
    virtual bool supports_voice() const { return false; }
    virtual bool supports_video() const { return false; }
    virtual bool supports_group_chat() const { return false; }
    
    // Health check
    virtual bool is_connected() const = 0;
    virtual core::Result<std::string> get_status() const = 0;
};

// Forward declarations for classes defined in manager.h
class ChannelManager;
class ChannelHealthMonitor;

// ============================================================================
// Channel Plugin Helper (for plugin developers)
// ============================================================================

class ChannelPlugin : public plugins::Plugin {
public:
    // Plugin interface
    core::Result<bool> init(const plugins::PluginContext& ctx) override;
    core::Result<bool> shutdown() override;
    core::Result<bool> on_load() override;
    core::Result<bool> on_unload() override;
    
    // Channel-specific
    virtual std::shared_ptr<ChannelAdapter> create_adapter() = 0;
    
protected:
    std::shared_ptr<ChannelAdapter> adapter_;
    std::shared_ptr<ChannelManager> channel_manager_;
};

} // namespace channels
} // namespace hunterclaw
