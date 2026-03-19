#pragma once

#include "hunterclaw/channels/channel.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <memory>
#include <atomic>
#include <thread>
<mutex>
#include <set>
#include <map>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Feishu Configuration
// ============================================================================

enum class FeishuDomain {
    Feishu,      // feishu.cn (China)
    Lark,        // larksuite.com (International)
    Custom       // Custom domain
};

enum class FeishuConnectionMode {
    WebSocket,   // Long-connection mode (recommended, no public IP required)
    Webhook      // Webhook mode (requires public IP)
};

enum class FeishuDMPolicy {
    Open,        // Accept all private messages
    Pairing,     // Requires pairing
    Allowlist    // Allowlist only
};

enum class FeishuGroupPolicy {
    Open,        // Accept all group messages
    Allowlist,   // Allowlist only
    Disabled     // Disable group messages
};

struct FeishuConfig {
    bool enabled = true;
    std::string default_account = "default";
    
    // Authentication credentials
    std::string app_id;
    std::string app_secret;
    std::string encrypt_key;        // Message encryption key
    std::string verification_token; // Webhook verification token
    
    // Domain configuration
    FeishuDomain domain = FeishuDomain::Feishu;
    std::string custom_domain;  // Used when domain == Custom
    
    // Connection mode
    FeishuConnectionMode connection_mode = FeishuConnectionMode::WebSocket;
    
    // Webhook configuration (used only when connection_mode == Webhook)
    std::string webhook_path = "/feishu/webhook";
    std::string webhook_host = "0.0.0.0";
    int webhook_port = 8089;
    
    // Permission policies
    FeishuDMPolicy dm_policy = FeishuDMPolicy::Open;
    FeishuGroupPolicy group_policy = FeishuGroupPolicy::Open;
    std::vector<std::string> allow_from;  // Allowlist user/group IDs
    
    // Message processing
    bool enable_message_dedup = true;     // Message deduplication
    int dedup_window_seconds = 300;       // Dedup window (5 minutes)
    
    // Advanced options
    int max_reconnect_attempts = 10;      // Max reconnection attempts
};

// ============================================================================
// Feishu Channel Adapter
// ============================================================================

class FeishuChannelAdapter : public ChannelAdapter {
public:
    explicit FeishuChannelAdapter(const FeishuConfig& config);
    ~FeishuChannelAdapter() override;
    
    // ChannelAdapter interface
    ChannelAdapterMeta meta() const override;
    
    core::Result<bool> start_account(const ChannelAdapterContext& ctx) override;
    core::Result<bool> stop_account() override;
    
    core::Result<bool> send_text(
        const std::string& to,
        const std::string& text
    ) override;
    
    core::Result<bool> send_image(
        const std::string& to,
        const std::string& image_url
    ) override;
    
    core::Result<bool> send_file(
        const std::string& to,
        const std::string& file_path
    ) override;
    
    bool supports_voice() const override { return true; }
    bool supports_video() const override { return true; }
    bool supports_group_chat() const override { return true; }
    
    bool is_connected() const override;
    core::Result<std::string> get_status() const override;
    
private:
    // WebSocket mode implementation
    void run_websocket_mode();
    
    // Webhook mode implementation
    void run_webhook_mode();
    
    // Event handlers
    void handle_message_received(const core::JsonValue& event_data);
    
    // Message deduplication
    bool is_duplicate_message(const std::string& message_id);
    
    // Permission checks
    bool check_permission(const std::string& chat_id, const std::string& sender_id);
    bool is_in_allowlist(const std::string& id);
    
    // HTTP client
    core::Result<core::JsonValue> get_tenant_access_token();
    
    std::string resolve_domain() const;
    std::string get_api_base_url() const;
    
    FeishuConfig config_;
    ChannelAdapterContext context_;
    
    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    
    // Worker thread
    std::thread worker_thread_;
    
    // Authentication
    mutable std::mutex token_mutex_;
    std::string tenant_access_token_;
    int64_t token_expire_time_ = 0;
    
    // Message deduplication
    mutable std::mutex dedup_mutex_;
    std::set<std::string> seen_message_ids_;
};

} // namespace channels
} // namespace hunterclaw
