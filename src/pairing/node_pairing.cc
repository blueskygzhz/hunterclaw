// HunterClaw - Node Pairing Implementation
// 严格对齐 OpenClaw src/infra/node-pairing.ts

#include "hunterclaw/pairing/node_pairing.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>

#include "hunterclaw/infra/utilities.h"

using json = nlohmann::json;

namespace hunterclaw {
namespace pairing {

namespace {

// ============================================================================
// File Lock & Persistence (node-pairing.ts:55)
// ============================================================================

std::mutex g_node_pairing_mutex;

struct NodePairingPaths {
  std::string pending_path;
  std::string paired_path;
};

NodePairingPaths resolve_pairing_paths(const std::string& base_dir) {
  std::string base =
      base_dir.empty() ? "/tmp/hunterclaw_pairing" : base_dir;
  return {base + "/nodes_pending.json", base + "/nodes_paired.json"};
}

struct NodePairingStateFile {
  std::unordered_map<std::string, NodePairingPendingRequest> pending_by_id;
  std::unordered_map<std::string, NodePairingPairedNode> paired_by_node_id;
};

// ============================================================================
// JSON Serialization
// ============================================================================

void to_json(json& j, const NodePairingNodeMetadata& meta) {
  j["nodeId"] = meta.node_id;
  if (meta.display_name) j["displayName"] = *meta.display_name;
  if (meta.platform) j["platform"] = *meta.platform;
  if (meta.version) j["version"] = *meta.version;
  if (meta.core_version) j["coreVersion"] = *meta.core_version;
  if (meta.ui_version) j["uiVersion"] = *meta.ui_version;
  if (meta.device_family) j["deviceFamily"] = *meta.device_family;
  if (meta.model_identifier) j["modelIdentifier"] = *meta.model_identifier;
  if (meta.remote_ip) j["remoteIp"] = *meta.remote_ip;
  if (!meta.caps.empty()) j["caps"] = meta.caps;
  if (!meta.commands.empty()) j["commands"] = meta.commands;
  if (!meta.permissions.empty()) j["permissions"] = meta.permissions;
}

void from_json(const json& j, NodePairingNodeMetadata& meta) {
  meta.node_id = j.value("nodeId", "");
  if (j.contains("displayName")) meta.display_name = j["displayName"];
  if (j.contains("platform")) meta.platform = j["platform"];
  if (j.contains("version")) meta.version = j["version"];
  if (j.contains("coreVersion")) meta.core_version = j["coreVersion"];
  if (j.contains("uiVersion")) meta.ui_version = j["uiVersion"];
  if (j.contains("deviceFamily")) meta.device_family = j["deviceFamily"];
  if (j.contains("modelIdentifier"))
    meta.model_identifier = j["modelIdentifier"];
  if (j.contains("remoteIp")) meta.remote_ip = j["remoteIp"];
  if (j.contains("caps")) meta.caps = j["caps"].get<std::vector<std::string>>();
  if (j.contains("commands"))
    meta.commands = j["commands"].get<std::vector<std::string>>();
  if (j.contains("permissions"))
    meta.permissions =
        j["permissions"].get<std::unordered_map<std::string, bool>>();
}

void to_json(json& j, const NodePairingPendingRequest& req) {
  to_json(j, static_cast<const NodePairingNodeMetadata&>(req));
  j["requestId"] = req.request_id;
  j["silent"] = req.silent;
  j["isRepair"] = req.is_repair;
  j["ts"] = req.ts;
}

void from_json(const json& j, NodePairingPendingRequest& req) {
  from_json(j, static_cast<NodePairingNodeMetadata&>(req));
  req.request_id = j.value("requestId", "");
  req.silent = j.value("silent", false);
  req.is_repair = j.value("isRepair", false);
  req.ts = j.value("ts", 0LL);
}

void to_json(json& j, const NodePairingPairedNode& node) {
  to_json(j, static_cast<const NodePairingNodeMetadata&>(node));
  j["token"] = node.token;
  j["createdAtMs"] = node.created_at_ms;
  j["approvedAtMs"] = node.approved_at_ms;
  if (!node.bins.empty()) j["bins"] = node.bins;
  if (node.last_connected_at_ms)
    j["lastConnectedAtMs"] = *node.last_connected_at_ms;
}

void from_json(const json& j, NodePairingPairedNode& node) {
  from_json(j, static_cast<NodePairingNodeMetadata&>(node));
  node.token = j.value("token", "");
  node.created_at_ms = j.value("createdAtMs", 0LL);
  node.approved_at_ms = j.value("approvedAtMs", 0LL);
  if (j.contains("bins"))
    node.bins = j["bins"].get<std::vector<std::string>>();
  if (j.contains("lastConnectedAtMs"))
    node.last_connected_at_ms = j["lastConnectedAtMs"];
}

// ============================================================================
// State Loading & Persistence (node-pairing.ts:57-77)
// ============================================================================

void prune_expired_pending(
    std::unordered_map<std::string, NodePairingPendingRequest>& pending,
    int64_t now_ms) {
  std::vector<std::string> expired_ids;
  for (const auto& [id, req] : pending) {
    if (now_ms - req.ts > NODE_PAIRING_PENDING_TTL_MS) {
      expired_ids.push_back(id);
    }
  }
  for (const auto& id : expired_ids) {
    pending.erase(id);
  }
}

NodePairingStateFile load_state(const std::string& base_dir) {
  auto paths = resolve_pairing_paths(base_dir);
  NodePairingStateFile state;

  // Load pending
  try {
    std::ifstream pending_file(paths.pending_path);
    if (pending_file.is_open()) {
      json j;
      pending_file >> j;
      for (auto& [id, req_json] : j.items()) {
        NodePairingPendingRequest req;
        from_json(req_json, req);  // 使用我们的反序列化函数
        state.pending_by_id[id] = req;
      }
    }
  } catch (const std::exception&) {
    // Ignore load errors
  }

  // Load paired
  try {
    std::ifstream paired_file(paths.paired_path);
    if (paired_file.is_open()) {
      json j;
      paired_file >> j;
      for (auto& [id, node_json] : j.items()) {
        NodePairingPairedNode node;
        from_json(node_json, node);  // 使用我们的反序列化函数
        state.paired_by_node_id[id] = node;
      }
    }
  } catch (const std::exception&) {
    // Ignore load errors
  }

  // Prune expired
  prune_expired_pending(state.pending_by_id, infra::current_timestamp_ms());

  return state;
}

void persist_state(const NodePairingStateFile& state,
                   const std::string& base_dir) {
  auto paths = resolve_pairing_paths(base_dir);

  // Save pending
  try {
    json pending_json = json::object();
    for (const auto& [id, req] : state.pending_by_id) {
      json req_json;
      to_json(req_json, req);  // 使用我们的序列化函数
      pending_json[id] = req_json;
    }
    std::ofstream pending_file(paths.pending_path);
    pending_file << pending_json.dump(2);
  } catch (const std::exception&) {
    // Ignore save errors
  }

  // Save paired
  try {
    json paired_json = json::object();
    for (const auto& [id, node] : state.paired_by_node_id) {
      json node_json;
      to_json(node_json, node);  // 使用我们的序列化函数
      paired_json[id] = node_json;
    }
    std::ofstream paired_file(paths.paired_path);
    paired_file << paired_json.dump(2);
  } catch (const std::exception&) {
    // Ignore save errors
  }
}

// ============================================================================
// Utilities (node-pairing.ts:79-85)
// ============================================================================

std::string normalize_node_id(const std::string& node_id) {
  return infra::trim(node_id);
}

}  // namespace

// ============================================================================
// Public API Implementation
// ============================================================================

NodePairingList list_node_pairing(const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_node_pairing_mutex);
  auto state = load_state(base_dir);

  // Convert to sorted vectors (node-pairing.ts:88-93)
  std::vector<NodePairingPendingRequest> pending;
  for (const auto& [_, req] : state.pending_by_id) {
    pending.push_back(req);
  }
  std::sort(pending.begin(), pending.end(),
            [](const auto& a, const auto& b) { return b.ts < a.ts; });

  std::vector<NodePairingPairedNode> paired;
  for (const auto& [_, node] : state.paired_by_node_id) {
    paired.push_back(node);
  }
  std::sort(paired.begin(), paired.end(), [](const auto& a, const auto& b) {
    return b.approved_at_ms < a.approved_at_ms;
  });

  return {pending, paired};
}

std::optional<NodePairingPairedNode> get_paired_node(
    const std::string& node_id, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_node_pairing_mutex);
  auto state = load_state(base_dir);
  std::string normalized = normalize_node_id(node_id);
  auto it = state.paired_by_node_id.find(normalized);
  return it != state.paired_by_node_id.end() ? std::optional(it->second)
                                              : std::nullopt;
}

NodePairingRequestResult request_node_pairing(
    const NodePairingNodeMetadata& metadata, bool silent,
    const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_node_pairing_mutex);
  auto state = load_state(base_dir);

  std::string node_id = normalize_node_id(metadata.node_id);
  if (node_id.empty()) {
    throw std::runtime_error("nodeId required");
  }

  // Check if already pending (node-pairing.ts:119-122)
  for (const auto& [id, req] : state.pending_by_id) {
    if (req.node_id == node_id) {
      // Update timestamp but don't create new request
      state.pending_by_id[id].ts = infra::current_timestamp_ms();
      persist_state(state, base_dir);
      return {NodePairingRequestResult::Status::PENDING, req, false};
    }
  }

  // Check if already paired
  bool is_repair = state.paired_by_node_id.count(node_id) > 0;

  // Create new request (node-pairing.ts:123-140)
  NodePairingPendingRequest req;
  static_cast<NodePairingNodeMetadata&>(req) = metadata;
  req.request_id = infra::generate_uuid();
  req.node_id = node_id;
  req.silent = silent;
  req.is_repair = is_repair;
  req.ts = infra::current_timestamp_ms();

  state.pending_by_id[req.request_id] = req;
  persist_state(state, base_dir);

  return {NodePairingRequestResult::Status::PENDING, req, true};
}

std::optional<NodePairingApprovalResult> approve_node_pairing(
    const std::string& request_id, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_node_pairing_mutex);
  auto state = load_state(base_dir);

  auto pending_it = state.pending_by_id.find(request_id);
  if (pending_it == state.pending_by_id.end()) {
    return std::nullopt;
  }

  const auto& pending = pending_it->second;
  int64_t now = infra::current_timestamp_ms();

  // Get existing or create new (node-pairing.ts:158-175)
  auto existing_it = state.paired_by_node_id.find(pending.node_id);
  int64_t created_at_ms =
      existing_it != state.paired_by_node_id.end()
          ? existing_it->second.created_at_ms
          : now;

  NodePairingPairedNode node;
  static_cast<NodePairingNodeMetadata&>(node) = pending;
  node.token = generate_pairing_token();
  node.created_at_ms = created_at_ms;
  node.approved_at_ms = now;

  // Remove pending and add to paired (node-pairing.ts:177-180)
  state.pending_by_id.erase(request_id);
  state.paired_by_node_id[pending.node_id] = node;
  persist_state(state, base_dir);

  return NodePairingApprovalResult{request_id, node};
}

std::optional<NodePairingRejectionResult> reject_node_pairing(
    const std::string& request_id, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_node_pairing_mutex);
  auto state = load_state(base_dir);

  auto pending_it = state.pending_by_id.find(request_id);
  if (pending_it == state.pending_by_id.end()) {
    return std::nullopt;
  }

  std::string node_id = pending_it->second.node_id;
  state.pending_by_id.erase(request_id);
  persist_state(state, base_dir);

  return NodePairingRejectionResult{request_id, node_id};
}

NodePairingVerifyResult verify_node_token(const std::string& node_id,
                                           const std::string& token,
                                           const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_node_pairing_mutex);
  auto state = load_state(base_dir);

  std::string normalized = normalize_node_id(node_id);
  auto it = state.paired_by_node_id.find(normalized);
  if (it == state.paired_by_node_id.end()) {
    return {false, std::nullopt};
  }

  const auto& node = it->second;
  bool ok = verify_pairing_token(token, node.token);
  return {ok, ok ? std::optional(node) : std::nullopt};
}

void update_paired_node_metadata(const std::string& node_id,
                                  const NodePairingNodeMetadata& patch,
                                  const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_node_pairing_mutex);
  auto state = load_state(base_dir);

  std::string normalized = normalize_node_id(node_id);
  auto it = state.paired_by_node_id.find(normalized);
  if (it == state.paired_by_node_id.end()) {
    return;
  }

  // Merge patch (node-pairing.ts:230-245)
  auto& existing = it->second;
  if (patch.display_name) existing.display_name = patch.display_name;
  if (patch.platform) existing.platform = patch.platform;
  if (patch.version) existing.version = patch.version;
  if (patch.core_version) existing.core_version = patch.core_version;
  if (patch.ui_version) existing.ui_version = patch.ui_version;
  if (patch.device_family) existing.device_family = patch.device_family;
  if (patch.model_identifier)
    existing.model_identifier = patch.model_identifier;
  if (patch.remote_ip) existing.remote_ip = patch.remote_ip;
  if (!patch.caps.empty()) existing.caps = patch.caps;
  if (!patch.commands.empty()) existing.commands = patch.commands;
  if (!patch.permissions.empty()) existing.permissions = patch.permissions;

  persist_state(state, base_dir);
}

std::optional<NodePairingPairedNode> rename_paired_node(
    const std::string& node_id, const std::string& display_name,
    const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_node_pairing_mutex);
  auto state = load_state(base_dir);

  std::string normalized = normalize_node_id(node_id);
  auto it = state.paired_by_node_id.find(normalized);
  if (it == state.paired_by_node_id.end()) {
    return std::nullopt;
  }

  std::string trimmed = infra::trim(display_name);
  if (trimmed.empty()) {
    throw std::runtime_error("displayName required");
  }

  it->second.display_name = trimmed;
  persist_state(state, base_dir);
  return it->second;
}

bool unpair_node(const std::string& node_id, const std::string& base_dir) {
  std::lock_guard<std::mutex> lock(g_node_pairing_mutex);
  auto state = load_state(base_dir);

  std::string normalized = normalize_node_id(node_id);
  auto it = state.paired_by_node_id.find(normalized);
  if (it == state.paired_by_node_id.end()) {
    return false;
  }

  state.paired_by_node_id.erase(it);
  persist_state(state, base_dir);
  return true;
}

// ============================================================================
// Token Management (aligned with pairing-token.ts)
// ============================================================================

std::string generate_pairing_token() {
  // Generate 32-byte random token
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);

  std::vector<uint8_t> token_bytes(32);
  for (auto& byte : token_bytes) {
    byte = static_cast<uint8_t>(dis(gen));
  }

  return infra::base64_encode(token_bytes);
}

bool verify_pairing_token(const std::string& provided,
                           const std::string& expected) {
  // Constant-time comparison
  if (provided.size() != expected.size()) {
    return false;
  }

  int result = 0;
  for (size_t i = 0; i < provided.size(); ++i) {
    result |= provided[i] ^ expected[i];
  }

  return result == 0;
}

}  // namespace pairing
}  // namespace hunterclaw
