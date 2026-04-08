// ============================================================================
// resolver.cc
// ✅ 严格对齐 OpenClaw:
//   src/routing/account-id.ts
//   src/routing/session-key.ts
//   src/routing/resolve-route.ts
//   src/routing/bindings.ts
// ============================================================================

#include "hunterclaw/routing/resolver.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace hunterclaw {
namespace routing {

namespace {

// ── 正则（与 OpenClaw 完全一致）────────────────────────────────────────────────
// ✅ OpenClaw: /^[a-z0-9][a-z0-9_-]{0,63}$/i
const std::regex VALID_ID_RE("^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$");
// ✅ OpenClaw: /[^a-z0-9_-]+/g
const std::regex INVALID_CHARS_RE("[^a-zA-Z0-9_-]+");

// 缓存大小
constexpr size_t ACCOUNT_ID_CACHE_MAX = 512;
constexpr size_t MAX_RESOLVED_ROUTE_CACHE_KEYS = 4000;

// ── 基础工具函数 ────────────────────────────────────────────────────────────────

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

// ✅ 对齐 OpenClaw: normalizeToken — trim + toLowerCase
std::string normalize_token(const std::string& value) {
  return to_lower(trim(value));
}

// ── isBlockedObjectKey ──────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: isBlockedObjectKey (infra/prototype-keys.ts)
// 防止原型链污染
bool is_blocked_object_key(const std::string& key) {
  return key == "__proto__" || key == "constructor" || key == "prototype";
}

// ── canonicalizeAccountId ─────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: canonicalizeAccountId (account-id.ts:14-24)
std::string canonicalize_account_id(const std::string& value) {
  if (std::regex_match(value, VALID_ID_RE)) {
    return to_lower(value);
  }
  std::string result = to_lower(value);
  // ✅ replace → strip leading/trailing dash → slice(0, 64)
  result = std::regex_replace(result, INVALID_CHARS_RE, "-");
  size_t start = 0;
  while (start < result.size() && result[start] == '-') ++start;
  size_t end = result.size();
  while (end > start && result[end - 1] == '-') --end;
  result = result.substr(start, end - start);
  if (result.size() > 64) result.resize(64);
  return result;
}

// ✅ 对齐 OpenClaw: normalizeCanonicalAccountId (account-id.ts:26-32)
std::optional<std::string> normalize_canonical_account_id(const std::string& value) {
  const std::string canonical = canonicalize_account_id(value);
  if (canonical.empty() || is_blocked_object_key(canonical)) {
    return std::nullopt;
  }
  return canonical;
}

// ── 缓存 ────────────────────────────────────────────────────────────────────────

std::unordered_map<std::string, std::string> account_id_cache;
std::unordered_map<std::string, std::optional<std::string>> optional_account_id_cache;

template <typename T>
void set_cache(std::unordered_map<std::string, T>& cache, const std::string& key,
               const T& value, size_t max_size) {
  cache[key] = value;
  if (cache.size() > max_size) {
    // ✅ 对齐 OpenClaw: delete oldest entry
    cache.erase(cache.begin());
  }
}

}  // namespace

// ============================================================================
// Account ID Management
// ✅ 对齐 OpenClaw: account-id.ts
// ============================================================================

std::string normalize_account_id(const std::string& value) {
  const std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return DEFAULT_ACCOUNT_ID;
  }
  auto it = account_id_cache.find(trimmed);
  if (it != account_id_cache.end()) {
    return it->second;
  }
  // ✅ 对齐 OpenClaw: normalizeCanonicalAccountId(trimmed) || DEFAULT_ACCOUNT_ID
  const std::string normalized =
      normalize_canonical_account_id(trimmed).value_or(DEFAULT_ACCOUNT_ID);
  set_cache(account_id_cache, trimmed, normalized, ACCOUNT_ID_CACHE_MAX);
  return normalized;
}

std::optional<std::string> normalize_optional_account_id(const std::string& value) {
  const std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  auto it = optional_account_id_cache.find(trimmed);
  if (it != optional_account_id_cache.end()) {
    return it->second;
  }
  // ✅ 对齐 OpenClaw: normalizeCanonicalAccountId(trimmed) || undefined
  const auto normalized = normalize_canonical_account_id(trimmed);
  set_cache(optional_account_id_cache, trimmed, normalized, ACCOUNT_ID_CACHE_MAX);
  return normalized;
}

// ============================================================================
// Agent ID Management
// ✅ 对齐 OpenClaw: session-key.ts:89-116
// ============================================================================

std::string normalize_agent_id(const std::string& value) {
  const std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return DEFAULT_AGENT_ID;
  }
  // ✅ 对齐 OpenClaw: if VALID_ID_RE.test → toLowerCase; else collapse invalid → "-"
  if (std::regex_match(trimmed, VALID_ID_RE)) {
    return to_lower(trimmed);
  }
  std::string result = to_lower(trimmed);
  result = std::regex_replace(result, INVALID_CHARS_RE, "-");
  size_t start = 0;
  while (start < result.size() && result[start] == '-') ++start;
  size_t end = result.size();
  while (end > start && result[end - 1] == '-') --end;
  result = result.substr(start, end - start);
  if (result.size() > 64) result.resize(64);
  return result.empty() ? DEFAULT_AGENT_ID : result;
}

bool is_valid_agent_id(const std::string& value) {
  const std::string trimmed = trim(value);
  return !trimmed.empty() && std::regex_match(trimmed, VALID_ID_RE);
}

// ✅ 对齐 OpenClaw: sanitizeAgentId = normalizeAgentId (session-key.ts:114-116)
std::string sanitize_agent_id(const std::string& value) {
  return normalize_agent_id(value);
}

// ============================================================================
// Session Key Management
// ✅ 对齐 OpenClaw: session-key.ts
// ============================================================================

std::optional<ParsedAgentSessionKey> parse_agent_session_key(const std::string& session_key) {
  const std::string trimmed = trim(session_key);
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
  const std::string raw = trim(session_key);
  if (raw.empty()) return SessionKeyShape::MISSING;
  if (parse_agent_session_key(raw)) return SessionKeyShape::AGENT;
  if (to_lower(raw).compare(0, 6, "agent:") == 0) return SessionKeyShape::MALFORMED_AGENT;
  return SessionKeyShape::LEGACY_OR_ALIAS;
}

std::string normalize_main_key(const std::string& value) {
  const std::string trimmed = trim(value);
  return trimmed.empty() ? DEFAULT_MAIN_KEY : to_lower(trimmed);
}

// ✅ 对齐 OpenClaw: toAgentRequestSessionKey (session-key.ts:45-51)
std::optional<std::string> to_agent_request_session_key(const std::string& store_key) {
  const std::string raw = trim(store_key);
  if (raw.empty()) return std::nullopt;
  auto parsed = parse_agent_session_key(raw);
  if (parsed) return parsed->rest;
  return raw;
}

// ✅ 对齐 OpenClaw: toAgentStoreSessionKey (session-key.ts:53-71)
std::string to_agent_store_session_key(const std::string& agent_id,
                                       const std::string& request_key,
                                       const std::string& main_key) {
  const std::string raw = trim(request_key);
  if (raw.empty() || to_lower(raw) == DEFAULT_MAIN_KEY) {
    return build_agent_main_session_key({agent_id, main_key.empty()
                                         ? std::nullopt
                                         : std::optional<std::string>(main_key)});
  }
  auto parsed = parse_agent_session_key(raw);
  if (parsed) {
    return "agent:" + parsed->agent_id + ":" + parsed->rest;
  }
  const std::string lowered = to_lower(raw);
  if (lowered.compare(0, 6, "agent:") == 0) {
    return lowered;
  }
  return "agent:" + normalize_agent_id(agent_id) + ":" + lowered;
}

std::string resolve_agent_id_from_session_key(const std::string& session_key) {
  auto parsed = parse_agent_session_key(session_key);
  return normalize_agent_id(parsed ? parsed->agent_id : DEFAULT_AGENT_ID);
}

std::string build_agent_main_session_key(const BuildAgentMainSessionKeyParams& params) {
  const std::string agent_id = normalize_agent_id(params.agent_id);
  const std::string main_key = normalize_main_key(params.main_key.value_or(DEFAULT_MAIN_KEY));
  return "agent:" + agent_id + ":" + main_key;
}

// ✅ 严格对齐 OpenClaw: buildAgentPeerSessionKey (session-key.ts:127-173)
//
// 关键差异（相较旧实现）:
//   per-account-channel-peer 字段顺序:
//     OpenClaw: agent:{id}:{channel}:{accountId}:direct:{peerId}
//     旧HunterClaw: agent:{id}:{accountId}:{channel}:direct:{peerId}  ← 错误!
//   dm_scope 默认值: OpenClaw 默认 "main"，旧实现默认 "per-account-channel-peer"
//   sessionKey 最终 toLowerCase（在 resolveAgentRoute 的 choose 中处理）
std::string build_agent_peer_session_key(const BuildAgentPeerSessionKeyParams& params) {
  const std::string dm_scope = to_lower(params.dm_scope.empty() ? "main" : params.dm_scope);

  // ✅ 对齐 OpenClaw: peerKind 默认 "direct"
  const std::string peer_kind = to_lower(params.peer_kind.empty() ? "direct" : params.peer_kind);

  if (peer_kind == "direct") {
    std::string peer_id = to_lower(trim(params.peer_id.value_or("")));

    // ✅ 对齐 OpenClaw: identity links 解析（简化版，无 identityLinks 支持时跳过）
    // identity links 由调用方预先解析后传入 peer_id

    // ✅ 对齐 OpenClaw: per-account-channel-peer (session-key.ts:154-158)
    // agent:{agentId}:{channel}:{accountId}:direct:{peerId}
    if (dm_scope == "per-account-channel-peer" && !peer_id.empty()) {
      const std::string channel = normalize_token(params.channel).empty()
                                   ? "unknown" : normalize_token(params.channel);
      const std::string account_id = normalize_account_id(params.account_id.value_or(""));
      return "agent:" + normalize_agent_id(params.agent_id)
           + ":" + channel
           + ":" + account_id
           + ":direct:" + peer_id;
    }
    // ✅ 对齐 OpenClaw: per-channel-peer (session-key.ts:159-162)
    // agent:{agentId}:{channel}:direct:{peerId}
    if (dm_scope == "per-channel-peer" && !peer_id.empty()) {
      const std::string channel = normalize_token(params.channel).empty()
                                   ? "unknown" : normalize_token(params.channel);
      return "agent:" + normalize_agent_id(params.agent_id)
           + ":" + channel
           + ":direct:" + peer_id;
    }
    // ✅ 对齐 OpenClaw: per-peer (session-key.ts:163-165)
    // agent:{agentId}:direct:{peerId}
    if (dm_scope == "per-peer" && !peer_id.empty()) {
      return "agent:" + normalize_agent_id(params.agent_id)
           + ":direct:" + peer_id;
    }
    // ✅ 对齐 OpenClaw: "main" 和 fallback (session-key.ts:166-169)
    return build_agent_main_session_key({params.agent_id, params.main_key});
  }

  // ✅ 对齐 OpenClaw: 非 direct peerKind (session-key.ts:171-173)
  // agent:{agentId}:{channel}:{peerKind}:{peerId}
  const std::string channel = normalize_token(params.channel).empty()
                               ? "unknown" : normalize_token(params.channel);
  const std::string peer_id = to_lower(trim(params.peer_id.value_or("unknown")));
  const std::string final_peer_id = peer_id.empty() ? "unknown" : peer_id;
  return "agent:" + normalize_agent_id(params.agent_id)
       + ":" + channel
       + ":" + peer_kind
       + ":" + final_peer_id;
}

// ✅ 对齐 OpenClaw: buildGroupHistoryKey (session-key.ts:222-232)
// {channel}:{accountId}:{peerKind}:{peerId}
std::string build_group_history_key(const BuildGroupHistoryKeyParams& params) {
  const std::string channel = normalize_token(params.channel).empty()
                               ? "unknown" : normalize_token(params.channel);
  const std::string account_id = normalize_account_id(params.account_id.value_or(""));
  const std::string peer_id = to_lower(trim(params.peer_id)).empty()
                               ? "unknown" : to_lower(trim(params.peer_id));
  return channel + ":" + account_id + ":" + params.peer_kind + ":" + peer_id;
}

// ✅ 对齐 OpenClaw: resolveThreadSessionKeys (session-key.ts:234-253)
ResolvedThreadSessionKeys resolve_thread_session_keys(
    const ResolveThreadSessionKeysParams& params) {
  const std::string thread_id = trim(params.thread_id.value_or(""));
  if (thread_id.empty()) {
    return {params.base_session_key, std::nullopt};
  }
  // ✅ normalizeThreadId 默认 toLowerCase
  const std::string normalized_thread_id = to_lower(thread_id);
  const bool use_suffix = params.use_suffix.value_or(true);
  const std::string session_key = use_suffix
      ? params.base_session_key + ":thread:" + normalized_thread_id
      : params.base_session_key;
  return {session_key, params.parent_session_key};
}

// ============================================================================
// Route Peer / ChatType helpers
// ============================================================================

std::string chat_type_to_string(ChatType type) {
  switch (type) {
    case ChatType::DIRECT:  return "direct";
    case ChatType::GROUP:   return "group";
    case ChatType::CHANNEL: return "channel";
    case ChatType::THREAD:  return "thread";
    default:                return "unknown";
  }
}

ChatType normalize_chat_type(const std::string& raw) {
  const std::string lower = to_lower(trim(raw));
  if (lower == "direct" || lower == "dm") return ChatType::DIRECT;
  if (lower == "group")   return ChatType::GROUP;
  if (lower == "channel") return ChatType::CHANNEL;
  if (lower == "thread")  return ChatType::THREAD;
  return ChatType::UNKNOWN;
}

std::string route_match_source_to_string(RouteMatchSource source) {
  switch (source) {
    case RouteMatchSource::BINDING_PEER:        return "binding.peer";
    case RouteMatchSource::BINDING_PEER_PARENT: return "binding.peer.parent";
    case RouteMatchSource::BINDING_GUILD_ROLES: return "binding.guild+roles";
    case RouteMatchSource::BINDING_GUILD:       return "binding.guild";
    case RouteMatchSource::BINDING_TEAM:        return "binding.team";
    case RouteMatchSource::BINDING_ACCOUNT:     return "binding.account";
    case RouteMatchSource::BINDING_CHANNEL:     return "binding.channel";
    default:                                    return "default";
  }
}

LastRoutePolicy derive_last_route_policy(const std::string& session_key,
                                         const std::string& main_session_key) {
  return session_key == main_session_key ? LastRoutePolicy::MAIN : LastRoutePolicy::SESSION;
}

std::string resolve_inbound_last_route_session_key(const ResolvedAgentRoute& route,
                                                   const std::string& session_key) {
  return route.last_route_policy == LastRoutePolicy::MAIN
             ? route.main_session_key
             : session_key;
}

// ============================================================================
// RouteResolver Implementation
// ✅ 对齐 OpenClaw: resolveAgentRoute (resolve-route.ts:614-804)
// ============================================================================

// ── 内部类型 ──────────────────────────────────────────────────────────────────

enum class PeerConstraintState { NONE, INVALID, VALID };

struct NormalizedPeerConstraint {
  PeerConstraintState state = PeerConstraintState::NONE;
  ChatType kind = ChatType::UNKNOWN;
  std::string id;
};

struct NormalizedBindingMatch {
  std::string account_pattern;  // "" 或具体 accountId 或 "*"
  NormalizedPeerConstraint peer;
  std::string guild_id;
  std::string team_id;
  std::vector<std::string> roles;
};

struct EvaluatedBinding {
  AgentRouteBinding binding;
  NormalizedBindingMatch match;
  int order = 0;
};

// ── peer lookup keys ──────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: peerLookupKeys — group/channel 互认
static std::vector<std::string> peer_lookup_keys(ChatType kind, const std::string& id) {
  if (kind == ChatType::GROUP) {
    return {"group:" + id, "channel:" + id};
  }
  if (kind == ChatType::CHANNEL) {
    return {"channel:" + id, "group:" + id};
  }
  return {chat_type_to_string(kind) + ":" + id};
}

// ── peerKindMatches ───────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: peerKindMatches — group/channel 互认
static bool peer_kind_matches(ChatType binding_kind, ChatType scope_kind) {
  if (binding_kind == scope_kind) return true;
  return (binding_kind == ChatType::GROUP || binding_kind == ChatType::CHANNEL)
      && (scope_kind == ChatType::GROUP || scope_kind == ChatType::CHANNEL);
}

// ── normalizePeerConstraint ───────────────────────────────────────────────────
static NormalizedPeerConstraint normalize_peer_constraint(
    const std::optional<RoutePeer>& peer) {
  if (!peer) return {PeerConstraintState::NONE};
  const std::string id = trim(peer->id);
  if (peer->kind == ChatType::UNKNOWN || id.empty()) {
    return {PeerConstraintState::INVALID};
  }
  return {PeerConstraintState::VALID, peer->kind, to_lower(id)};
}

// ── normalizeBindingMatch ─────────────────────────────────────────────────────
static NormalizedBindingMatch normalize_binding_match(const BindingMatchConstraint& match) {
  NormalizedBindingMatch m;
  m.account_pattern = match.account_id.value_or("").empty()
                      ? "" : trim(*match.account_id);
  m.peer = normalize_peer_constraint(match.peer);
  m.guild_id = match.guild_id.value_or("");
  m.team_id  = match.team_id.value_or("");
  m.roles    = match.roles;
  return m;
}

// ── matchesBindingScope ────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: matchesBindingScope (resolve-route.ts:584-612)
static bool matches_binding_scope(
    const NormalizedBindingMatch& match,
    const std::optional<RoutePeer>& scope_peer,
    const std::string& scope_guild_id,
    const std::string& scope_team_id,
    const std::set<std::string>& scope_member_role_ids) {
  if (match.peer.state == PeerConstraintState::INVALID) return false;
  if (match.peer.state == PeerConstraintState::VALID) {
    if (!scope_peer) return false;
    if (!peer_kind_matches(match.peer.kind, scope_peer->kind)) return false;
    if (to_lower(trim(scope_peer->id)) != match.peer.id) return false;
  }
  if (!match.guild_id.empty() && match.guild_id != scope_guild_id) return false;
  if (!match.team_id.empty() && match.team_id != scope_team_id) return false;
  if (!match.roles.empty()) {
    for (const auto& role : match.roles) {
      if (scope_member_role_ids.count(role)) return true;
    }
    return false;
  }
  return true;
}

// ── EvaluatedBindingsIndex ────────────────────────────────────────────────────
struct EvaluatedBindingsIndex {
  std::unordered_map<std::string, std::vector<int>> by_peer;          // key→binding 下标
  std::unordered_map<std::string, std::vector<int>> by_guild_roles;
  std::unordered_map<std::string, std::vector<int>> by_guild;
  std::unordered_map<std::string, std::vector<int>> by_team;
  std::vector<int> by_account;
  std::vector<int> by_channel;
};

static EvaluatedBindingsIndex build_bindings_index(
    const std::vector<EvaluatedBinding>& bindings) {
  EvaluatedBindingsIndex idx;
  for (int i = 0; i < (int)bindings.size(); i++) {
    const auto& b = bindings[i];
    if (b.match.peer.state == PeerConstraintState::VALID) {
      for (const auto& key : peer_lookup_keys(b.match.peer.kind, b.match.peer.id)) {
        idx.by_peer[key].push_back(i);
      }
    } else if (!b.match.guild_id.empty() && !b.match.roles.empty()) {
      idx.by_guild_roles[b.match.guild_id].push_back(i);
    } else if (!b.match.guild_id.empty()) {
      idx.by_guild[b.match.guild_id].push_back(i);
    } else if (!b.match.team_id.empty()) {
      idx.by_team[b.match.team_id].push_back(i);
    } else if (b.match.account_pattern != "*" && !b.match.account_pattern.empty()) {
      idx.by_account.push_back(i);
    } else {
      idx.by_channel.push_back(i);
    }
  }
  return idx;
}

struct RouteResolver::Impl {
  std::string default_agent_id = DEFAULT_AGENT_ID;
  std::string dm_scope = "main";  // ✅ 对齐 OpenClaw: cfg.session?.dmScope ?? "main"
  std::vector<AgentRouteBinding> bindings;
  std::vector<EvaluatedBinding> evaluated;
  EvaluatedBindingsIndex index;
  std::set<std::string> registered_agents;
  std::unordered_map<std::string, ResolvedAgentRoute> route_cache;

  void rebuild_index() {
    evaluated.clear();
    int order = 0;
    for (const auto& b : bindings) {
      EvaluatedBinding eb;
      eb.binding = b;
      eb.match = normalize_binding_match(b.match);
      eb.order = order++;
      evaluated.push_back(eb);
    }
    index = build_bindings_index(evaluated);
    route_cache.clear();
  }
};

RouteResolver::RouteResolver() : impl_(std::make_unique<Impl>()) {}
RouteResolver::~RouteResolver() = default;

void RouteResolver::set_default_agent_id(const std::string& agent_id) {
  impl_->default_agent_id = sanitize_agent_id(agent_id);
}

void RouteResolver::set_dm_scope(const std::string& scope) {
  impl_->dm_scope = to_lower(scope);
  impl_->route_cache.clear();
}

void RouteResolver::add_binding(const AgentRouteBinding& binding) {
  impl_->bindings.push_back(binding);
  impl_->rebuild_index();
}

void RouteResolver::clear_bindings() {
  impl_->bindings.clear();
  impl_->evaluated.clear();
  impl_->index = {};
  impl_->route_cache.clear();
}

void RouteResolver::register_agent(const std::string& agent_id) {
  impl_->registered_agents.insert(sanitize_agent_id(agent_id));
}

std::string RouteResolver::pick_first_existing_agent_id(const std::string& agent_id) const {
  const std::string normalized = sanitize_agent_id(agent_id);
  if (impl_->registered_agents.empty()) return normalized;
  if (impl_->registered_agents.count(normalized)) return normalized;
  return impl_->default_agent_id;
}

// ✅ 对齐 OpenClaw: resolveAgentRoute (resolve-route.ts:614-804)
// 7 层匹配优先级（tiers），与 OpenClaw 完全一致
ResolvedAgentRoute RouteResolver::resolve_route(const ResolveAgentRouteInput& input) {
  const std::string channel    = normalize_token(input.channel).empty()
                                  ? "unknown" : normalize_token(input.channel);
  const std::string account_id = normalize_account_id(input.account_id.value_or(""));
  const std::string guild_id   = input.guild_id ? to_lower(trim(*input.guild_id)) : "";
  const std::string team_id    = input.team_id  ? to_lower(trim(*input.team_id))  : "";
  const std::set<std::string> member_role_set(input.member_role_ids.begin(),
                                               input.member_role_ids.end());

  // ✅ 对齐 OpenClaw: buildResolvedRouteCacheKey
  std::ostringstream cache_key_builder;
  cache_key_builder << channel << "\t" << account_id << "\t";
  if (input.peer)
    cache_key_builder << chat_type_to_string(input.peer->kind) << ":" << input.peer->id;
  else
    cache_key_builder << "-";
  cache_key_builder << "\t";
  if (input.parent_peer)
    cache_key_builder << chat_type_to_string(input.parent_peer->kind) << ":" << input.parent_peer->id;
  else
    cache_key_builder << "-";
  cache_key_builder << "\t" << (guild_id.empty() ? "-" : guild_id);
  cache_key_builder << "\t" << (team_id.empty()  ? "-" : team_id);
  cache_key_builder << "\t" << impl_->dm_scope;

  const std::string cache_key = cache_key_builder.str();
  auto it = impl_->route_cache.find(cache_key);
  if (it != impl_->route_cache.end()) {
    return it->second;
  }

  // ── choose 函数：构建最终 route ─────────────────────────────────────────────
  auto choose = [&](const std::string& agent_id_raw,
                    RouteMatchSource matched_by) -> ResolvedAgentRoute {
    const std::string resolved_agent_id = pick_first_existing_agent_id(agent_id_raw);

    // ✅ 对齐 OpenClaw: sessionKey 全部 toLowerCase (choose函数 L670/674)
    const std::string session_key = to_lower(build_agent_peer_session_key({
        .agent_id   = resolved_agent_id,
        .main_key   = std::nullopt,
        .channel    = channel,
        .account_id = account_id,
        .peer_kind  = input.peer ? chat_type_to_string(input.peer->kind) : "direct",
        .peer_id    = input.peer ? std::optional<std::string>(input.peer->id) : std::nullopt,
        .dm_scope   = impl_->dm_scope,
    }));
    const std::string main_session_key = to_lower(build_agent_main_session_key({
        resolved_agent_id, std::nullopt}));

    ResolvedAgentRoute route;
    route.agent_id        = resolved_agent_id;
    route.channel         = channel;
    route.account_id      = account_id;
    route.session_key     = session_key;
    route.main_session_key= main_session_key;
    route.last_route_policy = derive_last_route_policy(session_key, main_session_key);
    route.matched_by      = matched_by;

    if (impl_->route_cache.size() < MAX_RESOLVED_ROUTE_CACHE_KEYS) {
      impl_->route_cache[cache_key] = route;
    }
    return route;
  };

  // ── 7 层匹配（tiers） ───────────────────────────────────────────────────────
  // ✅ 严格对齐 OpenClaw: tiers 数组 (resolve-route.ts:723-781)
  struct Tier {
    RouteMatchSource matched_by;
    bool enabled;
    std::optional<RoutePeer> scope_peer;
    std::vector<int> candidates;  // impl_->evaluated 的下标
  };

  // 收集各 tier 的候选 bindings
  auto collect_peer_bindings = [&](const std::optional<RoutePeer>& peer) -> std::vector<int> {
    if (!peer) return {};
    std::vector<int> result;
    std::set<int> seen;
    for (const auto& key : peer_lookup_keys(peer->kind, to_lower(trim(peer->id)))) {
      auto it2 = impl_->index.by_peer.find(key);
      if (it2 == impl_->index.by_peer.end()) continue;
      for (int i : it2->second) {
        if (seen.insert(i).second) result.push_back(i);
      }
    }
    return result;
  };

  const std::vector<Tier> tiers = {
    // Tier 1: binding.peer
    {RouteMatchSource::BINDING_PEER,        bool(input.peer),
     input.peer, collect_peer_bindings(input.peer)},
    // Tier 2: binding.peer.parent
    {RouteMatchSource::BINDING_PEER_PARENT, bool(input.parent_peer && !trim(input.parent_peer->id).empty()),
     input.parent_peer, collect_peer_bindings(input.parent_peer)},
    // Tier 3: binding.guild+roles
    {RouteMatchSource::BINDING_GUILD_ROLES, bool(!guild_id.empty() && !input.member_role_ids.empty()),
     input.peer, [&]{
       auto it2 = impl_->index.by_guild_roles.find(guild_id);
       return it2 != impl_->index.by_guild_roles.end() ? it2->second : std::vector<int>{};
     }()},
    // Tier 4: binding.guild
    {RouteMatchSource::BINDING_GUILD, bool(!guild_id.empty()),
     input.peer, [&]{
       auto it2 = impl_->index.by_guild.find(guild_id);
       return it2 != impl_->index.by_guild.end() ? it2->second : std::vector<int>{};
     }()},
    // Tier 5: binding.team
    {RouteMatchSource::BINDING_TEAM, bool(!team_id.empty()),
     input.peer, [&]{
       auto it2 = impl_->index.by_team.find(team_id);
       return it2 != impl_->index.by_team.end() ? it2->second : std::vector<int>{};
     }()},
    // Tier 6: binding.account
    {RouteMatchSource::BINDING_ACCOUNT, true, input.peer, impl_->index.by_account},
    // Tier 7: binding.channel (wildcard)
    {RouteMatchSource::BINDING_CHANNEL, true, input.peer, impl_->index.by_channel},
  };

  for (const auto& tier : tiers) {
    if (!tier.enabled) continue;
    for (int idx : tier.candidates) {
      const auto& eb = impl_->evaluated[idx];
      if (!matches_binding_scope(eb.match, tier.scope_peer,
                                 guild_id, team_id, member_role_set)) {
        continue;
      }
      return choose(eb.binding.agent_id, tier.matched_by);
    }
  }

  return choose(impl_->default_agent_id, RouteMatchSource::DEFAULT);
}

std::vector<std::string> RouteResolver::list_bound_account_ids(
    const std::string& channel_id) const {
  std::set<std::string> ids;
  const std::string normalized_channel = normalize_token(channel_id);
  for (const auto& binding : impl_->bindings) {
    if (!binding.match.channel ||
        normalize_token(*binding.match.channel) != normalized_channel) {
      continue;
    }
    if (binding.match.account_id) {
      const std::string account = normalize_account_id(*binding.match.account_id);
      if (account != "*") ids.insert(account);
    }
  }
  return std::vector<std::string>(ids.begin(), ids.end());
}

std::optional<std::string> RouteResolver::resolve_default_agent_bound_account_id(
    const std::string& channel_id) const {
  const std::string normalized_channel = normalize_token(channel_id);
  const std::string default_agent = sanitize_agent_id(impl_->default_agent_id);
  for (const auto& binding : impl_->bindings) {
    if (!binding.match.channel ||
        normalize_token(*binding.match.channel) != normalized_channel) {
      continue;
    }
    if (sanitize_agent_id(binding.agent_id) != default_agent) continue;
    if (binding.match.account_id) {
      const std::string account = normalize_account_id(*binding.match.account_id);
      if (account != "*") return account;
    }
  }
  return std::nullopt;
}

// ============================================================================
// BindingsRegistry
// ============================================================================

struct BindingsRegistry::Impl {
  std::unordered_map<std::string, std::vector<AgentRouteBinding>> by_channel;
};

BindingsRegistry::BindingsRegistry() : impl_(std::make_unique<Impl>()) {}
BindingsRegistry::~BindingsRegistry() = default;

void BindingsRegistry::add(const AgentRouteBinding& binding) {
  if (binding.match.channel) {
    impl_->by_channel[normalize_token(*binding.match.channel)].push_back(binding);
  }
}

void BindingsRegistry::remove_by_channel(const std::string& channel) {
  impl_->by_channel.erase(normalize_token(channel));
}

void BindingsRegistry::clear() { impl_->by_channel.clear(); }

std::vector<AgentRouteBinding> BindingsRegistry::list_by_channel(
    const std::string& channel) const {
  auto it = impl_->by_channel.find(normalize_token(channel));
  return it != impl_->by_channel.end() ? it->second : std::vector<AgentRouteBinding>{};
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
  const auto bindings = list_by_channel(channel);
  const std::string normalized_account = normalize_account_id(account_id);
  for (const auto& binding : bindings) {
    if (binding.match.account_id &&
        normalize_account_id(*binding.match.account_id) == normalized_account) {
      return sanitize_agent_id(binding.agent_id);
    }
  }
  return std::nullopt;
}

// ============================================================================
// AccountLookup
// ============================================================================

struct AccountLookup::Impl {
  std::unordered_map<std::string, std::set<std::string>> accounts_by_channel;
};

AccountLookup::AccountLookup() : impl_(std::make_unique<Impl>()) {}
AccountLookup::~AccountLookup() = default;

void AccountLookup::add_account(const std::string& channel, const std::string& account_id) {
  impl_->accounts_by_channel[normalize_token(channel)].insert(normalize_account_id(account_id));
}

std::vector<std::string> AccountLookup::list_accounts(const std::string& channel) const {
  auto it = impl_->accounts_by_channel.find(normalize_token(channel));
  if (it == impl_->accounts_by_channel.end()) return {};
  return std::vector<std::string>(it->second.begin(), it->second.end());
}

bool AccountLookup::has_account(const std::string& channel, const std::string& account_id) const {
  auto it = impl_->accounts_by_channel.find(normalize_token(channel));
  if (it == impl_->accounts_by_channel.end()) return false;
  return it->second.count(normalize_account_id(account_id)) > 0;
}

void AccountLookup::clear() { impl_->accounts_by_channel.clear(); }

}  // namespace routing
}  // namespace hunterclaw
