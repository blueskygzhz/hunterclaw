#include "hunterclaw/routing/router.h"
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace routing {

// ============================================================================
// Utility Functions
// ============================================================================

static std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

static std::string trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

static std::string chat_type_to_string(ChatType type) {
    switch (type) {
        case ChatType::Direct: return "direct";
        case ChatType::Group: return "group";
        case ChatType::Channel: return "channel";
        default: return "unknown";
    }
}

// ============================================================================
// DefaultMessageRouter Implementation
// ============================================================================

MessageRouter::Ptr MessageRouter::create(const std::string& default_agent_id) {
    return std::make_shared<DefaultMessageRouter>(default_agent_id);
}

DefaultMessageRouter::DefaultMessageRouter(const std::string& default_agent_id)
    : default_agent_id_(normalize_agent_id(default_agent_id))
{
}

std::string DefaultMessageRouter::normalize_agent_id(const std::string& agent_id) const {
    std::string normalized = trim(agent_id);
    if (normalized.empty()) {
        return "default";
    }
    // 移除非法字符，保留 a-z, 0-9, -, _
    std::string result;
    for (char c : normalized) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            result += std::tolower(c);
        }
    }
    return result.empty() ? "default" : result;
}

std::string DefaultMessageRouter::normalize_account_id(
    const std::optional<std::string>& account_id
) const {
    if (!account_id || trim(*account_id).empty()) {
        return DEFAULT_ACCOUNT_ID;
    }
    std::string normalized = trim(*account_id);
    return normalized == "*" ? DEFAULT_ACCOUNT_ID : normalized;
}

// 路由优先级匹配 (对齐 OpenClaw)
std::optional<ChannelBinding> DefaultMessageRouter::find_matching_binding(
    const RouteInput& input
) const {
    // Priority 1: Exact peer match
    if (input.peer) {
        for (const auto& binding : bindings_) {
            if (binding.peer && 
                binding.peer->kind == input.peer->kind &&
                binding.peer->id == input.peer->id) {
                return binding;
            }
        }
    }
    
    // Priority 2: Parent peer match (thread inheritance)
    if (input.parent_peer) {
        for (const auto& binding : bindings_) {
            if (binding.peer &&
                binding.peer->kind == input.parent_peer->kind &&
                binding.peer->id == input.parent_peer->id) {
                return binding;
            }
        }
    }
    
    // Priority 3: Guild + roles match (Discord)
    if (input.guild_id && !input.member_role_ids.empty()) {
        for (const auto& binding : bindings_) {
            if (binding.guild_id && *binding.guild_id == *input.guild_id &&
                !binding.require_roles.empty()) {
                // Check if user has required roles
                bool has_all_roles = true;
                for (const auto& required_role : binding.require_roles) {
                    auto it = std::find(input.member_role_ids.begin(),
                                       input.member_role_ids.end(),
                                       required_role);
                    if (it == input.member_role_ids.end()) {
                        has_all_roles = false;
                        break;
                    }
                }
                if (has_all_roles) {
                    return binding;
                }
            }
        }
    }
    
    // Priority 4: Guild match
    if (input.guild_id) {
        for (const auto& binding : bindings_) {
            if (binding.guild_id && *binding.guild_id == *input.guild_id) {
                return binding;
            }
        }
    }
    
    // Priority 5: Team match (Slack)
    if (input.team_id) {
        for (const auto& binding : bindings_) {
            if (binding.team_id && *binding.team_id == *input.team_id) {
                return binding;
            }
        }
    }
    
    // Priority 6: Account match
    if (input.account_id) {
        for (const auto& binding : bindings_) {
            if (binding.account_id && *binding.account_id == *input.account_id) {
                return binding;
            }
        }
    }
    
    // Priority 7: Channel wildcard match
    for (const auto& binding : bindings_) {
        if (binding.channel && *binding.channel == input.channel) {
            return binding;
        }
    }
    
    // No match found
    return std::nullopt;
}

core::Result<ResolvedRoute> DefaultMessageRouter::resolve_route(const RouteInput& input) {
    ResolvedRoute route;
    route.channel = input.channel;
    route.account_id = normalize_account_id(input.account_id);
    
    // Try to find matching binding
    auto binding = find_matching_binding(input);
    
    if (binding) {
        route.agent_id = normalize_agent_id(binding->agent_id);
        
        // Determine match type
        if (input.peer && binding->peer &&
            binding->peer->kind == input.peer->kind &&
            binding->peer->id == input.peer->id) {
            route.matched_by = ResolvedRoute::MatchType::BindingPeer;
        } else if (input.parent_peer && binding->peer &&
                   binding->peer->kind == input.parent_peer->kind &&
                   binding->peer->id == input.parent_peer->id) {
            route.matched_by = ResolvedRoute::MatchType::BindingPeerParent;
        } else if (binding->guild_id && !binding->require_roles.empty()) {
            route.matched_by = ResolvedRoute::MatchType::BindingGuildRoles;
        } else if (binding->guild_id) {
            route.matched_by = ResolvedRoute::MatchType::BindingGuild;
        } else if (binding->team_id) {
            route.matched_by = ResolvedRoute::MatchType::BindingTeam;
        } else if (binding->account_id) {
            route.matched_by = ResolvedRoute::MatchType::BindingAccount;
        } else {
            route.matched_by = ResolvedRoute::MatchType::BindingChannel;
        }
    } else {
        // Use default agent
        route.agent_id = default_agent_id_;
        route.matched_by = ResolvedRoute::MatchType::Default;
    }
    
    // Build session keys (默认使用 PerChannelPeer 作用域)
    route.session_key = build_session_key(
        route.agent_id,
        route.channel,
        route.account_id,
        input.peer,
        DmScope::PerChannelPeer
    );
    
    // Main session key (DM collapse)
    route.main_session_key = "agent:" + route.agent_id + ":main";
    
    return core::Result<ResolvedRoute>::Ok(route);
}

std::string DefaultMessageRouter::build_peer_session_key(
    const std::string& agent_id,
    const std::string& channel,
    const std::string& account_id,
    const std::optional<RoutePeer>& peer,
    DmScope dm_scope
) const {
    std::ostringstream oss;
    oss << "agent:" << agent_id;
    
    if (!peer) {
        // No peer, use main key
        oss << ":" << DEFAULT_MAIN_KEY;
        return oss.str();
    }
    
    // Apply DM scope logic
    if (peer->kind == ChatType::Direct) {
        switch (dm_scope) {
            case DmScope::Main:
                // All DMs share main session (UNSAFE for multi-user)
                oss << ":" << DEFAULT_MAIN_KEY;
                break;
            
            case DmScope::PerPeer:
                // Isolate by sender
                oss << ":direct:" << peer->id;
                break;
            
            case DmScope::PerChannelPeer:
                // Isolate by channel + sender (RECOMMENDED)
                oss << ":" << channel << ":direct:" << peer->id;
                break;
            
            case DmScope::PerAccountChannelPeer:
                // Full isolation
                oss << ":" << account_id << ":" << channel << ":direct:" << peer->id;
                break;
        }
    } else {
        // Group/Channel: always include full path
        oss << ":" << channel << ":" 
            << chat_type_to_string(peer->kind) << ":" << peer->id;
    }
    
    return oss.str();
}

std::string DefaultMessageRouter::build_session_key(
    const std::string& agent_id,
    const std::string& channel,
    const std::optional<std::string>& account_id,
    const std::optional<RoutePeer>& peer,
    DmScope dm_scope
) {
    std::string normalized_account = normalize_account_id(account_id);
    return build_peer_session_key(
        normalize_agent_id(agent_id),
        channel,
        normalized_account,
        peer,
        dm_scope
    );
}

void DefaultMessageRouter::add_binding(const ChannelBinding& binding) {
    // Remove existing binding for same agent
    remove_binding(binding.agent_id);
    bindings_.push_back(binding);
}

void DefaultMessageRouter::remove_binding(const std::string& agent_id) {
    std::string normalized = normalize_agent_id(agent_id);
    bindings_.erase(
        std::remove_if(bindings_.begin(), bindings_.end(),
            [&](const ChannelBinding& b) {
                return normalize_agent_id(b.agent_id) == normalized;
            }),
        bindings_.end()
    );
}

} // namespace routing
} // namespace hunterclaw
