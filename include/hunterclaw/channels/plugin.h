#pragma once

#include "hunterclaw/channels/channel.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>

namespace hunterclaw {
namespace channels {

/// Channel 元数据 (100% 对齐 OpenClaw)
struct ChannelMeta {
    ChannelId id;
    std::string label;              // "Telegram"
    std::string selection_label;    // "Telegram (Bot API)"
    std::string detail_label;       // "Telegram Bot"
    std::string docs_path;          // "/channels/telegram"
    std::string docs_label;
    std::string blurb;              // 简短描述
    std::string system_image;       // 系统图标名
    std::vector<std::string> aliases;
};

/// DM 策略 (100% 对齐 OpenClaw/QuantClaw)
enum class DmPolicy {
    Open,    // 接受所有 DM (通过 allow_from 过滤)
    Pairing  // 需要配对码
};

/// DM 会话范围 (100% 对齐 OpenClaw/QuantClaw)
enum class DmScope {
    Main,                    // 所有 DM 共享主会话
    PerPeer,                 // 每个发送者独立会话
    PerChannelPeer,          // 每个 channel+发送者独立会话
    PerAccountChannelPeer    // 每个 account+channel+发送者独立会话
};

/// 群组激活策略 (100% 对齐 OpenClaw/QuantClaw)
enum class GroupActivation {
    Mention,  // 仅在 @提及 时响应
    Always    // 响应所有群消息
};

/// Channel 策略配置 (100% 对齐 OpenClaw/QuantClaw)
struct ChannelPolicyConfig {
    DmPolicy dm_policy = DmPolicy::Open;
    DmScope dm_scope = DmScope::PerChannelPeer;
    GroupActivation group_activation = GroupActivation::Mention;
    
    std::vector<std::string> allow_from;
    int group_chunk_size = 2000;
    std::string bot_name;
    
    nlohmann::json to_json() const;
    static core::Result<ChannelPolicyConfig> from_json(const nlohmann::json& j);
};

/// Channel 上下文 (运行时环境)
struct ChannelContext {
    core::Config cfg;
    std::string account_id;
    nlohmann::json account;  // 账户特定配置
    
    std::map<std::string, std::string> runtime_env;  // 环境变量
    std::shared_ptr<std::atomic<bool>> abort_signal;  // 取消信号
    
    std::shared_ptr<spdlog::logger> log;
    
    std::function<ChannelAccountSnapshot()> get_status;
    std::function<void(const ChannelAccountSnapshot&)> set_status;
};

/// Channel 配置适配器 (100% 对齐 OpenClaw)
class ChannelConfigAdapter {
public:
    virtual ~ChannelConfigAdapter() = default;
    
    // 列出所有账户 ID
    virtual std::vector<std::string> list_account_ids(const core::Config& cfg) const = 0;
    
    // 解析特定账户配置
    virtual nlohmann::json resolve_account(const core::Config& cfg, const std::string& account_id) const = 0;
    
    // 检查账户是否启用
    virtual bool is_enabled(const nlohmann::json& account, const core::Config& cfg) const {
        return account.value("enabled", true);
    }
    
    // 检查账户是否已配置
    virtual core::Result<bool> is_configured(const nlohmann::json& account, const core::Config& cfg) const {
        return core::Result<bool>::ok(true);
    }
};

/// Channel Gateway 适配器 (入站和启动,100% 对齐 OpenClaw)
class ChannelGatewayAdapter {
public:
    virtual ~ChannelGatewayAdapter() = default;
    
    // 启动账户 (长期运行,监听入站消息)
    virtual core::Result<void> start_account(const ChannelContext& ctx) = 0;
    
    // 停止账户 (通过 ctx.abort_signal 信号)
};

/// Channel 状态适配器 (100% 对齐 OpenClaw)
class ChannelStatusAdapter {
public:
    virtual ~ChannelStatusAdapter() = default;
    
    // 获取连接状态描述
    virtual std::string get_status(const ChannelContext& ctx) const = 0;
    
    // 检查是否已连接
    virtual bool is_connected(const ChannelContext& ctx) const = 0;
};

/// Channel 出站适配器 (发送消息,100% 对齐 OpenClaw)
class ChannelOutboundAdapter {
public:
    virtual ~ChannelOutboundAdapter() = default;
    
    struct SendOptions {
        core::Config cfg;
        std::string account_id;
        std::string target;  // user ID, group ID, etc.
        std::string body;
        std::vector<core::Attachment> attachments;
        nlohmann::json metadata;
    };
    
    struct SendResult {
        bool ok;
        std::string message_id;
        std::string error;
    };
    
    // 发送消息
    virtual core::Result<SendResult> send(const SendOptions& opts) = 0;
};

/// Channel 目录适配器 (联系人和群组,100% 对齐 OpenClaw)
class ChannelDirectoryAdapter {
public:
    virtual ~ChannelDirectoryAdapter() = default;
    
    struct Contact {
        std::string id;
        std::string name;
        std::string type;  // "user" | "group" | "channel"
        nlohmann::json metadata;
    };
    
    // 列出联系人/群组
    virtual core::Result<std::vector<Contact>> list_contacts(const ChannelContext& ctx) const = 0;
    
    // 搜索联系人
    virtual core::Result<std::vector<Contact>> search_contacts(
        const ChannelContext& ctx,
        const std::string& query
    ) const = 0;
};

/// Channel 配对适配器 (设备配对,100% 对齐 OpenClaw)
class ChannelPairingAdapter {
public:
    virtual ~ChannelPairingAdapter() = default;
    
    struct PairingInfo {
        std::string code;
        std::string qr_code_url;
        std::string qr_code_data;  // base64 encoded image
        int64_t expires_at;
    };
    
    // 生成配对码
    virtual core::Result<PairingInfo> generate_pairing(const ChannelContext& ctx) = 0;
    
    // 等待配对完成
    virtual core::Result<bool> wait_for_pairing(
        const ChannelContext& ctx,
        int64_t timeout_ms = 60000
    ) = 0;
};

/// Channel 安全适配器 (加密和验证,100% 对齐 OpenClaw)
class ChannelSecurityAdapter {
public:
    virtual ~ChannelSecurityAdapter() = default;
    
    // 验证发件人
    virtual bool verify_sender(const ChannelContext& ctx, const std::string& sender_id) const = 0;
    
    // 加密消息
    virtual core::Result<std::string> encrypt_message(
        const ChannelContext& ctx,
        const std::string& plaintext
    ) const = 0;
    
    // 解密消息
    virtual core::Result<std::string> decrypt_message(
        const ChannelContext& ctx,
        const std::string& ciphertext
    ) const = 0;
};

/// Channel 插件 (完整定义,100% 对齐 OpenClaw)
struct ChannelPlugin {
    ChannelId id;
    ChannelMeta meta;
    
    // 必需适配器
    std::shared_ptr<ChannelConfigAdapter> config;
    
    // 可选适配器
    std::shared_ptr<ChannelGatewayAdapter> gateway;
    std::shared_ptr<ChannelStatusAdapter> status;
    std::shared_ptr<ChannelOutboundAdapter> outbound;
    std::shared_ptr<ChannelDirectoryAdapter> directory;
    std::shared_ptr<ChannelPairingAdapter> pairing;
    std::shared_ptr<ChannelSecurityAdapter> security;
    
    // Gateway 方法 (RPC)
    std::vector<std::string> gateway_methods;
};

/// Channel 插件注册表 (100% 对齐 OpenClaw)
class ChannelPluginRegistry {
public:
    static ChannelPluginRegistry& instance();
    
    // 注册插件
    void register_plugin(const ChannelPlugin& plugin, int priority = 0);
    void unregister_plugin(ChannelId channel_id);
    
    // 查询插件
    std::optional<ChannelPlugin> get_plugin(ChannelId channel_id) const;
    std::vector<ChannelPlugin> list_plugins() const;
    
    // 按优先级排序
    std::vector<ChannelPlugin> list_plugins_sorted() const;
    
private:
    ChannelPluginRegistry() = default;
    
    struct PluginEntry {
        ChannelPlugin plugin;
        int priority;
    };
    
    mutable std::mutex mutex_;
    std::map<ChannelId, PluginEntry> plugins_;
};

/// Channel 注册宏 (便捷)
#define REGISTER_CHANNEL_PLUGIN(plugin, priority) \
    static struct ChannelPluginRegistrar_##plugin { \
        ChannelPluginRegistrar_##plugin() { \
            ::hunterclaw::channels::ChannelPluginRegistry::instance().register_plugin(plugin, priority); \
        } \
    } _registrar_##plugin;

} // namespace channels
} // namespace hunterclaw
