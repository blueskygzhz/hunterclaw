// HunterClaw - Routing System Implementation
// 严格对齐 OpenClaw routing/

#include "hunterclaw/routing/resolver.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace hunterclaw {
namespace routing {

namespace {

constexpr size_t ACCOUNT_ID_CACHE_MAX = 512;
constexpr size_t MAX_RESOLVED_ROUTE_CACHE_KEYS = 4000;

std::unordered_map<std::string, std::string> account_id_cache;
std::unordered_map<std::string, std::optional<std::string>> optional_account_id_cache;

std::regex valid_id_re("^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$");
std::regex invalid_chars_re("[^a-zA-Z0-9_-]+");

std::string to_lower(const std::string& str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(), ::tolower);
  return result;
}

std::string trim(const std::string& str) {
  size_t start = 0;
  while (start < str.size() && std::isspace(str[start])) ++start;
  size_t end = str.size();
  while (end > start && std::isspace(str[end - 1])) --end;
  return str.substr(start, end - start);
}

std::string canonicalize_account_id(const std::string& value) {
  if (std::regex_match(value, valid_id_re)) {
    return to_lower(value);
  }
  std::string result = to_lower(value);
  result = std::regex_replace(result, invalid_chars_re, "-");
  // Remove leading/trailing dashes
  size_t start = 0;
  while (start < result.size() && result[start] == '-') ++start;
  size_t end = result.size();
  while (end > start && result[end - 1] == '-') --end;
  result = result.substr(start, end - start);
  if (result.size() > 64) result.resize(64);
  return result;
}

std::optional<std::string> normalize_canonical_account_id(const std::string& value) {
  std::string canonical = canonicalize_account_id(value);
  if (canonical.empty() || canonical == "__proto__" || canonical == "constructor") {
    return std::nullopt;
  }
  return canonical;
}

template <typename T>
void set_cache(std::unordered_map<std::string, T>& cache, const std::string& key,
               const T& value, size_t max_size) {
  cache[key] = value;
  if (cache.size() > max_size) {
    cache.erase(cache.begin());
  }
}

}  // namespace

// ============================================================================
// Account ID Management
// ============================================================================

std::string normalize_account_id(const std::string& value) {
  std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return DEFAULT_ACCOUNT_ID;
  }
  auto it = account_id_cache.find(trimmed);
  if (it != account_id_cache.end()) {
    return it->second;
  }
  std::string normalized =
      normalize_canonical_account_id(trimmed).value_or(DEFAULT_ACCOUNT_ID);
  set_cache(account_id_cache, trimmed, normalized, ACCOUNT_ID_CACHE_MAX);
  return normalized;
}

std::optional<std::string> normalize_optional_account_id(const std::string& value) {
  std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  auto it = optional_account_id_cache.find(trimmed);
  if (it != optional_account_id_cache.end()) {
    return it->second;
  }
  auto normalized = normalize_canonical_account_id(trimmed);
  set_cache(optional_account_id_cache, trimmed, normalized, ACCOUNT_ID_CACHE_MAX);
  return normalized;
}

// ============================================================================
// Agent ID Management
// ============================================================================

std::string normalize_agent_id(const std::string& value) {
  std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return DEFAULT_AGENT_ID;
  }
  return to_lower(trimmed);
}

std::string sanitize_agent_id(const std::string& value) {
  std::string normalized = normalize_agent_id(value);
  if (!std::regex_match(normalized, valid_id_re)) {
    return DEFAULT_AGENT_ID;
  }
  return normalized;
}

// ============================================================================
// Session Key Management
// ============================================================================

std::optional<ParsedAgentSessionKey> parse_agent_session_key(
    const std::string& session_key) {
  std::string trimmed = trim(session_key);
  if (trimmed.empty() || trimmed.compare(0, 6, "agent:") != 0) {
    return std::nullopt;
  }
  size_t first_colon = trimmed.find(':', 6);
  if (first_colon == std::string::npos) {
    return std::nullopt;
  }
  ParsedAgentSessionKey result;
  result.agent_id = trimmed.substr(6, first_colon - 6);
  result.rest = trimmed.substr(first_colon + 1);
  return result;
}

SessionKeyShape classify_session_key_shape(const std::string& session_key) {
  std::string trimmed = trim(session_key);
  if (trimmed.empty()) {
    return SessionKeyShape::MISSING;
  }
  if (parse_agent_session_key(trimmed)) {
    return SessionKeyShape::AGENT;
  }
  if (trimmed.compare(0, 6, "agent:") == 0) {
    return SessionKeyShape::MALFORMED_AGENT;
  }
  return SessionKeyShape::LEGACY_OR_ALIAS;
}

std::string normalize_main_key(const std::string& value) {
  std::string trimmed = trim(value);
  return trimmed.empty() ? DEFAULT_MAIN_KEY : to_lower(trimmed);
}

std::optional<std::string> to_agent_request_session_key(const std::string& store_key) {
  std::string trimmed = trim(store_key);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  auto parsed = parse_agent_session_key(trimmed);
  return parsed ? std::optional<std::string>(parsed->rest) : trimmed;
}

std::string resolve_agent_id_from_session_key(const std::string& session_key) {
  auto parsed = parse_agent_session_key(session_key);
  return normalize_agent_id(parsed ? parsed->agent_id : DEFAULT_AGENT_ID);
}

std::string build_agent_main_session_key(const BuildAgentMainSessionKeyParams& params) {
  std::string agent_id = sanitize_agent_id(params.agent_id);
  std::string main_key =
      normalize_main_key(params.main_key.value_or(DEFAULT_MAIN_KEY));
  return "agent:" + agent_id + ":" + main_key;
}

std::string build_agent_peer_session_key(const BuildAgentPeerSessionKeyParams& params) {
  std::string agent_id = sanitize_agent_id(params.agent_id);
  std::string channel = to_lower(trim(params.channel));
  if (channel.empty()) channel = "unknown";

  std::string account_id =
      normalize_account_id(params.account_id.value_or(DEFAULT_ACCOUNT_ID));
  std::string peer_kind = to_lower(params.peer_kind);
  std::string peer_id = params.peer_id.value_or("");

  // DM scope determines session key format
  std::string dm_scope = to_lower(params.dm_scope);
  if (dm_scope == "main") {
    return build_agent_main_session_key({agent_id, std::nullopt});
  }

  std::ostringstream oss;
  oss << "agent:" << agent_id << ":";

  if (dm_scope == "per-peer") {
    oss << peer_kind << ":" << peer_id;
  } else if (dm_scope == "per-channel-peer") {
    oss << channel << ":" << peer_kind << ":" << peer_id;
  } else if (dm_scope == "per-account-channel-peer") {
    oss << account_id << ":" << channel << ":" << peer_kind << ":" << peer_id;
  } else {
    // Default: per-account-channel-peer
    oss << account_id << ":" << channel << ":" << peer_kind << ":" << peer_id;
  }

  return oss.str();
}

// ============================================================================
// Route Peer
// ============================================================================

std::string chat_type_to_string(ChatType type) {
  switch (type) {
    case ChatType::DIRECT:
      return "direct";
    case ChatType::GROUP:
      return "group";
    case ChatType::CHANNEL:
      return "channel";
    case ChatType::THREAD:
      return "thread";
    default:
      return "unknown";
  }
}

ChatType normalize_chat_type(const std::string& raw) {
  std::string lower = to_lower(trim(raw));
  if (lower == "direct" || lower == "dm") return ChatType::DIRECT;
  if (lower == "group") return ChatType::GROUP;
  if (lower == "channel") return ChatType::CHANNEL;
  if (lower == "thread") return ChatType::THREAD;
  return ChatType::UNKNOWN;
}

std::string route_match_source_to_string(RouteMatchSource source) {
  switch (source) {
    case RouteMatchSource::BINDING_PEER:
      return "binding.peer";
    case RouteMatchSource::BINDING_PEER_PARENT:
      return "binding.peer.parent";
    case RouteMatchSource::BINDING_GUILD_ROLES:
      return "binding.guild+roles";
    case RouteMatchSource::BINDING_GUILD:
      return "binding.guild";
    case RouteMatchSource::BINDING_TEAM:
      return "binding.team";
    case RouteMatchSource::BINDING_ACCOUNT:
      return "binding.account";
    case RouteMatchSource::BINDING_CHANNEL:
      return "binding.channel";
    default:
      return "default";
  }
}

LastRoutePolicy derive_last_route_policy(const std::string& session_key,
                                         const std::string& main_session_key) {
  return session_key == main_session_key ? LastRoutePolicy::MAIN
                                         : LastRoutePolicy::SESSION;
}

std::string resolve_inbound_last_route_session_key(const ResolvedAgentRoute& route,
                                                   const std::string& session_key) {
  return route.last_route_policy == LastRoutePolicy::MAIN ? route.main_session_key
                                                           : session_key;
}

// ============================================================================
// Route Resolver Implementation
// ============================================================================

struct RouteResolver::Impl {
  std::string default_agent_id = DEFAULT_AGENT_ID;
  std::string dm_scope = "per-account-channel-peer";
  std::vector<AgentRouteBinding> bindings;
  std::set<std::string> registered_agents;
  std::unordered_map<std::string, ResolvedAgentRoute> route_cache;
};

RouteResolver::RouteResolver() : impl_(std::make_unique<Impl>()) {}
RouteResolver::~RouteResolver() = default;

void RouteResolver::set_default_agent_id(const std::string& agent_id) {
  impl_->default_agent_id = sanitize_agent_id(agent_id);
}

void RouteResolver::set_dm_scope(const std::string& scope) {
  impl_->dm_scope = to_lower(scope);
}

void RouteResolver::add_binding(const AgentRouteBinding& binding) {
  impl_->bindings.push_back(binding);
  // Sort by priority (higher first)
  std::sort(impl_->bindings.begin(), impl_->bindings.end(),
            [](const auto& a, const auto& b) { return a.priority > b.priority; });
  impl_->route_cache.clear();
}

void RouteResolver::clear_bindings() {
  impl_->bindings.clear();
  impl_->route_cache.clear();
}

void RouteResolver::register_agent(const std::string& agent_id) {
  impl_->registered_agents.insert(sanitize_agent_id(agent_id));
}

std::string RouteResolver::pick_first_existing_agent_id(
    const std::string& agent_id) const {
  std::string normalized = sanitize_agent_id(agent_id);
  if (impl_->registered_agents.empty()) {
    return normalized;
  }
  if (impl_->registered_agents.count(normalized)) {
    return normalized;
  }
  return impl_->default_agent_id;
}

ResolvedAgentRoute RouteResolver::resolve_route(const ResolveAgentRouteInput& input) {
  // Build cache key
  std::ostringstream key_builder;
  key_builder << input.channel << "|";
  if (input.account_id) key_builder << *input.account_id;
  key_builder << "|";
  if (input.peer) {
    key_builder << chat_type_to_string(input.peer->kind) << ":" << input.peer->id;
  }

  std::string cache_key = key_builder.str();
  auto it = impl_->route_cache.find(cache_key);
  if (it != impl_->route_cache.end()) {
    return it->second;
  }

  // Default route
  ResolvedAgentRoute route;
  route.agent_id = impl_->default_agent_id;
  route.channel = to_lower(input.channel);
  route.account_id = normalize_account_id(input.account_id.value_or(""));
  route.matched_by = RouteMatchSource::DEFAULT;

  // Try to match bindings
  std::string normalized_channel = to_lower(input.channel);
  std::string normalized_account = route.account_id;

  for (const auto& binding : impl_->bindings) {
    // Check channel
    if (binding.match.channel &&
        to_lower(*binding.match.channel) != normalized_channel) {
      continue;
    }

    // Check account
    if (binding.match.account_id) {
      std::string bind_account = normalize_account_id(*binding.match.account_id);
      if (bind_account != "*" && bind_account != normalized_account) {
        continue;
      }
    }

    // Check peer
    if (binding.match.peer && input.peer) {
      if (binding.match.peer->kind != input.peer->kind ||
          (binding.match.peer->id != "*" &&
           binding.match.peer->id != input.peer->id)) {
        continue;
      }
      route.matched_by = RouteMatchSource::BINDING_PEER;
    } else if (binding.match.guild_id && input.guild_id &&
               *binding.match.guild_id == *input.guild_id) {
      route.matched_by = RouteMatchSource::BINDING_GUILD;
    } else if (binding.match.team_id && input.team_id &&
               *binding.match.team_id == *input.team_id) {
      route.matched_by = RouteMatchSource::BINDING_TEAM;
    } else if (binding.match.account_id) {
      route.matched_by = RouteMatchSource::BINDING_ACCOUNT;
    } else if (binding.match.channel) {
      route.matched_by = RouteMatchSource::BINDING_CHANNEL;
    }

    // Match found
    route.agent_id = sanitize_agent_id(binding.agent_id);
    break;
  }

  // Build session keys
  route.main_session_key = build_agent_main_session_key({route.agent_id, std::nullopt});

  if (input.peer) {
    BuildAgentPeerSessionKeyParams peer_params;
    peer_params.agent_id = route.agent_id;
    peer_params.channel = route.channel;
    peer_params.account_id = route.account_id;
    peer_params.peer_kind = chat_type_to_string(input.peer->kind);
    peer_params.peer_id = input.peer->id;
    peer_params.dm_scope = impl_->dm_scope;
    route.session_key = build_agent_peer_session_key(peer_params);
  } else {
    route.session_key = route.main_session_key;
  }

  route.last_route_policy =
      derive_last_route_policy(route.session_key, route.main_session_key);

  // Cache result
  if (impl_->route_cache.size() < MAX_RESOLVED_ROUTE_CACHE_KEYS) {
    impl_->route_cache[cache_key] = route;
  }

  return route;
}

std::vector<std::string> RouteResolver::list_bound_account_ids(
    const std::string& channel_id) const {
  std::set<std::string> ids;
  std::string normalized_channel = to_lower(channel_id);

  for (const auto& binding : impl_->bindings) {
    if (!binding.match.channel ||
        to_lower(*binding.match.channel) != normalized_channel) {
      continue;
    }
    if (binding.match.account_id) {
      std::string account = normalize_account_id(*binding.match.account_id);
      if (account != "*") {
        ids.insert(account);
      }
    }
  }

  return std::vector<std::string>(ids.begin(), ids.end());
}

std::optional<std::string> RouteResolver::resolve_default_agent_bound_account_id(
    const std::string& channel_id) const {
  std::string normalized_channel = to_lower(channel_id);
  std::string default_agent = sanitize_agent_id(impl_->default_agent_id);

  for (const auto& binding : impl_->bindings) {
    if (!binding.match.channel ||
        to_lower(*binding.match.channel) != normalized_channel) {
      continue;
    }
    if (sanitize_agent_id(binding.agent_id) != default_agent) {
      continue;
    }
    if (binding.match.account_id) {
      std::string account = normalize_account_id(*binding.match.account_id);
      if (account != "*") {
        return account;
      }
    }
  }

  return std::nullopt;
}

// ============================================================================
// Bindings Registry
// ============================================================================

struct BindingsRegistry::Impl {
  std::unordered_map<std::string, std::vector<AgentRouteBinding>> by_channel;
};

BindingsRegistry::BindingsRegistry() : impl_(std::make_unique<Impl>()) {}
BindingsRegistry::~BindingsRegistry() = default;

void BindingsRegistry::add(const AgentRouteBinding& binding) {
  if (binding.match.channel) {
    std::string channel = to_lower(*binding.match.channel);
    impl_->by_channel[channel].push_back(binding);
  }
}

void BindingsRegistry::remove_by_channel(const std::string& channel) {
  impl_->by_channel.erase(to_lower(channel));
}

void BindingsRegistry::clear() { impl_->by_channel.clear(); }

std::vector<AgentRouteBinding> BindingsRegistry::list_by_channel(
    const std::string& channel) const {
  auto it = impl_->by_channel.find(to_lower(channel));
  return it != impl_->by_channel.end() ? it->second
                                       : std::vector<AgentRouteBinding>();
}

std::vector<AgentRouteBinding> BindingsRegistry::list_all() const {
  std::vector<AgentRouteBinding> all;
  for (const auto& [_, bindings] : impl_->by_channel) {
    all.insert(all.end(), bindings.begin(), bindings.end());
  }
  return all;
}

std::optional<std::string> BindingsRegistry::find_agent_for_account(
    const std::string& channel, const std::string& account_id) const {
  auto bindings = list_by_channel(channel);
  std::string normalized_account = normalize_account_id(account_id);

  for (const auto& binding : bindings) {
    if (binding.match.account_id &&
        normalize_account_id(*binding.match.account_id) == normalized_account) {
      return sanitize_agent_id(binding.agent_id);
    }
  }

  return std::nullopt;
}

// ============================================================================
// Account Lookup
// ============================================================================

struct AccountLookup::Impl {
  std::unordered_map<std::string, std::set<std::string>> accounts_by_channel;
};

AccountLookup::AccountLookup() : impl_(std::make_unique<Impl>()) {}
AccountLookup::~AccountLookup() = default;

void AccountLookup::add_account(const std::string& channel,
                                const std::string& account_id) {
  impl_->accounts_by_channel[to_lower(channel)].insert(
      normalize_account_id(account_id));
}

std::vector<std::string> AccountLookup::list_accounts(
    const std::string& channel) const {
  auto it = impl_->accounts_by_channel.find(to_lower(channel));
  if (it == impl_->accounts_by_channel.end()) {
    return {};
  }
  return std::vector<std::string>(it->second.begin(), it->second.end());
}

bool AccountLookup::has_account(const std::string& channel,
                                const std::string& account_id) const {
  auto it = impl_->accounts_by_channel.find(to_lower(channel));
  if (it == impl_->accounts_by_channel.end()) {
    return false;
  }
  return it->second.count(normalize_account_id(account_id)) > 0;
}

void AccountLookup::clear() { impl_->accounts_by_channel.clear(); }

}  // namespace routing
}  // namespace hunterclaw
