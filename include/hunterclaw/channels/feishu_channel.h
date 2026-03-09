#pragma once

#include "hunterclaw/channels/channel.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <set>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Feishu Configuration (100% 对齐 OpenClaw)
// ============================================================================

enum class FeishuDomain {
    Feishu,      // feishu.cn (国内)
    Lark,        // larksuite.com (海外)
    Custom       // 自定义域名
};

enum class FeishuConnectionMode {
    WebSocket,   // 长连接模式 (推荐,无需公网IP)
    Webhook      // Webhook模式 (需要公网IP)
};

enum class FeishuDMPolicy {
    Open,        // 接受所有私聊
    Pairing,     // 需要配对
    Allowlist    // 仅允许白名单
};

enum class FeishuGroupPolicy {
    Open,        // 接受所有群聊
    Allowlist,   // 仅允许白名单
    Disabled     // 禁用群聊
};

struct FeishuConfig {
    bool enabled = true;
    std::string default_account = "default";
    
    // 认证凭据
    std::string app_id;
    std::string app_secret;
    std::string encrypt_key;        // 消息加密密钥
    std::string verification_token; // Webhook 验证令牌
    
    // 域名配置
    FeishuDomain domain = FeishuDomain::Feishu;
    std::string custom_domain;  // 当 domain == Custom 时使用
    
    // 连接模式
    FeishuConnectionMode connection_mode = FeishuConnectionMode::WebSocket;
    
    // Webhook 配置 (仅当 connection_mode == Webhook 时使用)
    std::string webhook_path = "/feishu/webhook";
    std::string webhook_host = "0.0.0.0";
    int webhook_port = 8089;
    
    // 权限策略
    FeishuDMPolicy dm_policy = FeishuDMPolicy::Open;
    FeishuGroupPolicy group_policy = FeishuGroupPolicy::Open;
    std::vector<std::string> allow_from;  // 白名单用户/群组ID
    
    // 消息处理
    bool enable_message_dedup = true;     // 消息去重
    int dedup_window_seconds = 300;       // 去重窗口(5分钟)
    bool enable_inbound_debounce = true;  // 消息防抖合并
    int debounce_delay_ms = 500;          // 防抖延迟
    
    // 高级选项
    bool enable_broadcast = false;        // 广播模式
    int max_reconnect_attempts = 10;      // 最大重连次数
    
    // 多账号支持
    std::map<std::string, FeishuConfig> accounts;
};

// ============================================================================
// Feishu Message Event (100% 对齐 OpenClaw)
// ============================================================================

enum class FeishuMessageType {
    Text,
    Image,
    File,
    Audio,
    Video,
    Post,            // 富文本消息
    MergeForward,    // 合并转发
    Interactive,     // 交互式卡片
    Unknown
};

struct FeishuMessage {
    std::string message_id;
    std::string root_id;         // 会话根消息ID
    std::string parent_id;       // 父消息ID (回复时)
    std::string chat_id;         // 会话ID
    std::string chat_type;       // p2p, group
    
    FeishuMessageType message_type;
    std::string content;         // 消息内容 (JSON字符串)
    
    // 发送者信息
    std::string sender_id;
    std::string sender_name;
    std::string sender_type;     // user, app, system
    
    // 提及信息
    bool mentions_bot = false;
    std::vector<std::string> mentioned_user_ids;
    
    // 时间戳
    int64_t create_time = 0;     // 毫秒时间戳
    
    // 附件
    std::vector<std::string> media_keys;     // 媒体文件key
    std::vector<std::string> file_keys;      // 文件key
    
    // 元数据
    core::JsonValue metadata;
};

// ============================================================================
// Feishu Card Action Event (交互式卡片动作)
// ============================================================================

struct FeishuCardAction {
    std::string message_id;
    std::string open_message_id;
    std::string chat_id;
    std::string action_value;    // 按钮 value
    
    // 操作者信息
    std::string operator_id;
    std::string operator_name;
    
    // 卡片信息
    core::JsonValue card_data;
    
    int64_t action_time = 0;
};

// ============================================================================
// Feishu Channel Adapter (100% 对齐 OpenClaw)
// ============================================================================

class FeishuChannelAdapter : public ChannelAdapter {
public:
    explicit FeishuChannelAdapter(const FeishuConfig& config);
    ~FeishuChannelAdapter() override;
    
    // ChannelAdapter interface
    ChannelMeta meta() const override;
    
    core::Result<bool> start_account(const ChannelContext& ctx) override;
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
    
    // Feishu-specific capabilities
    core::Result<bool> send_post(
        const std::string& to,
        const core::JsonValue& post_content
    );
    
    core::Result<bool> send_card(
        const std::string& to,
        const core::JsonValue& card_content
    );
    
    core::Result<bool> reply_message(
        const std::string& message_id,
        const std::string& text
    );
    
    core::Result<bool> edit_message(
        const std::string& message_id,
        const std::string& text
    );
    
    core::Result<bool> add_reaction(
        const std::string& message_id,
        const std::string& emoji_type
    );
    
    // 媒体处理
    core::Result<std::string> download_media(
        const std::string& file_key,
        const std::string& save_path
    );
    
    core::Result<std::string> upload_image(
        const std::string& image_path
    );
    
    core::Result<std::string> upload_file(
        const std::string& file_path
    );
    
    // 信息查询
    core::Result<std::string> get_user_name(const std::string& user_id);
    core::Result<core::JsonValue> get_message(const std::string& message_id);
    
    bool supports_voice() const override { return true; }
    bool supports_video() const override { return true; }
    bool supports_group_chat() const override { return true; }
    
    bool is_connected() const override;
    core::Result<std::string> get_status() const override;
    
private:
    // WebSocket 模式实现
    void run_websocket_mode();
    void handle_websocket_event(const core::JsonValue& event);
    
    // Webhook 模式实现
    void run_webhook_mode();
    void handle_webhook_request(const std::string& body);
    
    // 事件处理器
    void handle_message_received(const core::JsonValue& event_data);
    void handle_message_read(const core::JsonValue& event_data);
    void handle_bot_added_to_chat(const core::JsonValue& event_data);
    void handle_bot_removed_from_chat(const core::JsonValue& event_data);
    void handle_reaction_created(const core::JsonValue& event_data);
    void handle_card_action(const core::JsonValue& event_data);
    
    // 消息处理
    core::Result<FeishuMessage> parse_message_event(const core::JsonValue& event_data);
    bool check_bot_mentioned(const FeishuMessage& msg);
    core::Result<std::vector<std::string>> resolve_media_list(const FeishuMessage& msg);
    
    // 消息去重
    bool is_duplicate_message(const std::string& message_id);
    void mark_message_seen(const std::string& message_id);
    
    // 消息防抖
    void enqueue_inbound_message(const FeishuMessage& msg);
    void flush_debounced_messages();
    
    // 权限检查
    bool check_permission(const std::string& chat_id, const std::string& sender_id);
    bool is_in_allowlist(const std::string& id);
    
    // HTTP 客户端
    core::Result<core::JsonValue> get_tenant_access_token();
    core::Result<core::JsonValue> http_get(const std::string& endpoint);
    core::Result<core::JsonValue> http_post(const std::string& endpoint, const core::JsonValue& body);
    
    std::string resolve_domain() const;
    std::string get_api_base_url() const;
    
    FeishuConfig config_;
    ChannelContext context_;
    
    // 连接状态
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    
    // 工作线程
    std::thread worker_thread_;
    std::thread debounce_thread_;
    
    // 认证
    mutable std::mutex token_mutex_;
    std::string tenant_access_token_;
    int64_t token_expire_time_ = 0;
    
    // 消息去重
    mutable std::mutex dedup_mutex_;
    std::set<std::string> seen_message_ids_;
    std::map<std::string, int64_t> message_timestamps_;  // 用于清理旧记录
    
    // 消息防抖
    mutable std::mutex debounce_mutex_;
    std::map<std::string, std::vector<FeishuMessage>> debounce_buffer_;  // chat_id -> messages
    
    // 用户名缓存
    mutable std::mutex user_cache_mutex_;
    std::map<std::string, std::string> user_name_cache_;
    
    // Webhook server (仅 Webhook 模式) - 暂未实现
    // struct WebhookServer;
    // std::unique_ptr<WebhookServer> webhook_server_;
};

// ============================================================================
// Feishu Channel Plugin
// ============================================================================

class FeishuChannelPlugin : public ChannelPlugin {
public:
    explicit FeishuChannelPlugin(const FeishuConfig& config);
    
    // Plugin interface
    std::string id() const override;
    std::string name() const override;
    std::string version() const override;
    
    // ChannelPlugin interface
    std::shared_ptr<ChannelAdapter> create_adapter() override;
    
private:
    FeishuConfig config_;
};

} // namespace channels
} // namespace hunterclaw
