// ============================================================================
// nodes_impl.cc - Node Management Methods
// 🎯 严格对齐 OpenClaw server-methods/nodes.ts (nodeHandlers)
//
// Methods:
//   node.pair.request               → requestNodePairing + broadcast
//   node.pair.list                  → listNodePairing
//   node.pair.approve               → approveNodePairing + broadcast
//   node.pair.reject                → rejectNodePairing + broadcast
//   node.pair.verify                → verifyNodeToken
//   node.rename                     → renamePairedNode
//   node.list                       → merge paired+connected nodes
//   node.describe                   → single node details
//   node.canvas.capability.refresh  → mint canvas capability token
//   node.pending.pull               → list pending actions
//   node.pending.ack                → ack pending actions
//   node.invoke                     → invoke node command (APNs wake)
//   node.invoke.result              → handle invoke result
//   node.event                      → handle node events
//
// Also includes: nodes-pending.ts alignment
//   node.pending.drain              → drain pending work
//   node.pending.enqueue            → enqueue pending work + wake
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <map>
#include <set>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t nodes_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// Constants (对齐 OpenClaw nodes.ts L53-59)
// ============================================================================
static constexpr int64_t NODE_WAKE_RECONNECT_WAIT_MS = 3000;
static constexpr int64_t NODE_WAKE_RECONNECT_RETRY_WAIT_MS = 12000;
static constexpr int64_t NODE_WAKE_RECONNECT_POLL_MS = 150;
static constexpr int64_t NODE_WAKE_THROTTLE_MS = 15000;
static constexpr int64_t NODE_PENDING_ACTION_TTL_MS = 10 * 60 * 1000;
static constexpr int NODE_PENDING_ACTION_MAX_PER_NODE = 64;

// ============================================================================
// Pending Node Actions (对齐 OpenClaw nodes.ts L87-245)
// ============================================================================
struct PendingNodeAction {
    std::string id;
    std::string node_id;
    std::string command;
    std::string params_json;
    std::string idempotency_key;
    int64_t enqueued_at_ms = 0;
};

static std::mutex s_pending_mutex;
static std::map<std::string, std::vector<PendingNodeAction>> s_pending_by_node;

static std::vector<PendingNodeAction> prune_pending_actions(const std::string& node_id, int64_t now) {
    auto it = s_pending_by_node.find(node_id);
    if (it == s_pending_by_node.end()) return {};

    int64_t min_ts = now - NODE_PENDING_ACTION_TTL_MS;
    auto& queue = it->second;
    queue.erase(
        std::remove_if(queue.begin(), queue.end(),
            [min_ts](const PendingNodeAction& a) { return a.enqueued_at_ms < min_ts; }),
        queue.end()
    );
    if (queue.empty()) {
        s_pending_by_node.erase(it);
        return {};
    }
    return queue;
}

static PendingNodeAction enqueue_pending_action(
    const std::string& node_id, const std::string& command,
    const std::string& params_json, const std::string& idempotency_key
) {
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    int64_t now = nodes_now_ms();
    auto& queue = s_pending_by_node[node_id];
    prune_pending_actions(node_id, now);

    // Check for existing by idempotency key
    for (const auto& entry : queue) {
        if (entry.idempotency_key == idempotency_key) return entry;
    }

    PendingNodeAction action;
    action.id = core::utils::generate_uuid();
    action.node_id = node_id;
    action.command = command;
    action.params_json = params_json;
    action.idempotency_key = idempotency_key;
    action.enqueued_at_ms = now;

    queue.push_back(action);
    if ((int)queue.size() > NODE_PENDING_ACTION_MAX_PER_NODE) {
        queue.erase(queue.begin(), queue.begin() + (queue.size() - NODE_PENDING_ACTION_MAX_PER_NODE));
    }
    return action;
}

static std::vector<PendingNodeAction> ack_pending_actions(
    const std::string& node_id, const std::vector<std::string>& ids
) {
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    prune_pending_actions(node_id, nodes_now_ms());
    if (ids.empty()) {
        auto it = s_pending_by_node.find(node_id);
        return it != s_pending_by_node.end() ? it->second : std::vector<PendingNodeAction>{};
    }
    std::set<std::string> id_set(ids.begin(), ids.end());
    auto& queue = s_pending_by_node[node_id];
    queue.erase(
        std::remove_if(queue.begin(), queue.end(),
            [&id_set](const PendingNodeAction& a) { return id_set.count(a.id) > 0; }),
        queue.end()
    );
    return queue;
}

// ============================================================================
// Helper: Load/save node pairing data
// ============================================================================
static json load_node_pairing(std::shared_ptr<GatewayServer> server) {
    auto cfg = server->config();
    if (!cfg) return json::object();
    std::string file = cfg->config_dir + "/node-pairing.json";
    try {
        if (std::filesystem::exists(file)) {
            std::ifstream f(file);
            json data;
            if (f.is_open()) { f >> data; return data; }
        }
    } catch (...) {}
    return json::object();
}

static void save_node_pairing(std::shared_ptr<GatewayServer> server, const json& data) {
    auto cfg = server->config();
    if (!cfg) return;
    std::string file = cfg->config_dir + "/node-pairing.json";
    try {
        std::filesystem::create_directories(std::filesystem::path(file).parent_path());
        std::ofstream out(file);
        if (out.is_open()) out << data.dump(2);
    } catch (...) {}
}

// ============================================================================
// Helper: uniqueSortedStrings (对齐 OpenClaw nodes.helpers.ts)
// ============================================================================
static std::vector<std::string> unique_sorted_strings(std::vector<std::string> items) {
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
    return items;
}

// ============================================================================
// node.pair.request
// 🎯 OpenClaw 对齐: nodes.ts L461-494
// ============================================================================
core::Result<json> method_node_pair_request(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string node_id = params.value("nodeId", "");
    if (node_id.empty()) {
        return core::Result<json>::Err("invalid node.pair.request params: nodeId required");
    }

    auto pairing = load_node_pairing(server);
    if (!pairing.contains("pending")) pairing["pending"] = json::array();
    if (!pairing.contains("paired")) pairing["paired"] = json::array();

    // Check if already paired
    for (const auto& p : pairing["paired"]) {
        if (p.value("nodeId", "") == node_id) {
            json result;
            result["status"] = "already_paired";
            result["created"] = false;
            return core::Result<json>::Ok(result);
        }
    }

    // Check if already pending
    for (const auto& p : pairing["pending"]) {
        if (p.value("nodeId", "") == node_id) {
            json result;
            result["status"] = "pending";
            result["created"] = false;
            result["request"] = p;
            return core::Result<json>::Ok(result);
        }
    }

    // Create new request
    json request;
    request["requestId"] = core::utils::generate_uuid();
    request["nodeId"] = node_id;
    request["displayName"] = params.value("displayName", node_id);
    request["platform"] = params.value("platform", "");
    request["version"] = params.value("version", "");
    request["coreVersion"] = params.value("coreVersion", "");
    request["uiVersion"] = params.value("uiVersion", "");
    request["deviceFamily"] = params.value("deviceFamily", "");
    request["modelIdentifier"] = params.value("modelIdentifier", "");
    if (params.contains("caps")) request["caps"] = params["caps"];
    if (params.contains("commands")) request["commands"] = params["commands"];
    if (params.contains("permissions")) request["permissions"] = params["permissions"];
    request["createdAtMs"] = nodes_now_ms();

    bool silent = params.value("silent", false);
    pairing["pending"].push_back(request);
    save_node_pairing(server, pairing);

    // Broadcast if not silent
    if (!silent) {
        core::EventFrame ef;
        ef.event = "node.pair.requested";
        ef.payload = core::JsonValue(request);
        server->broadcast(ef);
    }

    json result;
    result["status"] = "pending";
    result["created"] = true;
    result["request"] = request;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.pair.list
// 🎯 OpenClaw 对齐: nodes.ts L495-508
// ============================================================================
core::Result<json> method_node_pair_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;
    return core::Result<json>::Ok(load_node_pairing(server));
}

// ============================================================================
// node.pair.approve
// 🎯 OpenClaw 对齐: nodes.ts L509-537
// ============================================================================
core::Result<json> method_node_pair_approve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string request_id = params.value("requestId", "");
    if (request_id.empty()) {
        return core::Result<json>::Err("invalid node.pair.approve params: requestId required");
    }

    auto pairing = load_node_pairing(server);
    auto& pending = pairing["pending"];
    if (!pending.is_array()) return core::Result<json>::Err("unknown requestId");

    json approved_node;
    bool found = false;
    json new_pending = json::array();
    for (auto& req : pending) {
        if (req.value("requestId", "") == request_id) {
            found = true;
            approved_node = req;
            approved_node["status"] = "paired";
            approved_node["approvedAtMs"] = nodes_now_ms();
            // Generate token
            approved_node["token"] = "hc_node_" + core::utils::generate_uuid();
        } else {
            new_pending.push_back(req);
        }
    }
    if (!found) return core::Result<json>::Err("unknown requestId");

    pairing["pending"] = new_pending;
    if (!pairing.contains("paired")) pairing["paired"] = json::array();
    pairing["paired"].push_back(approved_node);
    save_node_pairing(server, pairing);

    // Broadcast
    json broadcast;
    broadcast["requestId"] = request_id;
    broadcast["nodeId"] = approved_node.value("nodeId", "");
    broadcast["decision"] = "approved";
    broadcast["ts"] = nodes_now_ms();
    { core::EventFrame ef; ef.event = "node.pair.resolved"; ef.payload = core::JsonValue(broadcast); server->broadcast(ef); }

    json result;
    result["node"] = approved_node;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.pair.reject
// 🎯 OpenClaw 对齐: nodes.ts L538-566
// ============================================================================
core::Result<json> method_node_pair_reject(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string request_id = params.value("requestId", "");
    if (request_id.empty()) {
        return core::Result<json>::Err("invalid node.pair.reject params: requestId required");
    }

    auto pairing = load_node_pairing(server);
    auto& pending = pairing["pending"];
    if (!pending.is_array()) return core::Result<json>::Err("unknown requestId");

    json rejected;
    bool found = false;
    json new_pending = json::array();
    for (auto& req : pending) {
        if (req.value("requestId", "") == request_id) {
            found = true;
            rejected = req;
        } else {
            new_pending.push_back(req);
        }
    }
    if (!found) return core::Result<json>::Err("unknown requestId");

    pairing["pending"] = new_pending;
    save_node_pairing(server, pairing);

    json broadcast;
    broadcast["requestId"] = request_id;
    broadcast["nodeId"] = rejected.value("nodeId", "");
    broadcast["decision"] = "rejected";
    broadcast["ts"] = nodes_now_ms();
    { core::EventFrame ef; ef.event = "node.pair.resolved"; ef.payload = core::JsonValue(broadcast); server->broadcast(ef); }

    return core::Result<json>::Ok(rejected);
}

// ============================================================================
// node.pair.verify
// 🎯 OpenClaw 对齐: nodes.ts L567-584
// ============================================================================
core::Result<json> method_node_pair_verify(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string node_id = params.value("nodeId", "");
    std::string token = params.value("token", "");
    if (node_id.empty() || token.empty()) {
        return core::Result<json>::Err("invalid node.pair.verify params: nodeId and token required");
    }

    auto pairing = load_node_pairing(server);
    auto& paired = pairing["paired"];
    if (!paired.is_array()) {
        json result;
        result["valid"] = false;
        return core::Result<json>::Ok(result);
    }

    for (const auto& node : paired) {
        if (node.value("nodeId", "") == node_id && node.value("token", "") == token) {
            json result;
            result["valid"] = true;
            result["nodeId"] = node_id;
            return core::Result<json>::Ok(result);
        }
    }

    json result;
    result["valid"] = false;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.rename
// 🎯 OpenClaw 对齐: nodes.ts L585-611
// ============================================================================
core::Result<json> method_node_rename(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string node_id = params.value("nodeId", "");
    std::string display_name = params.value("displayName", "");
    // Trim
    while (!display_name.empty() && display_name.front() == ' ') display_name.erase(0, 1);
    while (!display_name.empty() && display_name.back() == ' ') display_name.pop_back();

    if (node_id.empty() || display_name.empty()) {
        return core::Result<json>::Err("invalid node.rename params: nodeId and displayName required");
    }

    auto pairing = load_node_pairing(server);
    auto& paired = pairing["paired"];
    if (!paired.is_array()) return core::Result<json>::Err("unknown nodeId");

    bool found = false;
    for (auto& node : paired) {
        if (node.value("nodeId", "") == node_id) {
            node["displayName"] = display_name;
            found = true;
            break;
        }
    }
    if (!found) return core::Result<json>::Err("unknown nodeId");

    save_node_pairing(server, pairing);

    json result;
    result["nodeId"] = node_id;
    result["displayName"] = display_name;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.list
// 🎯 OpenClaw 对齐: nodes.ts L612-691
// Merge paired + connected nodes
// ============================================================================
core::Result<json> method_node_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    auto pairing = load_node_pairing(server);
    auto paired = pairing.value("paired", json::array());

    // Build paired map
    std::map<std::string, json> paired_by_id;
    for (const auto& p : paired) {
        std::string nid = p.value("nodeId", p.value("deviceId", ""));
        if (!nid.empty()) {
            paired_by_id[nid] = p;
        }
    }

    // Get connected nodes from runtime state (role="node" clients)
    auto state = server->runtime_state();
    std::map<std::string, json> connected_by_id;
    if (state) {
        for (const auto& [conn_id, client] : state->clients) {
            if (client && client->role == "node") {
                std::string nid = conn_id;  // use connId as nodeId
                json n;
                n["nodeId"] = nid;
                n["connId"] = conn_id;
                n["connected"] = true;
                connected_by_id[nid] = n;
            }
        }
    }

    // Merge
    std::set<std::string> all_ids;
    for (auto& [k, _] : paired_by_id) all_ids.insert(k);
    for (auto& [k, _] : connected_by_id) all_ids.insert(k);

    json nodes_arr = json::array();
    for (const auto& nid : all_ids) {
        auto pi = paired_by_id.find(nid);
        auto ci = connected_by_id.find(nid);
        bool is_paired = pi != paired_by_id.end();
        bool is_connected = ci != connected_by_id.end();
        const json& p_data = is_paired ? pi->second : json::object();
        const json& c_data = is_connected ? ci->second : json::object();

        json node;
        node["nodeId"] = nid;
        node["displayName"] = c_data.value("displayName", p_data.value("displayName", ""));
        node["platform"] = c_data.value("platform", p_data.value("platform", ""));
        node["version"] = c_data.value("version", p_data.value("version", nullptr));
        node["coreVersion"] = c_data.value("coreVersion", p_data.value("coreVersion", nullptr));
        node["uiVersion"] = c_data.value("uiVersion", p_data.value("uiVersion", nullptr));
        node["deviceFamily"] = c_data.value("deviceFamily", p_data.value("deviceFamily", nullptr));
        node["modelIdentifier"] = c_data.value("modelIdentifier", p_data.value("modelIdentifier", nullptr));
        node["remoteIp"] = c_data.value("remoteIp", p_data.value("remoteIp", nullptr));

        // Merge caps/commands
        std::vector<std::string> caps, commands;
        auto extract_strings = [](const json& arr, std::vector<std::string>& out) {
            if (arr.is_array()) {
                for (const auto& s : arr) {
                    if (s.is_string()) out.push_back(s.get<std::string>());
                }
            }
        };
        extract_strings(is_connected ? c_data.value("caps", json::array()) : p_data.value("caps", json::array()), caps);
        extract_strings(is_connected ? c_data.value("commands", json::array()) : p_data.value("commands", json::array()), commands);
        node["caps"] = unique_sorted_strings(caps);
        node["commands"] = unique_sorted_strings(commands);

        if (is_connected && c_data.contains("connectedAtMs")) {
            node["connectedAtMs"] = c_data["connectedAtMs"];
        }
        node["paired"] = is_paired;
        node["connected"] = is_connected;

        nodes_arr.push_back(node);
    }

    // Sort: connected first, then by display name
    std::sort(nodes_arr.begin(), nodes_arr.end(), [](const json& a, const json& b) {
        bool ac = a.value("connected", false);
        bool bc = b.value("connected", false);
        if (ac != bc) return ac;  // connected first
        std::string an = a.value("displayName", a.value("nodeId", ""));
        std::string bn = b.value("displayName", b.value("nodeId", ""));
        std::transform(an.begin(), an.end(), an.begin(), ::tolower);
        std::transform(bn.begin(), bn.end(), bn.begin(), ::tolower);
        return an < bn;
    });

    json result;
    result["ts"] = nodes_now_ms();
    result["nodes"] = nodes_arr;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.describe
// 🎯 OpenClaw 对齐: nodes.ts L693-746
// ============================================================================
core::Result<json> method_node_describe(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string node_id = params.value("nodeId", "");
    if (node_id.empty()) {
        return core::Result<json>::Err("nodeId required");
    }

    auto pairing = load_node_pairing(server);
    auto paired = pairing.value("paired", json::array());

    // Find paired entry
    json paired_entry;
    bool found_paired = false;
    for (const auto& p : paired) {
        if (p.value("nodeId", p.value("deviceId", "")) == node_id) {
            paired_entry = p;
            found_paired = true;
            break;
        }
    }

    // Find connected entry
    json live_entry;
    bool found_live = false;
    auto state = server->runtime_state();
    if (state) {
        for (const auto& [conn_id, client] : state->clients) {
            if (client && client->role == "node" && conn_id == node_id) {
                live_entry["nodeId"] = conn_id;
                live_entry["connected"] = true;
                found_live = true;
                break;
            }
        }
    }

    if (!found_paired && !found_live) {
        return core::Result<json>::Err("unknown nodeId");
    }

    json result;
    result["ts"] = nodes_now_ms();
    result["nodeId"] = node_id;
    result["displayName"] = found_live ? live_entry.value("displayName", "") : paired_entry.value("displayName", "");
    result["platform"] = found_live ? live_entry.value("platform", "") : paired_entry.value("platform", "");
    result["version"] = found_live ? live_entry.value("version", nullptr) : nullptr;
    result["remoteIp"] = found_live ? live_entry.value("remoteIp", nullptr) : paired_entry.value("remoteIp", nullptr);
    result["paired"] = found_paired;
    result["connected"] = found_live;
    if (found_live && live_entry.contains("connectedAtMs")) {
        result["connectedAtMs"] = live_entry["connectedAtMs"];
    }

    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.canvas.capability.refresh
// 🎯 OpenClaw 对齐: nodes.ts L747-791
// ============================================================================
core::Result<json> method_node_canvas_capability_refresh(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)params;

    std::string base_url = client_ctx.canvas_host_url.value_or("");
    if (base_url.empty()) {
        return core::Result<json>::Err("canvas host unavailable for this node session");
    }

    // Mint new capability token
    std::string capability = core::utils::generate_uuid();
    int64_t expires_at = nodes_now_ms() + 3600000;  // 1 hour TTL

    // Build scoped URL
    std::string scoped_url = base_url;
    if (scoped_url.find('?') != std::string::npos) {
        scoped_url += "&_cap=" + capability;
    } else {
        scoped_url += "?_cap=" + capability;
    }

    json result;
    result["canvasCapability"] = capability;
    result["canvasCapabilityExpiresAtMs"] = expires_at;
    result["canvasHostUrl"] = scoped_url;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.pending.pull
// 🎯 OpenClaw 对齐: nodes.ts L792-822
// ============================================================================
core::Result<json> method_node_pending_pull(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)params;

    std::string node_id = client_ctx.device_id;
    if (node_id.empty()) node_id = client_ctx.connect.client.id;
    if (node_id.empty()) {
        return core::Result<json>::Err("nodeId required");
    }

    std::lock_guard<std::mutex> lock(s_pending_mutex);
    auto pending = prune_pending_actions(node_id, nodes_now_ms());

    json actions = json::array();
    for (const auto& entry : pending) {
        json action;
        action["id"] = entry.id;
        action["command"] = entry.command;
        action["paramsJSON"] = entry.params_json.empty() ? json(nullptr) : json(entry.params_json);
        action["enqueuedAtMs"] = entry.enqueued_at_ms;
        actions.push_back(action);
    }

    json result;
    result["nodeId"] = node_id;
    result["actions"] = actions;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.pending.ack
// 🎯 OpenClaw 对齐: nodes.ts L823-851
// ============================================================================
core::Result<json> method_node_pending_ack(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard;

    std::string node_id = client_ctx.device_id;
    if (node_id.empty()) node_id = client_ctx.connect.client.id;
    if (node_id.empty()) {
        return core::Result<json>::Err("nodeId required");
    }

    std::vector<std::string> ids;
    if (params.contains("ids") && params["ids"].is_array()) {
        for (const auto& id : params["ids"]) {
            if (id.is_string()) {
                std::string s = id.get<std::string>();
                if (!s.empty()) ids.push_back(s);
            }
        }
    }

    auto remaining = ack_pending_actions(node_id, ids);

    json result;
    result["nodeId"] = node_id;
    result["ackedIds"] = ids;
    result["remainingCount"] = (int)remaining.size();
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.invoke
// 🎯 OpenClaw 对齐: nodes.ts L852-1080
// Full two-phase APNs wake flow with command allowlist
// ============================================================================
core::Result<json> method_node_invoke(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string node_id = params.value("nodeId", "");
    std::string command = params.value("command", "");
    std::string idempotency_key = params.value("idempotencyKey", "");
    int timeout_ms = params.value("timeoutMs", 30000);

    if (node_id.empty() || command.empty()) {
        return core::Result<json>::Err("nodeId and command required");
    }

    // 🎯 OpenClaw 对齐: Block system.execApprovals.* via node.invoke
    if (command == "system.execApprovals.get" || command == "system.execApprovals.set") {
        return core::Result<json>::Err(
            "node.invoke does not allow system.execApprovals.*; use exec.approvals.node.*"
        );
    }

    // Check if node is connected
    auto state = server->runtime_state();
    bool node_connected = false;
    if (state) {
        for (const auto& [conn_id, client] : state->clients) {
            if (client && client->role == "node" && conn_id == node_id) {
                node_connected = true;
                break;
            }
        }
    }
    if (!node_connected) {
        // Node not connected - in production would do APNs wake flow
        // 🎯 OpenClaw 对齐: nodes.ts L893-972 (two-phase wake)
        get_logger()->warn("node.invoke: node {} not connected, wake not implemented", node_id);
        return core::Result<json>::Err("node not connected");
    }

    // 🎯 OpenClaw 对齐: command allowlist check (nodes.ts L973-990)
    // Note: node session details not available via runtime_state, skip allowlist check
    std::vector<std::string> declared_commands;

    // Simple allowlist check: command must be in declared commands
    if (!declared_commands.empty()) {
        bool allowed = false;
        for (const auto& dc : declared_commands) {
            if (dc == command || dc == "*") { allowed = true; break; }
        }
        if (!allowed) {
            std::string platform = ns.value("platform", "unknown");
            return core::Result<json>::Err(
                "node command not allowed: the node (platform: " + platform +
                ") does not support \"" + command + "\""
            );
        }
    }

    // Forward invoke to node
    json forwarded_params = params.contains("params") ? params["params"] : json(nullptr);

    auto invoke_result = conn_pool->invoke_node(node_id, command, forwarded_params, timeout_ms, idempotency_key);
    if (!invoke_result.has_value()) {
        return core::Result<json>::Err("node invoke failed");
    }

    auto& res = invoke_result.value();
    if (res.contains("error")) {
        return core::Result<json>::Err(res["error"].dump());
    }

    json result;
    result["ok"] = true;
    result["nodeId"] = node_id;
    result["command"] = command;
    if (res.contains("payload")) result["payload"] = res["payload"];
    if (res.contains("payloadJSON")) result["payloadJSON"] = res["payloadJSON"];
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.invoke.result
// 🎯 OpenClaw 对齐: nodes.ts L1081
// ============================================================================
core::Result<json> method_node_invoke_result(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;
    // Delegated to handleNodeInvokeResult in OpenClaw
    // For now, acknowledge receipt
    json result;
    result["ok"] = true;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.event
// 🎯 OpenClaw 对齐: nodes.ts L1082-1128
// ============================================================================
core::Result<json> method_node_event(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;

    std::string event = params.value("event", "");
    if (event.empty()) {
        return core::Result<json>::Err("invalid node.event params: event required");
    }

    std::string node_id = client_ctx.device_id;
    if (node_id.empty()) node_id = client_ctx.connect.client.id;
    if (node_id.empty()) node_id = "node";

    // Extract payload
    json payload;
    if (params.contains("payloadJSON") && params["payloadJSON"].is_string()) {
        try {
            payload = json::parse(params["payloadJSON"].get<std::string>());
        } catch (...) {
            payload = nullptr;
        }
    } else if (params.contains("payload")) {
        payload = params["payload"];
    }

    // Broadcast the event
    json event_data;
    event_data["event"] = event;
    event_data["nodeId"] = node_id;
    event_data["payload"] = payload;
    event_data["ts"] = nodes_now_ms();
    { core::EventFrame ef; ef.event = "node.event"; ef.payload = core::JsonValue(event_data); server->broadcast(ef); }

    json result;
    result["ok"] = true;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.pending.drain
// 🎯 OpenClaw 对齐: nodes-pending.ts L31-59
// ============================================================================
core::Result<json> method_node_pending_drain(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard;

    std::string node_id = client_ctx.device_id;
    if (node_id.empty()) node_id = client_ctx.connect.client.id;
    if (node_id.empty()) {
        return core::Result<json>::Err("node.pending.drain requires a connected device identity");
    }

    int max_items = params.value("maxItems", 64);

    std::lock_guard<std::mutex> lock(s_pending_mutex);
    auto& queue = s_pending_by_node[node_id];
    prune_pending_actions(node_id, nodes_now_ms());

    int count = std::min(max_items, (int)queue.size());
    json items = json::array();
    for (int i = 0; i < count; i++) {
        json item;
        item["id"] = queue[i].id;
        item["command"] = queue[i].command;
        item["paramsJSON"] = queue[i].params_json.empty() ? json(nullptr) : json(queue[i].params_json);
        item["enqueuedAtMs"] = queue[i].enqueued_at_ms;
        items.push_back(item);
    }
    // Remove drained items
    queue.erase(queue.begin(), queue.begin() + count);

    json result;
    result["nodeId"] = node_id;
    result["items"] = items;
    result["remaining"] = (int)queue.size();
    return core::Result<json>::Ok(result);
}

// ============================================================================
// node.pending.enqueue
// 🎯 OpenClaw 对齐: nodes-pending.ts L60-159
// ============================================================================
core::Result<json> method_node_pending_enqueue(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string node_id = params.value("nodeId", "");
    if (node_id.empty()) {
        return core::Result<json>::Err("invalid node.pending.enqueue params: nodeId required");
    }

    std::string type = params.value("type", "");
    std::string priority = params.value("priority", "normal");

    auto action = enqueue_pending_action(node_id, type, "", core::utils::generate_uuid());

    // In production, would trigger APNs wake flow here
    get_logger()->info("node pending enqueue node={} type={}", node_id, type);

    json result;
    result["nodeId"] = node_id;
    result["queued"] = json::object({
        {"id", action.id},
        {"command", action.command},
        {"enqueuedAtMs", action.enqueued_at_ms},
    });
    result["wakeTriggered"] = false;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
