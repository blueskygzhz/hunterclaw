// ============================================================================
// router.cc
// ✅ 薄包装层，将旧 MessageRouter 接口委托给 resolver.cc 的规范实现
// ============================================================================

#include "hunterclaw/routing/router.h"
#include "hunterclaw/routing/resolver.h"
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace routing {

namespace {

// router.h 中的 ChatType（通过 resolver.h 的 ChatType::DIRECT 等）
// 和 resolver.h 是同一个枚举，直接使用，不需要转换

// DmScope → string
static std::string dm_scope_to_string(DmScope scope) {
    switch (scope) {
        case DmScope::Main:               return "main";
        case DmScope::PerPeer:            return "per-peer";
        case DmScope::PerChannelPeer:     return "per-channel-peer";
        case DmScope::PerAccountChannelPeer: return "per-account-channel-peer";
        default:                          return "main";
    }
}

} // namespace

// ============================================================================
// MessageRouter factory
// ============================================================================

MessageRouter::Ptr MessageRouter::create(const std::string& default_agent_id) {
    return std::make_shared<DefaultMessageRouter>(default_agent_id);
}

DefaultMessageRouter::DefaultMessageRouter(const std::string& default_agent_id)
    : default_agent_id_(::hunterclaw::routing::normalize_agent_id(default_agent_id))
{
}

// ============================================================================
// resolve_route — 委托给 RouteResolver
// ============================================================================

core::Result<ResolvedRoute> DefaultMessageRouter::resolve_route(const RouteInput& input) {
    // 构建 RouteResolver（每次调用时按需构建，或考虑缓存）
    ::hunterclaw::routing::RouteResolver resolver;
    resolver.set_default_agent_id(default_agent_id_);
    resolver.set_dm_scope("main");  // DefaultMessageRouter 使用 "main" scope

    // 注入 bindings
    for (const auto& b : bindings_) {
        ::hunterclaw::routing::AgentRouteBinding rb;
        rb.agent_id = b.agent_id;
        if (b.channel) rb.match.channel = *b.channel;
        if (b.account_id) rb.match.account_id = *b.account_id;
        if (b.peer) {
            rb.match.peer = ::hunterclaw::routing::RoutePeer{
                b.peer->kind,   // ✅ ChatType 是同一枚举
                b.peer->id
            };
        }
        if (b.guild_id) rb.match.guild_id = *b.guild_id;
        rb.match.roles = b.require_roles;
        if (b.team_id) rb.match.team_id = *b.team_id;
        resolver.add_binding(rb);
    }

    // 构建输入
    ::hunterclaw::routing::ResolveAgentRouteInput ri;
    ri.channel = input.channel;
    ri.account_id = input.account_id;
    if (input.peer) {
        ri.peer = ::hunterclaw::routing::RoutePeer{
            input.peer->kind, input.peer->id
        };
    }
    if (input.parent_peer) {
        ri.parent_peer = ::hunterclaw::routing::RoutePeer{
            input.parent_peer->kind, input.parent_peer->id
        };
    }
    ri.guild_id = input.guild_id;
    ri.team_id  = input.team_id;
    ri.member_role_ids = input.member_role_ids;

    const auto rr = resolver.resolve_route(ri);

    // 映射回 ResolvedRoute
    ResolvedRoute route;
    route.agent_id        = rr.agent_id;
    route.channel         = rr.channel;
    route.account_id      = rr.account_id;
    route.session_key     = rr.session_key;
    route.main_session_key= rr.main_session_key;

    switch (rr.matched_by) {
        case ::hunterclaw::routing::RouteMatchSource::BINDING_PEER:
            route.matched_by = ResolvedRoute::MatchType::BindingPeer; break;
        case ::hunterclaw::routing::RouteMatchSource::BINDING_PEER_PARENT:
            route.matched_by = ResolvedRoute::MatchType::BindingPeerParent; break;
        case ::hunterclaw::routing::RouteMatchSource::BINDING_GUILD_ROLES:
            route.matched_by = ResolvedRoute::MatchType::BindingGuildRoles; break;
        case ::hunterclaw::routing::RouteMatchSource::BINDING_GUILD:
            route.matched_by = ResolvedRoute::MatchType::BindingGuild; break;
        case ::hunterclaw::routing::RouteMatchSource::BINDING_TEAM:
            route.matched_by = ResolvedRoute::MatchType::BindingTeam; break;
        case ::hunterclaw::routing::RouteMatchSource::BINDING_ACCOUNT:
            route.matched_by = ResolvedRoute::MatchType::BindingAccount; break;
        case ::hunterclaw::routing::RouteMatchSource::BINDING_CHANNEL:
            route.matched_by = ResolvedRoute::MatchType::BindingChannel; break;
        default:
            route.matched_by = ResolvedRoute::MatchType::Default; break;
    }

    return core::Result<ResolvedRoute>::Ok(route);
}

// ============================================================================
// build_session_key — 委托给 build_agent_peer_session_key
// ============================================================================

std::string DefaultMessageRouter::build_session_key(
    const std::string& agent_id,
    const std::string& channel,
    const std::optional<std::string>& account_id,
    const std::optional<RoutePeer>& peer,
    DmScope dm_scope
) const {
    ::hunterclaw::routing::BuildAgentPeerSessionKeyParams p;
    p.agent_id   = agent_id;
    p.channel    = channel;
    p.account_id = account_id;
    if (peer) {
        p.peer_kind = [&]{
            switch (peer->kind) {
                case ChatType::DIRECT:  return std::string("direct");
                case ChatType::GROUP:   return std::string("group");
                case ChatType::CHANNEL: return std::string("channel");
                case ChatType::THREAD:  return std::string("thread");
                default:                return std::string("unknown");
            }
        }();
        p.peer_id = peer->id;
    }
    p.dm_scope = dm_scope_to_string(dm_scope);
    return ::hunterclaw::routing::build_agent_peer_session_key(p);
}

// ============================================================================
// add_binding / remove_binding
// ============================================================================

void DefaultMessageRouter::add_binding(const ChannelBinding& binding) {
    remove_binding(binding.agent_id);
    bindings_.push_back(binding);
}

void DefaultMessageRouter::remove_binding(const std::string& agent_id) {
    const std::string normalized = ::hunterclaw::routing::normalize_agent_id(agent_id);
    bindings_.erase(
        std::remove_if(bindings_.begin(), bindings_.end(),
            [&](const ChannelBinding& b) {
                return ::hunterclaw::routing::normalize_agent_id(b.agent_id) == normalized;
            }),
        bindings_.end()
    );
}

std::string DefaultMessageRouter::normalize_agent_id(const std::string& agent_id) const {
    return ::hunterclaw::routing::normalize_agent_id(agent_id);
}

std::string DefaultMessageRouter::normalize_account_id(
    const std::optional<std::string>& account_id) const {
    return ::hunterclaw::routing::normalize_account_id(account_id.value_or(""));
}

std::optional<ChannelBinding> DefaultMessageRouter::find_matching_binding(
    const RouteInput& /*input*/) const {
    // 已被 resolve_route 委托给 RouteResolver，不再需要
    return std::nullopt;
}

std::string DefaultMessageRouter::build_peer_session_key(
    const std::string& agent_id,
    const std::string& channel,
    const std::string& account_id,
    const std::optional<RoutePeer>& peer,
    DmScope dm_scope) const {
    return build_session_key(agent_id, channel,
                             std::optional<std::string>(account_id),
                             peer, dm_scope);
}

} // namespace routing
} // namespace hunterclaw
