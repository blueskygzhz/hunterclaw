#ifndef HUNTERCLAW_ROUTING_ROUTER_H
#define HUNTERCLAW_ROUTING_ROUTER_H

#include <string>
#include <memory>
#include <vector>
#include <optional>
#include "hunterclaw/core/result.h"

namespace hunterclaw {
namespace routing {

// 对齐 OpenClaw ChatType
enum class ChatType {
    Direct,    // 1-on-1 DM
    Group,     // 群聊
    Channel    // 频道/主题
};

// Peer 标识 (对齐 OpenClaw RoutePeer)
struct RoutePeer {
    ChatType kind;
    std::string id;
};

// 路由输入 (对齐 OpenClaw ResolveAgentRouteInput)
struct RouteInput {
    std::string channel;                          // 频道名称 (e.g., "feishu", "slack")
    std::optional<std::string> account_id;        // 账号ID
    std::optional<RoutePeer> peer;                // 对话ID
    std::optional<RoutePeer> parent_peer;         // 父对话(线程继承)
    std::optional<std::string> guild_id;          // Discord服务器ID
    std::optional<std::string> team_id;           // Slack团队ID
    std::vector<std::string> member_role_ids;     // Discord角色ID
};

// 路由结果 (对齐 OpenClaw ResolvedAgentRoute)
struct ResolvedRoute {
    std::string agent_id;           // Agent ID
    std::string channel;            // 频道名称
    std::string account_id;         // 账号ID (已规范化)
    std::string session_key;        // 会话持久化key
    std::string main_session_key;   // 主会话key (DM collapse)
    
    // 匹配方式 (调试用)
    enum class MatchType {
        BindingPeer,         // 精确匹配对话ID
        BindingPeerParent,   // 线程继承父对话
        BindingGuildRoles,   // Discord服务器+角色
        BindingGuild,        // Discord服务器
        BindingTeam,         // Slack团队
        BindingAccount,      // 账号匹配
        BindingChannel,      // 频道通配
        Default              // 默认Agent
    };
    MatchType matched_by;
};

// DM 会话作用域 (对齐 OpenClaw session.dmScope)
enum class DmScope {
    Main,                    // 所有DM共享主会话 (默认,不安全)
    PerPeer,                 // 按发送者隔离
    PerChannelPeer,          // 按频道+发送者隔离 (推荐)
    PerAccountChannelPeer    // 按账号+频道+发送者隔离
};

// Binding 配置 (对齐 OpenClaw bindings)
struct ChannelBinding {
    std::string agent_id;                         // 目标Agent
    std::optional<std::string> channel;           // 频道过滤
    std::optional<std::string> account_id;        // 账号过滤
    std::optional<RoutePeer> peer;                // 对话过滤
    std::optional<std::string> guild_id;          // Discord服务器
    std::vector<std::string> require_roles;       // Discord角色要求
    std::optional<std::string> team_id;           // Slack团队
};

// 消息路由器接口 (对齐 OpenClaw resolveAgentRoute)
class MessageRouter {
public:
    using Ptr = std::shared_ptr<MessageRouter>;
    
    virtual ~MessageRouter() = default;
    
    // 解析路由
    virtual core::Result<ResolvedRoute> resolve_route(const RouteInput& input) = 0;
    
    // 构建会话key (对齐 OpenClaw buildAgentSessionKey)
    virtual std::string build_session_key(
        const std::string& agent_id,
        const std::string& channel,
        const std::optional<std::string>& account_id,
        const std::optional<RoutePeer>& peer,
        DmScope dm_scope = DmScope::Main
    ) = 0;
    
    // 添加/移除 binding
    virtual void add_binding(const ChannelBinding& binding) = 0;
    virtual void remove_binding(const std::string& agent_id) = 0;
    
    // 工厂方法
    static Ptr create(const std::string& default_agent_id);
};

// 默认实现
class DefaultMessageRouter : public MessageRouter {
public:
    explicit DefaultMessageRouter(const std::string& default_agent_id);
    
    core::Result<ResolvedRoute> resolve_route(const RouteInput& input) override;
    
    std::string build_session_key(
        const std::string& agent_id,
        const std::string& channel,
        const std::optional<std::string>& account_id,
        const std::optional<RoutePeer>& peer,
        DmScope dm_scope
    ) override;
    
    void add_binding(const ChannelBinding& binding) override;
    void remove_binding(const std::string& agent_id) override;
    
private:
    std::string default_agent_id_;
    std::vector<ChannelBinding> bindings_;
    
    // 辅助方法
    std::string normalize_agent_id(const std::string& agent_id) const;
    std::string normalize_account_id(const std::optional<std::string>& account_id) const;
    std::optional<ChannelBinding> find_matching_binding(const RouteInput& input) const;
    std::string build_peer_session_key(
        const std::string& agent_id,
        const std::string& channel,
        const std::string& account_id,
        const std::optional<RoutePeer>& peer,
        DmScope dm_scope
    ) const;
};

// 常量定义
constexpr const char* DEFAULT_ACCOUNT_ID = "default";
constexpr const char* DEFAULT_MAIN_KEY = "main";

} // namespace routing
} // namespace hunterclaw

#endif // HUNTERCLAW_ROUTING_ROUTER_H
