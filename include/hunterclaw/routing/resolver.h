// HunterClaw - Routing System
// ✅ 严格对齐 OpenClaw routing/
//   src/routing/account-id.ts
//   src/routing/session-key.ts
//   src/routing/resolve-route.ts

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hunterclaw {
namespace routing {

// ============================================================================
// Account ID Management
// ✅ 对齐 OpenClaw: account-id.ts
// ============================================================================

constexpr const char* DEFAULT_ACCOUNT_ID = "default";

// ✅ 对齐 OpenClaw: normalizeAccountId
std::string normalize_account_id(const std::string& value);
// ✅ 对齐 OpenClaw: normalizeOptionalAccountId
std::optional<std::string> normalize_optional_account_id(const std::string& value);

// ============================================================================
// Agent ID Management
// ✅ 对齐 OpenClaw: session-key.ts:89-116
// ============================================================================

constexpr const char* DEFAULT_AGENT_ID = "main";
constexpr const char* DEFAULT_MAIN_KEY = "main";

// ✅ 对齐 OpenClaw: normalizeAgentId
std::string normalize_agent_id(const std::string& value);
// ✅ 对齐 OpenClaw: isValidAgentId (新增)
bool is_valid_agent_id(const std::string& value);
// ✅ 对齐 OpenClaw: sanitizeAgentId = normalizeAgentId
std::string sanitize_agent_id(const std::string& value);

// ============================================================================
// Session Key Management
// ✅ 对齐 OpenClaw: session-key.ts
// ============================================================================

enum class SessionKeyShape {
  MISSING,
  AGENT,
  LEGACY_OR_ALIAS,
  MALFORMED_AGENT
};

struct ParsedAgentSessionKey {
  std::string agent_id;
  std::string rest;
};

// ✅ 对齐 OpenClaw: parseAgentSessionKey
std::optional<ParsedAgentSessionKey> parse_agent_session_key(const std::string& session_key);
// ✅ 对齐 OpenClaw: classifySessionKeyShape
SessionKeyShape classify_session_key_shape(const std::string& session_key);

// ✅ 对齐 OpenClaw: normalizeMainKey
std::string normalize_main_key(const std::string& value);
// ✅ 对齐 OpenClaw: toAgentRequestSessionKey
std::optional<std::string> to_agent_request_session_key(const std::string& store_key);
// ✅ 对齐 OpenClaw: toAgentStoreSessionKey (新增)
std::string to_agent_store_session_key(const std::string& agent_id,
                                       const std::string& request_key,
                                       const std::string& main_key = "");
// ✅ 对齐 OpenClaw: resolveAgentIdFromSessionKey
std::string resolve_agent_id_from_session_key(const std::string& session_key);

struct BuildAgentMainSessionKeyParams {
  std::string agent_id;
  std::optional<std::string> main_key;
};

// ✅ 对齐 OpenClaw: buildAgentMainSessionKey
std::string build_agent_main_session_key(const BuildAgentMainSessionKeyParams& params);

struct BuildAgentPeerSessionKeyParams {
  std::string agent_id;
  std::optional<std::string> main_key;       // ✅ 新增（对齐 OpenClaw mainKey 参数）
  std::string channel;
  std::optional<std::string> account_id;
  std::string peer_kind;                     // "direct", "group", "channel", "thread", ...
  std::optional<std::string> peer_id;
  std::string dm_scope;                      // "main"(默认), "per-peer", "per-channel-peer", "per-account-channel-peer"
  // identity_links 由调用方预处理后通过 peer_id 传入
};

// ✅ 对齐 OpenClaw: buildAgentPeerSessionKey
std::string build_agent_peer_session_key(const BuildAgentPeerSessionKeyParams& params);

// ✅ 对齐 OpenClaw: buildGroupHistoryKey (新增)
struct BuildGroupHistoryKeyParams {
  std::string channel;
  std::optional<std::string> account_id;
  std::string peer_kind;  // "group" | "channel"
  std::string peer_id;
};

std::string build_group_history_key(const BuildGroupHistoryKeyParams& params);

// ✅ 对齐 OpenClaw: resolveThreadSessionKeys (新增)
struct ResolveThreadSessionKeysParams {
  std::string base_session_key;
  std::optional<std::string> thread_id;
  std::optional<std::string> parent_session_key;
  std::optional<bool> use_suffix;  // 默认 true
};

struct ResolvedThreadSessionKeys {
  std::string session_key;
  std::optional<std::string> parent_session_key;
};

ResolvedThreadSessionKeys resolve_thread_session_keys(
    const ResolveThreadSessionKeysParams& params);

// ============================================================================
// Route Peer
// ============================================================================

enum class ChatType {
  DIRECT,
  GROUP,
  CHANNEL,
  THREAD,
  UNKNOWN
};

std::string chat_type_to_string(ChatType type);
ChatType normalize_chat_type(const std::string& raw);

struct RoutePeer {
  ChatType kind;
  std::string id;
};

// ============================================================================
// Agent Route Binding
// ============================================================================

struct BindingMatchConstraint {
  std::optional<std::string> channel;
  std::optional<std::string> account_id;
  std::optional<RoutePeer> peer;
  std::optional<std::string> guild_id;
  std::optional<std::string> team_id;
  std::vector<std::string> roles;
};

struct AgentRouteBinding {
  std::string agent_id;
  BindingMatchConstraint match;
  int priority = 0;
};

// ============================================================================
// Resolved Agent Route
// ✅ 对齐 OpenClaw: ResolvedAgentRoute (resolve-route.ts:39-59)
// ============================================================================

// ✅ 对齐 OpenClaw: matchedBy 字符串
enum class RouteMatchSource {
  BINDING_PEER,         // "binding.peer"
  BINDING_PEER_PARENT,  // "binding.peer.parent"
  BINDING_GUILD_ROLES,  // "binding.guild+roles"
  BINDING_GUILD,        // "binding.guild"
  BINDING_TEAM,         // "binding.team"
  BINDING_ACCOUNT,      // "binding.account"
  BINDING_CHANNEL,      // "binding.channel"
  DEFAULT               // "default"
};

std::string route_match_source_to_string(RouteMatchSource source);

enum class LastRoutePolicy {
  MAIN,    // "main"
  SESSION  // "session"
};

struct ResolvedAgentRoute {
  std::string agent_id;
  std::string channel;
  std::string account_id;
  std::string session_key;
  std::string main_session_key;
  LastRoutePolicy last_route_policy;
  RouteMatchSource matched_by;
};

// ✅ 对齐 OpenClaw: deriveLastRoutePolicy
LastRoutePolicy derive_last_route_policy(const std::string& session_key,
                                         const std::string& main_session_key);
// ✅ 对齐 OpenClaw: resolveInboundLastRouteSessionKey
std::string resolve_inbound_last_route_session_key(const ResolvedAgentRoute& route,
                                                   const std::string& session_key);

// ============================================================================
// Route Resolution Input
// ✅ 对齐 OpenClaw: ResolveAgentRouteInput (resolve-route.ts:26-37)
// ============================================================================

struct ResolveAgentRouteInput {
  std::string channel;
  std::optional<std::string> account_id;
  std::optional<RoutePeer> peer;
  std::optional<RoutePeer> parent_peer;  // ✅ 对齐 OpenClaw: parentPeer
  std::optional<std::string> guild_id;
  std::optional<std::string> team_id;
  std::vector<std::string> member_role_ids;  // ✅ 对齐 OpenClaw: memberRoleIds
};

// ============================================================================
// Route Resolver
// ✅ 对齐 OpenClaw: resolveAgentRoute (resolve-route.ts:614)
// ============================================================================

class RouteResolver {
 public:
  RouteResolver();
  ~RouteResolver();

  void set_default_agent_id(const std::string& agent_id);
  // ✅ dm_scope 对齐 OpenClaw: cfg.session?.dmScope ?? "main"
  void set_dm_scope(const std::string& scope);
  void add_binding(const AgentRouteBinding& binding);
  void clear_bindings();

  // ✅ 对齐 OpenClaw: resolveAgentRoute — 7 层分层匹配
  ResolvedAgentRoute resolve_route(const ResolveAgentRouteInput& input);

  // ✅ 对齐 OpenClaw: listBoundAccountIds (bindings.ts:48)
  std::vector<std::string> list_bound_account_ids(const std::string& channel_id) const;
  // ✅ 对齐 OpenClaw: resolveDefaultAgentBoundAccountId (bindings.ts:64)
  std::optional<std::string> resolve_default_agent_bound_account_id(
      const std::string& channel_id) const;

  // ✅ 对齐 OpenClaw: pickFirstExistingAgentId (resolve-route.ts:151)
  std::string pick_first_existing_agent_id(const std::string& agent_id) const;
  void register_agent(const std::string& agent_id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ============================================================================
// BindingsRegistry
// ============================================================================

class BindingsRegistry {
 public:
  BindingsRegistry();
  ~BindingsRegistry();

  void add(const AgentRouteBinding& binding);
  void remove_by_channel(const std::string& channel);
  void clear();

  std::vector<AgentRouteBinding> list_by_channel(const std::string& channel) const;
  std::vector<AgentRouteBinding> list_all() const;

  std::optional<std::string> find_agent_for_account(const std::string& channel,
                                                    const std::string& account_id) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ============================================================================
// AccountLookup
// ============================================================================

class AccountLookup {
 public:
  AccountLookup();
  ~AccountLookup();

  void add_account(const std::string& channel, const std::string& account_id);
  std::vector<std::string> list_accounts(const std::string& channel) const;
  bool has_account(const std::string& channel, const std::string& account_id) const;
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Session Key Utilities (补齐)
// ✅ 对齐 OpenClaw: session-key-utils.ts getSubagentDepth
// ============================================================================

// 🎯 对齐 OpenClaw: getSubagentDepth(sessionKey)
// 返回 session key 中 sub: 嵌套深度 (0 = 非子代理)
int get_subagent_depth(const std::string& session_key);

// 🎯 对齐 OpenClaw: scopedHeartbeatWakeOptions(sessionKey, wakeOptions)
// 如果 sessionKey 是 agent: 格式，返回包含 sessionKey 的 options
bool should_scope_heartbeat_wake(const std::string& session_key);

// ============================================================================
// Resolve Preferred Account ID
// ✅ 对齐 OpenClaw: bindings.ts resolvePreferredAccountId
// ============================================================================

struct ResolvePreferredAccountIdParams {
  std::vector<std::string> account_ids;
  std::string default_account_id;
  std::vector<std::string> bound_accounts;
};

// 🎯 对齐 OpenClaw: resolvePreferredAccountId
// 优先绑定账户，回退到默认
std::string resolve_preferred_account_id(const ResolvePreferredAccountIdParams& params);

// ============================================================================
// Default Account Warnings
// ✅ 对齐 OpenClaw: routing/default-account-warnings.ts
// ============================================================================

// 🎯 对齐 OpenClaw: formatChannelDefaultAccountPath
std::string format_channel_default_account_path(const std::string& channel_key);

// 🎯 对齐 OpenClaw: formatChannelAccountsDefaultPath
std::string format_channel_accounts_default_path(const std::string& channel_key);

// 🎯 对齐 OpenClaw: formatSetExplicitDefaultInstruction
std::string format_set_explicit_default_instruction(const std::string& channel_key);

// 🎯 对齐 OpenClaw: formatSetExplicitDefaultToConfiguredInstruction
std::string format_set_explicit_default_to_configured_instruction(const std::string& channel_key);

// ============================================================================
// Channel Route Resolution Helpers
// ✅ 对齐 OpenClaw 各渠道 route-resolution 封装
// ============================================================================

// 🎯 对齐 OpenClaw: discord/monitor/route-resolution.ts buildDiscordRoutePeer
RoutePeer build_discord_route_peer(bool is_direct_message, bool is_group_dm,
                                   const std::string& direct_user_id,
                                   const std::string& conversation_id);

// 🎯 对齐 OpenClaw: resolveDiscordConversationRoute
ResolvedAgentRoute resolve_discord_conversation_route(
    RouteResolver& resolver,
    const std::optional<std::string>& account_id,
    const std::optional<std::string>& guild_id,
    const std::vector<std::string>& member_role_ids,
    const RoutePeer& peer,
    const std::optional<std::string>& parent_conversation_id = std::nullopt);

// 🎯 对齐 OpenClaw: resolveDiscordBoundConversationRoute
// bound_session_key 覆盖路由的 sessionKey
ResolvedAgentRoute resolve_discord_bound_conversation_route(
    RouteResolver& resolver,
    const std::optional<std::string>& account_id,
    const std::optional<std::string>& guild_id,
    const std::vector<std::string>& member_role_ids,
    bool is_direct_message,
    bool is_group_dm,
    const std::string& direct_user_id,
    const std::string& conversation_id,
    const std::optional<std::string>& parent_conversation_id = std::nullopt,
    const std::optional<std::string>& bound_session_key = std::nullopt);

// 🎯 对齐 OpenClaw: 通用渠道路由解析（Telegram/Slack/Signal/LINE 等）
// 简化封装，指定 channel 名称后调用 resolveAgentRoute
ResolvedAgentRoute resolve_channel_route(
    RouteResolver& resolver,
    const std::string& channel_name,  // "telegram", "slack", "signal", "line", etc.
    const std::optional<std::string>& account_id,
    const RoutePeer& peer,
    const std::optional<std::string>& team_id = std::nullopt);

}  // namespace routing
}  // namespace hunterclaw
