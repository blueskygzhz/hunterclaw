// ============================================================================
// device_pairing.cc
// ✅ 严格对齐 OpenClaw src/infra/device-pairing.ts (1039行)
// 完整实现：request → approve → verify → rotate → revoke 流水线
// ============================================================================

#include "hunterclaw/pairing/device_pairing.h"
#include "hunterclaw/pairing/node_pairing.h"  // generate/verify_pairing_token

#include <algorithm>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <set>

#include "hunterclaw/infra/utilities.h"

using json = nlohmann::json;

namespace hunterclaw {
namespace pairing {

namespace {

// ── Lock & Paths ────────────────────────────────────────────────────────────

std::mutex g_device_pairing_mutex;

struct DevicePairingPaths {
  std::string pending_path;
  std::string paired_path;
};

DevicePairingPaths resolve_device_pairing_paths(const std::string& base_dir) {
  std::string base = base_dir.empty() ? "/tmp/hunterclaw_pairing" : base_dir;
  return {base + "/devices_pending.json", base + "/devices_paired.json"};
}

// ── State File ──────────────────────────────────────────────────────────────

struct DevicePairingStateFile {
  std::unordered_map<std::string, DevicePairingPendingRequest> pending_by_id;
  std::unordered_map<std::string, PairedDevice> paired_by_device_id;
};

// ── JSON serialization ──────────────────────────────────────────────────────

void to_json(json& j, const DeviceAuthToken& t) {
  j["token"] = t.token;
  j["role"] = t.role;
  j["scopes"] = t.scopes;
  j["createdAtMs"] = t.created_at_ms;
  if (t.rotated_at_ms) j["rotatedAtMs"] = *t.rotated_at_ms;
  if (t.revoked_at_ms) j["revokedAtMs"] = *t.revoked_at_ms;
  if (t.last_used_at_ms) j["lastUsedAtMs"] = *t.last_used_at_ms;
}

void from_json(const json& j, DeviceAuthToken& t) {
  t.token = j.value("token", "");
  t.role = j.value("role", "");
  t.scopes = j.value("scopes", std::vector<std::string>{});
  t.created_at_ms = j.value("createdAtMs", 0LL);
  if (j.contains("rotatedAtMs")) t.rotated_at_ms = j["rotatedAtMs"].get<int64_t>();
  if (j.contains("revokedAtMs")) t.revoked_at_ms = j["revokedAtMs"].get<int64_t>();
  if (j.contains("lastUsedAtMs")) t.last_used_at_ms = j["lastUsedAtMs"].get<int64_t>();
}

void to_json(json& j, const DevicePairingPendingRequest& r) {
  j["requestId"] = r.request_id;
  j["deviceId"] = r.device_id;
  j["publicKey"] = r.public_key;
  if (r.display_name) j["displayName"] = *r.display_name;
  if (r.platform) j["platform"] = *r.platform;
  if (r.device_family) j["deviceFamily"] = *r.device_family;
  if (r.client_id) j["clientId"] = *r.client_id;
  if (r.client_mode) j["clientMode"] = *r.client_mode;
  if (r.role) j["role"] = *r.role;
  if (!r.roles.empty()) j["roles"] = r.roles;
  if (!r.scopes.empty()) j["scopes"] = r.scopes;
  if (r.remote_ip) j["remoteIp"] = *r.remote_ip;
  j["silent"] = r.silent;
  j["isRepair"] = r.is_repair;
  j["ts"] = r.ts;
}

void from_json(const json& j, DevicePairingPendingRequest& r) {
  r.request_id = j.value("requestId", "");
  r.device_id = j.value("deviceId", "");
  r.public_key = j.value("publicKey", "");
  if (j.contains("displayName")) r.display_name = j["displayName"].get<std::string>();
  if (j.contains("platform")) r.platform = j["platform"].get<std::string>();
  if (j.contains("deviceFamily")) r.device_family = j["deviceFamily"].get<std::string>();
  if (j.contains("clientId")) r.client_id = j["clientId"].get<std::string>();
  if (j.contains("clientMode")) r.client_mode = j["clientMode"].get<std::string>();
  if (j.contains("role")) r.role = j["role"].get<std::string>();
  r.roles = j.value("roles", std::vector<std::string>{});
  r.scopes = j.value("scopes", std::vector<std::string>{});
  if (j.contains("remoteIp")) r.remote_ip = j["remoteIp"].get<std::string>();
  r.silent = j.value("silent", false);
  r.is_repair = j.value("isRepair", false);
  r.ts = j.value("ts", 0LL);
}

void to_json(json& j, const PairedDevice& d) {
  j["deviceId"] = d.device_id;
  j["publicKey"] = d.public_key;
  if (d.display_name) j["displayName"] = *d.display_name;
  if (d.platform) j["platform"] = *d.platform;
  if (d.device_family) j["deviceFamily"] = *d.device_family;
  if (d.client_id) j["clientId"] = *d.client_id;
  if (d.client_mode) j["clientMode"] = *d.client_mode;
  if (d.role) j["role"] = *d.role;
  if (!d.roles.empty()) j["roles"] = d.roles;
  if (!d.scopes.empty()) j["scopes"] = d.scopes;
  if (!d.approved_scopes.empty()) j["approvedScopes"] = d.approved_scopes;
  if (d.remote_ip) j["remoteIp"] = *d.remote_ip;
  j["createdAtMs"] = d.created_at_ms;
  j["approvedAtMs"] = d.approved_at_ms;
  if (!d.tokens.empty()) {
    json tokens_obj = json::object();
    for (const auto& [role, tok] : d.tokens) {
      json tok_json;
      to_json(tok_json, tok);
      tokens_obj[role] = tok_json;
    }
    j["tokens"] = tokens_obj;
  }
}

void from_json(const json& j, PairedDevice& d) {
  d.device_id = j.value("deviceId", "");
  d.public_key = j.value("publicKey", "");
  if (j.contains("displayName")) d.display_name = j["displayName"].get<std::string>();
  if (j.contains("platform")) d.platform = j["platform"].get<std::string>();
  if (j.contains("deviceFamily")) d.device_family = j["deviceFamily"].get<std::string>();
  if (j.contains("clientId")) d.client_id = j["clientId"].get<std::string>();
  if (j.contains("clientMode")) d.client_mode = j["clientMode"].get<std::string>();
  if (j.contains("role")) d.role = j["role"].get<std::string>();
  d.roles = j.value("roles", std::vector<std::string>{});
  d.scopes = j.value("scopes", std::vector<std::string>{});
  d.approved_scopes = j.value("approvedScopes", std::vector<std::string>{});
  if (j.contains("remoteIp")) d.remote_ip = j["remoteIp"].get<std::string>();
  d.created_at_ms = j.value("createdAtMs", 0LL);
  d.approved_at_ms = j.value("approvedAtMs", 0LL);
  if (j.contains("tokens") && j["tokens"].is_object()) {
    for (auto& [role, tok_json] : j["tokens"].items()) {
      DeviceAuthToken tok;
      from_json(tok_json, tok);
      d.tokens[role] = tok;
    }
  }
}

// ── State Load/Persist ──────────────────────────────────────────────────────

void prune_expired_pending(
    std::unordered_map<std::string, DevicePairingPendingRequest>& pending,
    int64_t now_ms) {
  std::vector<std::string> expired;
  for (const auto& [id, req] : pending) {
    if (now_ms - req.ts > DEVICE_PAIRING_PENDING_TTL_MS) {
      expired.push_back(id);
    }
  }
  for (const auto& id : expired) {
    pending.erase(id);
  }
}

DevicePairingStateFile load_state(const std::string& base_dir) {
  auto paths = resolve_device_pairing_paths(base_dir);
  DevicePairingStateFile state;
  try {
    std::ifstream f(paths.pending_path);
    if (f.is_open()) {
      json j; f >> j;
      for (auto& [id, rj] : j.items()) {
        DevicePairingPendingRequest r;
        from_json(rj, r);
        state.pending_by_id[id] = r;
      }
    }
  } catch (...) {}
  try {
    std::ifstream f(paths.paired_path);
    if (f.is_open()) {
      json j; f >> j;
      for (auto& [id, dj] : j.items()) {
        PairedDevice d;
        from_json(dj, d);
        state.paired_by_device_id[id] = d;
      }
    }
  } catch (...) {}
  prune_expired_pending(state.pending_by_id, infra::current_timestamp_ms());
  return state;
}

void persist_state(const DevicePairingStateFile& state, const std::string& base_dir) {
  auto paths = resolve_device_pairing_paths(base_dir);
  try {
    json pj = json::object();
    for (const auto& [id, r] : state.pending_by_id) {
      json rj; to_json(rj, r); pj[id] = rj;
    }
    std::ofstream(paths.pending_path) << pj.dump(2);
  } catch (...) {}
  try {
    json dj = json::object();
    for (const auto& [id, d] : state.paired_by_device_id) {
      json djj; to_json(djj, d); dj[id] = djj;
    }
    std::ofstream(paths.paired_path) << dj.dump(2);
  } catch (...) {}
}

// ── Utility ─────────────────────────────────────────────────────────────────

std::string normalize_device_id(const std::string& id) {
  return infra::trim(id);
}

std::optional<std::string> normalize_role(const std::string& role) {
  std::string trimmed = infra::trim(role);
  return trimmed.empty() ? std::nullopt : std::optional<std::string>(trimmed);
}

// ✅ 对齐 OpenClaw: mergeRoles — 合并多个 role 来源，去重
std::vector<std::string> merge_roles(
    const std::vector<std::vector<std::string>>& sources,
    const std::vector<std::string>& singles = {}) {
  std::set<std::string> roles;
  for (const auto& src : sources) {
    for (const auto& r : src) {
      std::string trimmed = infra::trim(r);
      if (!trimmed.empty()) roles.insert(trimmed);
    }
  }
  for (const auto& r : singles) {
    std::string trimmed = infra::trim(r);
    if (!trimmed.empty()) roles.insert(trimmed);
  }
  return {roles.begin(), roles.end()};
}

// ✅ 对齐 OpenClaw: mergeScopes
std::vector<std::string> merge_scopes(
    const std::vector<std::vector<std::string>>& sources) {
  std::set<std::string> scopes;
  for (const auto& src : sources) {
    for (const auto& s : src) {
      std::string trimmed = infra::trim(s);
      if (!trimmed.empty()) scopes.insert(trimmed);
    }
  }
  return {scopes.begin(), scopes.end()};
}

// ✅ 对齐 OpenClaw: roleScopesAllow
bool role_scopes_allow(const std::string& role,
                       const std::vector<std::string>& requested,
                       const std::vector<std::string>& allowed) {
  std::set<std::string> allowed_set(allowed.begin(), allowed.end());
  for (const auto& scope : requested) {
    if (scope.empty()) continue;
    // operator.* scopes only checked for operator role
    bool is_operator_scope = scope.compare(0, 9, "operator.") == 0;
    if (role == OPERATOR_ROLE && !is_operator_scope) continue;
    if (role != OPERATOR_ROLE && is_operator_scope) continue;
    if (allowed_set.find(scope) == allowed_set.end()) return false;
  }
  return true;
}

// ✅ 对齐 OpenClaw: resolveMissingRequestedScope
std::optional<std::string> resolve_missing_scope(
    const std::string& role,
    const std::vector<std::string>& requested,
    const std::vector<std::string>& allowed) {
  std::set<std::string> allowed_set(allowed.begin(), allowed.end());
  for (const auto& scope : requested) {
    if (!scope.empty() && allowed_set.find(scope) == allowed_set.end()) {
      return scope;
    }
  }
  return std::nullopt;
}

// ✅ 对齐 OpenClaw: resolveScopeOutsideRequestedRoles
std::optional<std::string> resolve_scope_outside_roles(
    const std::vector<std::string>& roles,
    const std::vector<std::string>& scopes) {
  std::set<std::string> role_set(roles.begin(), roles.end());
  bool has_operator = role_set.count(OPERATOR_ROLE) > 0;
  for (const auto& scope : scopes) {
    bool is_op = scope.compare(0, 9, "operator.") == 0;
    if (is_op && !has_operator) return scope;
  }
  return std::nullopt;
}

// ✅ 对齐 OpenClaw: resolveRoleScopedDeviceTokenScopes
std::vector<std::string> resolve_role_scoped_token_scopes(
    const std::string& role, const std::vector<std::string>& scopes) {
  std::vector<std::string> result;
  for (const auto& scope : scopes) {
    std::string trimmed = infra::trim(scope);
    if (trimmed.empty()) continue;
    bool is_op = trimmed.compare(0, 9, "operator.") == 0;
    if (role == OPERATOR_ROLE) {
      if (is_op) result.push_back(trimmed);
    } else {
      if (!is_op) result.push_back(trimmed);
    }
  }
  return result;
}

// ✅ 对齐 OpenClaw: resolveApprovedDeviceScopeBaseline
std::vector<std::string> resolve_approved_scope_baseline(const PairedDevice& device) {
  if (!device.approved_scopes.empty()) return device.approved_scopes;
  return device.scopes;
}

// ✅ 对齐 OpenClaw: scopesWithinApprovedDeviceBaseline
bool scopes_within_approved_baseline(const std::string& role,
                                      const std::vector<std::string>& scopes,
                                      const std::vector<std::string>& approved) {
  if (approved.empty()) return false;
  return role_scopes_allow(role, scopes, approved);
}

std::string new_token() { return generate_pairing_token(); }

}  // namespace

// ============================================================================
// format_device_pairing_forbidden_message (device-pairing.ts:127-141)
// ============================================================================

std::string format_device_pairing_forbidden_message(
    const DevicePairingForbiddenResult& result) {
  switch (result.reason) {
    case DevicePairingForbiddenReason::CALLER_SCOPES_REQUIRED:
      return "missing scope: " + result.scope.value_or("callerScopes-required");
    case DevicePairingForbiddenReason::CALLER_MISSING_SCOPE:
      return "missing scope: " + result.scope.value_or("unknown");
    case DevicePairingForbiddenReason::SCOPE_OUTSIDE_REQUESTED_ROLES:
      return "invalid scope for requested roles: " + result.scope.value_or("unknown");
    case DevicePairingForbiddenReason::BOOTSTRAP_ROLE_NOT_ALLOWED:
      return "bootstrap profile does not allow role: " + result.role.value_or("unknown");
    case DevicePairingForbiddenReason::BOOTSTRAP_SCOPE_NOT_ALLOWED:
      return "bootstrap profile does not allow scope: " + result.scope.value_or("unknown");
  }
  return "unknown forbidden reason";
}

// ============================================================================
// Role & Scope Helpers (device-pairing.ts:213-245)
// ============================================================================

std::vector<std::string> list_approved_paired_device_roles(const PairedDevice& device) {
  std::vector<std::string> singles;
  if (device.role) singles.push_back(*device.role);
  return merge_roles({device.roles}, singles);
}

std::vector<std::string> list_effective_paired_device_roles(const PairedDevice& device) {
  // ✅ 对齐 OpenClaw: effective roles = active token roles ∩ approved roles
  std::vector<std::string> active_token_roles;
  for (const auto& [role, tok] : device.tokens) {
    if (!tok.revoked_at_ms) active_token_roles.push_back(tok.role);
  }
  if (active_token_roles.empty()) return {};
  std::set<std::string> approved(list_approved_paired_device_roles(device).begin(),
                                  list_approved_paired_device_roles(device).end());
  std::vector<std::string> result;
  for (const auto& r : active_token_roles) {
    if (approved.count(r)) result.push_back(r);
  }
  return result;
}

bool has_effective_paired_device_role(const PairedDevice& device, const std::string& role) {
  auto nr = normalize_role(role);
  if (!nr) return false;
  auto effective = list_effective_paired_device_roles(device);
  return std::find(effective.begin(), effective.end(), *nr) != effective.end();
}

// ============================================================================
// list / get (device-pairing.ts:451-474)
// ============================================================================

DevicePairingList list_device_pairing(const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  std::vector<DevicePairingPendingRequest> pending;
  for (const auto& [_, r] : state.pending_by_id) pending.push_back(r);
  std::sort(pending.begin(), pending.end(),
            [](const auto& a, const auto& b) { return b.ts < a.ts; });
  std::vector<PairedDevice> paired;
  for (const auto& [_, d] : state.paired_by_device_id) paired.push_back(d);
  std::sort(paired.begin(), paired.end(),
            [](const auto& a, const auto& b) { return b.approved_at_ms < a.approved_at_ms; });
  return {pending, paired};
}

std::optional<PairedDevice> get_paired_device(const std::string& device_id,
                                               const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto it = state.paired_by_device_id.find(normalize_device_id(device_id));
  return it != state.paired_by_device_id.end() ? std::optional(it->second) : std::nullopt;
}

std::optional<DevicePairingPendingRequest> get_pending_device_pairing(
    const std::string& request_id, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto it = state.pending_by_id.find(request_id);
  return it != state.pending_by_id.end() ? std::optional(it->second) : std::nullopt;
}

// ============================================================================
// requestDevicePairing (device-pairing.ts:476-532)
// ============================================================================

RequestDevicePairingResult request_device_pairing(
    const DevicePairingPendingRequest& req, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  std::string device_id = normalize_device_id(req.device_id);
  if (device_id.empty()) throw std::runtime_error("deviceId required");

  bool is_repair = state.paired_by_device_id.count(device_id) > 0;

  // ✅ 对齐 OpenClaw: 单个 pending 且 snapshot 匹配 → refresh
  std::vector<std::string> pending_ids;
  for (const auto& [id, p] : state.pending_by_id) {
    if (p.device_id == device_id) pending_ids.push_back(id);
  }

  if (pending_ids.size() == 1) {
    auto& existing = state.pending_by_id[pending_ids[0]];
    if (existing.public_key == req.public_key) {
      // Refresh: keep original ts for queue fairness
      if (req.display_name) existing.display_name = req.display_name;
      if (req.platform) existing.platform = req.platform;
      if (req.device_family) existing.device_family = req.device_family;
      if (req.client_id) existing.client_id = req.client_id;
      if (req.client_mode) existing.client_mode = req.client_mode;
      if (req.remote_ip) existing.remote_ip = req.remote_ip;
      existing.silent = existing.silent && req.silent;
      existing.is_repair = existing.is_repair || is_repair;
      persist_state(state, base_dir);
      return {"pending", existing, false};
    }
  }

  // Remove all stale pending for this device, create new
  for (const auto& id : pending_ids) state.pending_by_id.erase(id);

  DevicePairingPendingRequest new_req = req;
  new_req.request_id = infra::generate_uuid();
  new_req.device_id = device_id;
  new_req.is_repair = is_repair;
  new_req.ts = infra::current_timestamp_ms();
  if (!new_req.role) {
    auto nr = normalize_role(req.role.value_or(""));
    if (nr) new_req.role = nr;
  }

  state.pending_by_id[new_req.request_id] = new_req;
  persist_state(state, base_dir);
  return {"pending", new_req, true};
}

// ============================================================================
// approveDevicePairing (device-pairing.ts:534-642)
// ============================================================================

std::optional<ApproveDevicePairingResult> approve_device_pairing(
    const std::string& request_id,
    const std::vector<std::string>& caller_scopes,
    const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto pit = state.pending_by_id.find(request_id);
  if (pit == state.pending_by_id.end()) return std::nullopt;

  const auto& pending = pit->second;

  // ✅ 对齐 OpenClaw: requested roles/scopes validation
  std::vector<std::string> singles;
  if (pending.role) singles.push_back(*pending.role);
  auto requested_roles = merge_roles({pending.roles}, singles);
  auto requested_scopes = pending.scopes;

  // Check scope outside requested roles
  auto role_mismatch = resolve_scope_outside_roles(requested_roles, requested_scopes);
  if (role_mismatch) {
    ApproveDevicePairingResult r;
    r.status = ApproveDevicePairingResult::Status::FORBIDDEN;
    r.forbidden = DevicePairingForbiddenResult{
        DevicePairingForbiddenReason::SCOPE_OUTSIDE_REQUESTED_ROLES, role_mismatch, std::nullopt};
    return r;
  }

  // Check operator scopes require callerScopes
  std::vector<std::string> op_scopes;
  for (const auto& s : requested_scopes) {
    if (s.compare(0, 9, "operator.") == 0) op_scopes.push_back(s);
  }
  if (!op_scopes.empty()) {
    if (caller_scopes.empty()) {
      ApproveDevicePairingResult r;
      r.status = ApproveDevicePairingResult::Status::FORBIDDEN;
      r.forbidden = DevicePairingForbiddenResult{
          DevicePairingForbiddenReason::CALLER_SCOPES_REQUIRED, op_scopes[0], std::nullopt};
      return r;
    }
    auto missing = resolve_missing_scope(OPERATOR_ROLE, op_scopes, caller_scopes);
    if (missing) {
      ApproveDevicePairingResult r;
      r.status = ApproveDevicePairingResult::Status::FORBIDDEN;
      r.forbidden = DevicePairingForbiddenResult{
          DevicePairingForbiddenReason::CALLER_MISSING_SCOPE, missing, std::nullopt};
      return r;
    }
  }

  int64_t now = infra::current_timestamp_ms();
  auto eit = state.paired_by_device_id.find(pending.device_id);
  PairedDevice* existing = eit != state.paired_by_device_id.end() ? &eit->second : nullptr;

  // ✅ 对齐 OpenClaw: merge roles, scopes, tokens
  std::vector<std::string> existing_roles = existing ? existing->roles : std::vector<std::string>{};
  std::vector<std::string> existing_singles;
  if (existing && existing->role) existing_singles.push_back(*existing->role);
  auto merged_roles = merge_roles({existing_roles, pending.roles, requested_roles}, existing_singles);

  auto existing_scopes = existing ? (existing->approved_scopes.empty() ? existing->scopes : existing->approved_scopes)
                                  : std::vector<std::string>{};
  auto approved_scopes = merge_scopes({existing_scopes, pending.scopes});

  // Build tokens per role
  std::unordered_map<std::string, DeviceAuthToken> tokens;
  if (existing) tokens = existing->tokens;
  for (const auto& role : requested_roles) {
    auto existing_tok_it = tokens.find(role);
    DeviceAuthToken* existing_tok = existing_tok_it != tokens.end() ? &existing_tok_it->second : nullptr;
    auto token_scopes = resolve_role_scoped_token_scopes(role, pending.scopes);
    if (token_scopes.empty() && existing_tok) {
      token_scopes = resolve_role_scoped_token_scopes(role, existing_tok->scopes);
    }
    if (token_scopes.empty()) {
      token_scopes = resolve_role_scoped_token_scopes(role, approved_scopes);
    }

    DeviceAuthToken new_tok;
    new_tok.token = new_token();
    new_tok.role = role;
    new_tok.scopes = token_scopes;
    new_tok.created_at_ms = existing_tok ? existing_tok->created_at_ms : now;
    if (existing_tok) new_tok.rotated_at_ms = now;
    if (existing_tok) new_tok.last_used_at_ms = existing_tok->last_used_at_ms;
    tokens[role] = new_tok;
  }

  PairedDevice device;
  device.device_id = pending.device_id;
  device.public_key = pending.public_key;
  device.display_name = pending.display_name;
  device.platform = pending.platform;
  device.device_family = pending.device_family;
  device.client_id = pending.client_id;
  device.client_mode = pending.client_mode;
  device.role = pending.role;
  device.roles = merged_roles;
  device.scopes = approved_scopes;
  device.approved_scopes = approved_scopes;
  device.remote_ip = pending.remote_ip;
  device.tokens = tokens;
  device.created_at_ms = existing ? existing->created_at_ms : now;
  device.approved_at_ms = now;

  state.pending_by_id.erase(request_id);
  state.paired_by_device_id[device.device_id] = device;
  persist_state(state, base_dir);

  ApproveDevicePairingResult r;
  r.status = ApproveDevicePairingResult::Status::APPROVED;
  r.request_id = request_id;
  r.device = device;
  return r;
}

// ============================================================================
// approveBootstrapDevicePairing (device-pairing.ts:644-729)
// ============================================================================

std::optional<ApproveDevicePairingResult> approve_bootstrap_device_pairing(
    const std::string& request_id,
    const DeviceBootstrapProfile& profile,
    const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto pit = state.pending_by_id.find(request_id);
  if (pit == state.pending_by_id.end()) return std::nullopt;

  const auto& pending = pit->second;
  std::vector<std::string> singles;
  if (pending.role) singles.push_back(*pending.role);
  auto requested_roles = merge_roles({pending.roles}, singles);

  // Check roles within bootstrap profile
  std::set<std::string> allowed_roles(profile.roles.begin(), profile.roles.end());
  for (const auto& role : requested_roles) {
    if (allowed_roles.find(role) == allowed_roles.end()) {
      ApproveDevicePairingResult r;
      r.status = ApproveDevicePairingResult::Status::FORBIDDEN;
      r.forbidden = DevicePairingForbiddenResult{
          DevicePairingForbiddenReason::BOOTSTRAP_ROLE_NOT_ALLOWED, std::nullopt, role};
      return r;
    }
  }

  // Check operator scopes within bootstrap profile
  std::vector<std::string> op_scopes;
  for (const auto& s : pending.scopes) {
    if (s.compare(0, 9, "operator.") == 0) op_scopes.push_back(s);
  }
  auto missing = resolve_missing_scope(OPERATOR_ROLE, op_scopes, profile.scopes);
  if (missing) {
    ApproveDevicePairingResult r;
    r.status = ApproveDevicePairingResult::Status::FORBIDDEN;
    r.forbidden = DevicePairingForbiddenResult{
        DevicePairingForbiddenReason::BOOTSTRAP_SCOPE_NOT_ALLOWED, missing, std::nullopt};
    return r;
  }

  int64_t now = infra::current_timestamp_ms();
  auto eit = state.paired_by_device_id.find(pending.device_id);
  PairedDevice* existing = eit != state.paired_by_device_id.end() ? &eit->second : nullptr;

  auto existing_roles = existing ? existing->roles : std::vector<std::string>{};
  std::vector<std::string> existing_singles;
  if (existing && existing->role) existing_singles.push_back(*existing->role);
  auto merged_roles = merge_roles({existing_roles, pending.roles, profile.roles}, existing_singles);
  if (pending.role) merged_roles = merge_roles({merged_roles}, {*pending.role});

  auto existing_scopes = existing ? (existing->approved_scopes.empty() ? existing->scopes : existing->approved_scopes)
                                  : std::vector<std::string>{};
  auto next_approved = merge_scopes({existing_scopes, pending.scopes, profile.scopes});

  std::unordered_map<std::string, DeviceAuthToken> tokens;
  if (existing) tokens = existing->tokens;
  for (const auto& role : profile.roles) {
    auto existing_tok_it = tokens.find(role);
    DeviceAuthToken* existing_tok = existing_tok_it != tokens.end() ? &existing_tok_it->second : nullptr;
    auto token_scopes = (role == OPERATOR_ROLE)
        ? resolve_role_scoped_token_scopes(role, profile.scopes)
        : std::vector<std::string>{};
    DeviceAuthToken new_tok;
    new_tok.token = new_token();
    new_tok.role = role;
    new_tok.scopes = token_scopes;
    new_tok.created_at_ms = existing_tok ? existing_tok->created_at_ms : now;
    if (existing_tok) new_tok.rotated_at_ms = now;
    if (existing_tok) new_tok.last_used_at_ms = existing_tok->last_used_at_ms;
    tokens[role] = new_tok;
  }

  PairedDevice device;
  device.device_id = pending.device_id;
  device.public_key = pending.public_key;
  device.display_name = pending.display_name;
  device.platform = pending.platform;
  device.device_family = pending.device_family;
  device.client_id = pending.client_id;
  device.client_mode = pending.client_mode;
  device.role = pending.role;
  device.roles = merged_roles;
  device.scopes = next_approved;
  device.approved_scopes = next_approved;
  device.remote_ip = pending.remote_ip;
  device.tokens = tokens;
  device.created_at_ms = existing ? existing->created_at_ms : now;
  device.approved_at_ms = now;

  state.pending_by_id.erase(request_id);
  state.paired_by_device_id[device.device_id] = device;
  persist_state(state, base_dir);

  ApproveDevicePairingResult r;
  r.status = ApproveDevicePairingResult::Status::APPROVED;
  r.request_id = request_id;
  r.device = device;
  return r;
}

// ============================================================================
// rejectDevicePairing (device-pairing.ts:731-748)
// ============================================================================

std::optional<RejectDevicePairingResult> reject_device_pairing(
    const std::string& request_id, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto it = state.pending_by_id.find(request_id);
  if (it == state.pending_by_id.end()) return std::nullopt;
  std::string device_id = it->second.device_id;
  state.pending_by_id.erase(request_id);
  persist_state(state, base_dir);
  return RejectDevicePairingResult{request_id, device_id};
}

// ============================================================================
// removePairedDevice (device-pairing.ts:750-764)
// ============================================================================

std::optional<std::string> remove_paired_device(
    const std::string& device_id, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto n = normalize_device_id(device_id);
  if (state.paired_by_device_id.erase(n) == 0) return std::nullopt;
  persist_state(state, base_dir);
  return n;
}

// ============================================================================
// updatePairedDeviceMetadata (device-pairing.ts:766-794)
// ============================================================================

void update_paired_device_metadata(const std::string& device_id,
                                    const PairedDeviceMetadataPatch& patch,
                                    const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto n = normalize_device_id(device_id);
  auto it = state.paired_by_device_id.find(n);
  if (it == state.paired_by_device_id.end()) return;
  auto& d = it->second;
  if (patch.display_name) d.display_name = patch.display_name;
  if (patch.client_id) d.client_id = patch.client_id;
  if (patch.client_mode) d.client_mode = patch.client_mode;
  if (patch.remote_ip) d.remote_ip = patch.remote_ip;
  persist_state(state, base_dir);
}

// ============================================================================
// summarizeDeviceTokens (device-pairing.ts:796-813)
// ============================================================================

std::optional<std::vector<DeviceAuthTokenSummary>> summarize_device_tokens(
    const std::unordered_map<std::string, DeviceAuthToken>& tokens) {
  if (tokens.empty()) return std::nullopt;
  std::vector<DeviceAuthTokenSummary> summaries;
  for (const auto& [_, tok] : tokens) {
    summaries.push_back({tok.role, tok.scopes, tok.created_at_ms,
                         tok.rotated_at_ms, tok.revoked_at_ms, tok.last_used_at_ms});
  }
  std::sort(summaries.begin(), summaries.end(),
            [](const auto& a, const auto& b) { return a.role < b.role; });
  return summaries;
}

// ============================================================================
// verifyDeviceToken (device-pairing.ts:815-863)
// ============================================================================

VerifyDeviceTokenResult verify_device_token(
    const std::string& device_id, const std::string& token,
    const std::string& role, const std::vector<std::string>& scopes,
    const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto n = normalize_device_id(device_id);
  auto dit = state.paired_by_device_id.find(n);
  if (dit == state.paired_by_device_id.end()) return {false, "device-not-paired"};
  auto& device = dit->second;

  auto nr = normalize_role(role);
  if (!nr) return {false, "role-missing"};

  auto tit = device.tokens.find(*nr);
  if (tit == device.tokens.end()) return {false, "token-missing"};
  auto& entry = tit->second;

  if (entry.revoked_at_ms) return {false, "token-revoked"};
  if (!verify_pairing_token(token, entry.token)) return {false, "token-mismatch"};

  auto approved = resolve_approved_scope_baseline(device);
  if (!scopes_within_approved_baseline(*nr, entry.scopes, approved))
    return {false, "scope-mismatch"};

  if (!role_scopes_allow(*nr, scopes, entry.scopes))
    return {false, "scope-mismatch"};

  // Update last used
  entry.last_used_at_ms = infra::current_timestamp_ms();
  persist_state(state, base_dir);
  return {true, ""};
}

// ============================================================================
// ensureDeviceToken (device-pairing.ts:865-920)
// ============================================================================

std::optional<DeviceAuthToken> ensure_device_token(
    const std::string& device_id, const std::string& role,
    const std::vector<std::string>& scopes, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto n = normalize_device_id(device_id);
  auto dit = state.paired_by_device_id.find(n);
  if (dit == state.paired_by_device_id.end()) return std::nullopt;
  auto& device = dit->second;

  auto nr = normalize_role(role);
  if (!nr) return std::nullopt;

  // Must be within approved roles
  auto approved_roles = list_approved_paired_device_roles(device);
  if (std::find(approved_roles.begin(), approved_roles.end(), *nr) == approved_roles.end())
    return std::nullopt;

  auto approved = resolve_approved_scope_baseline(device);
  if (!scopes_within_approved_baseline(*nr, scopes, approved)) return std::nullopt;

  auto tit = device.tokens.find(*nr);
  if (tit != device.tokens.end() && !tit->second.revoked_at_ms) {
    auto& existing = tit->second;
    if (scopes_within_approved_baseline(*nr, existing.scopes, approved) &&
        role_scopes_allow(*nr, scopes, existing.scopes)) {
      return existing;
    }
  }

  int64_t now = infra::current_timestamp_ms();
  DeviceAuthToken* existing_tok = tit != device.tokens.end() ? &tit->second : nullptr;
  DeviceAuthToken new_tok;
  new_tok.token = new_token();
  new_tok.role = *nr;
  new_tok.scopes = scopes;
  new_tok.created_at_ms = existing_tok ? existing_tok->created_at_ms : now;
  if (existing_tok) new_tok.rotated_at_ms = now;
  if (existing_tok) new_tok.last_used_at_ms = existing_tok->last_used_at_ms;
  device.tokens[*nr] = new_tok;
  persist_state(state, base_dir);
  return new_tok;
}

// ============================================================================
// rotateDeviceToken (device-pairing.ts:950-997)
// ============================================================================

RotateDeviceTokenResult rotate_device_token(
    const std::string& device_id, const std::string& role,
    const std::vector<std::string>& scopes, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto n = normalize_device_id(device_id);
  auto dit = state.paired_by_device_id.find(n);
  if (dit == state.paired_by_device_id.end())
    return {false, std::nullopt, RotateDeviceTokenDenyReason::UNKNOWN_DEVICE_OR_ROLE};
  auto& device = dit->second;

  auto nr = normalize_role(role);
  if (!nr) return {false, std::nullopt, RotateDeviceTokenDenyReason::UNKNOWN_DEVICE_OR_ROLE};

  auto approved_roles = list_approved_paired_device_roles(device);
  if (std::find(approved_roles.begin(), approved_roles.end(), *nr) == approved_roles.end())
    return {false, std::nullopt, RotateDeviceTokenDenyReason::UNKNOWN_DEVICE_OR_ROLE};

  auto approved = resolve_approved_scope_baseline(device);
  if (approved.empty())
    return {false, std::nullopt, RotateDeviceTokenDenyReason::MISSING_APPROVED_SCOPE_BASELINE};

  auto tit = device.tokens.find(*nr);
  DeviceAuthToken* existing = tit != device.tokens.end() ? &tit->second : nullptr;
  auto requested = scopes.empty() ? (existing ? existing->scopes : device.scopes) : scopes;

  if (!scopes_within_approved_baseline(*nr, requested, approved))
    return {false, std::nullopt, RotateDeviceTokenDenyReason::SCOPE_OUTSIDE_APPROVED_BASELINE};

  int64_t now = infra::current_timestamp_ms();
  DeviceAuthToken new_tok;
  new_tok.token = new_token();
  new_tok.role = *nr;
  new_tok.scopes = requested;
  new_tok.created_at_ms = existing ? existing->created_at_ms : now;
  new_tok.rotated_at_ms = now;
  if (existing) new_tok.last_used_at_ms = existing->last_used_at_ms;
  device.tokens[*nr] = new_tok;
  persist_state(state, base_dir);
  return {true, new_tok, std::nullopt};
}

// ============================================================================
// revokeDeviceToken (device-pairing.ts:999-1025)
// ============================================================================

std::optional<DeviceAuthToken> revoke_device_token(
    const std::string& device_id, const std::string& role,
    const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto n = normalize_device_id(device_id);
  auto dit = state.paired_by_device_id.find(n);
  if (dit == state.paired_by_device_id.end()) return std::nullopt;
  auto& device = dit->second;
  auto nr = normalize_role(role);
  if (!nr) return std::nullopt;
  auto tit = device.tokens.find(*nr);
  if (tit == device.tokens.end()) return std::nullopt;
  tit->second.revoked_at_ms = infra::current_timestamp_ms();
  persist_state(state, base_dir);
  return tit->second;
}

// ============================================================================
// clearDevicePairing (device-pairing.ts:1027-1038)
// ============================================================================

bool clear_device_pairing(const std::string& device_id, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_device_pairing_mutex);
  auto state = load_state(base_dir);
  auto n = normalize_device_id(device_id);
  if (state.paired_by_device_id.erase(n) == 0) return false;
  persist_state(state, base_dir);
  return true;
}

}  // namespace pairing
}  // namespace hunterclaw
