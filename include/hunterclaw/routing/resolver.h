// HunterClaw - Routing System
// 严格对齐 OpenClaw routing/
// Purpose: Agent route resolution, session key management, account ID normalization

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
// ============================================================================

constexpr const char* DEFAULT_ACCOUNT_ID = "default";

// Normalize account ID to canonical form
std::string normalize_account_id(const std::string& value);
std::optional<std::string> normalize_optional_account_id(const std::string& value);

// ============================================================================
// Agent ID Management
// ============================================================================

constexpr const char* DEFAULT_AGENT_ID = "main";
constexpr const char* DEFAULT_MAIN_KEY = "main";

std::string normalize_agent_id(const std::string& value);
std::string sanitize_agent_id(const std::string& value);

// ============================================================================
// Session Key Management
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

std::optional<ParsedAgentSessionKey> parse_agent_session_key(const std::string& session_key);
SessionKeyShape classify_session_key_shape(const std::string& session_key);

std::string normalize_main_key(const std::string& value);
std::optional<std::string> to_agent_request_session_key(const std::string& store_key);
std::string resolve_agent_id_from_session_key(const std::string& session_key);

struct BuildAgentMainSessionKeyParams {
  std::string agent_id;
  std::optional<std::string> main_key;
};

std::string build_agent_main_session_key(const BuildAgentMainSessionKeyParams& params);

struct BuildAgentPeerSessionKeyParams {
  std::string agent_id;
  std::string channel;
  std::optional<std::string> account_id;
  std::string peer_kind;  // "direct", "group", "channel", etc.
  std::optional<std::string> peer_id;
  std::string dm_scope;  // "main", "per-peer", "per-channel-peer", "per-account-channel-peer"
};

std::string build_agent_peer_session_key(const BuildAgentPeerSessionKeyParams& params);

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
// ============================================================================

enum class RouteMatchSource {
  BINDING_PEER,
  BINDING_PEER_PARENT,
  BINDING_GUILD_ROLES,
  BINDING_GUILD,
  BINDING_TEAM,
  BINDING_ACCOUNT,
  BINDING_CHANNEL,
  DEFAULT
};

std::string route_match_source_to_string(RouteMatchSource source);

enum class LastRoutePolicy {
  MAIN,
  SESSION
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

LastRoutePolicy derive_last_route_policy(const std::string& session_key,
                                         const std::string& main_session_key);

std::string resolve_inbound_last_route_session_key(const ResolvedAgentRoute& route,
                                                   const std::string& session_key);

// ============================================================================
// Route Resolution Input
// ============================================================================

struct ResolveAgentRouteInput {
  std::string channel;
  std::optional<std::string> account_id;
  std::optional<RoutePeer> peer;
  std::optional<RoutePeer> parent_peer;
  std::optional<std::string> guild_id;
  std::optional<std::string> team_id;
  std::vector<std::string> member_role_ids;
};

// ============================================================================
// Route Resolver
// ============================================================================

class RouteResolver {
 public:
  RouteResolver();
  ~RouteResolver();

  // Configuration
  void set_default_agent_id(const std::string& agent_id);
  void set_dm_scope(const std::string& scope);
  void add_binding(const AgentRouteBinding& binding);
  void clear_bindings();

  // Route resolution
  ResolvedAgentRoute resolve_route(const ResolveAgentRouteInput& input);

  // Binding queries
  std::vector<std::string> list_bound_account_ids(const std::string& channel_id) const;
  std::optional<std::string> resolve_default_agent_bound_account_id(
      const std::string& channel_id) const;

  // Agent lookup
  std::string pick_first_existing_agent_id(const std::string& agent_id) const;
  void register_agent(const std::string& agent_id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Bindings Registry
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

  // Query helpers
  std::optional<std::string> find_agent_for_account(const std::string& channel,
                                                    const std::string& account_id) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Account Lookup
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

}  // namespace routing
}  // namespace hunterclaw
