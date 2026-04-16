// 🎯 对齐 OpenClaw: gateway/node-registry.ts (完整 NodeRegistry 类)
// 核心: register/unregister/listConnected/get/invoke/handleInvokeResult/sendEvent

#include "hunterclaw/nodes/node_registry.h"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace nodes {

// 🎯 对齐 OpenClaw: gateway/node-registry.ts:40-43
NodeRegistry::NodeRegistry() = default;

// ============================================================================
// register — 对齐 OpenClaw: gateway/node-registry.ts:45-83
// ============================================================================

NodeSession NodeRegistry::register_node(
    const std::string& conn_id,
    const std::string& node_id,
    const std::string& client_id,
    const std::string& client_mode,
    const std::string& display_name,
    const std::string& platform,
    const std::string& version,
    const std::string& core_version,
    const std::string& ui_version,
    const std::string& device_family,
    const std::string& model_identifier,
    const std::string& remote_ip,
    const std::vector<std::string>& caps,
    const std::vector<std::string>& commands,
    const std::map<std::string, bool>& permissions,
    const std::string& path_env) {

    NodeSession session;
    session.node_id = node_id;
    session.conn_id = conn_id;
    session.client_id = client_id;
    session.client_mode = client_mode;
    session.display_name = display_name;
    session.platform = platform;
    session.version = version;
    session.core_version = core_version;
    session.ui_version = ui_version;
    session.device_family = device_family;
    session.model_identifier = model_identifier;
    session.remote_ip = remote_ip;
    session.caps = caps;
    session.commands = commands;
    session.permissions = permissions;
    session.path_env = path_env;
    session.connected_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(mutex_);
    // 🎯 对齐 OpenClaw: this.nodesById.set(nodeId, session)
    nodes_by_id_[node_id] = session;
    // 🎯 对齐 OpenClaw: this.nodesByConn.set(client.connId, nodeId)
    nodes_by_conn_[conn_id] = node_id;

    return session;
}

// ============================================================================
// unregister — 对齐 OpenClaw: gateway/node-registry.ts:85-101
// ============================================================================

std::optional<std::string> NodeRegistry::unregister(const std::string& conn_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto conn_it = nodes_by_conn_.find(conn_id);
    if (conn_it == nodes_by_conn_.end()) {
        return std::nullopt;
    }
    std::string node_id = conn_it->second;
    nodes_by_conn_.erase(conn_it);
    nodes_by_id_.erase(node_id);

    // 🎯 对齐 OpenClaw: 拒绝此 nodeId 所有 pending invokes
    // for (const [id, pending] of this.pendingInvokes.entries()) {
    //   if (pending.nodeId !== nodeId) continue;
    //   clearTimeout(pending.timer);
    //   pending.reject(new Error(`node disconnected (${pending.command})`));
    //   this.pendingInvokes.delete(id);
    // }
    auto it = pending_invokes_.begin();
    while (it != pending_invokes_.end()) {
        if (it->second.node_id == node_id) {
            if (it->second.reject) {
                it->second.reject("node disconnected (" + it->second.command + ")");
            }
            it = pending_invokes_.erase(it);
        } else {
            ++it;
        }
    }

    return node_id;
}

// ============================================================================
// listConnected — 对齐 OpenClaw: gateway/node-registry.ts:103-105
// ============================================================================

std::vector<NodeSession> NodeRegistry::list_connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeSession> result;
    result.reserve(nodes_by_id_.size());
    for (const auto& [id, session] : nodes_by_id_) {
        result.push_back(session);
    }
    return result;
}

// ============================================================================
// get — 对齐 OpenClaw: gateway/node-registry.ts:107-109
// ============================================================================

std::optional<NodeSession> NodeRegistry::get(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodes_by_id_.find(node_id);
    if (it == nodes_by_id_.end()) {
        return std::nullopt;
    }
    return it->second;
}

// ============================================================================
// invoke — 对齐 OpenClaw: gateway/node-registry.ts:111-159
// ============================================================================

NodeInvokeResult NodeRegistry::invoke(
    const std::string& node_id,
    const std::string& command,
    const std::string& params_json,
    int timeout_ms,
    const std::string& idempotency_key) {

    // 🎯 对齐 OpenClaw: const node = this.nodesById.get(params.nodeId)
    NodeSession node;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodes_by_id_.find(node_id);
        if (it == nodes_by_id_.end()) {
            // 🎯 对齐 OpenClaw: { ok: false, error: { code: "NOT_CONNECTED", message: "node not connected" } }
            NodeInvokeResult result;
            result.ok = false;
            result.error = NodeInvokeResult::Error{"NOT_CONNECTED", "node not connected"};
            return result;
        }
        node = it->second;
    }

    // 🎯 对齐 OpenClaw: 构建 node.invoke.request payload
    // const payload = { id: requestId, nodeId, command, paramsJSON, timeoutMs, idempotencyKey }
    std::string request_id = std::to_string(next_timer_id_++);  // 简化替代 randomUUID

    json payload;
    payload["id"] = request_id;
    payload["nodeId"] = node_id;
    payload["command"] = command;
    if (!params_json.empty()) {
        payload["paramsJSON"] = params_json;
    } else {
        payload["paramsJSON"] = nullptr;
    }
    if (timeout_ms > 0) {
        payload["timeoutMs"] = timeout_ms;
    }
    if (!idempotency_key.empty()) {
        payload["idempotencyKey"] = idempotency_key;
    }

    // 🎯 对齐 OpenClaw: const ok = this.sendEventToSession(node, "node.invoke.request", payload)
    bool sent = send_event_to_session(node, "node.invoke.request", payload.dump());
    if (!sent) {
        NodeInvokeResult result;
        result.ok = false;
        result.error = NodeInvokeResult::Error{"UNAVAILABLE", "failed to send invoke to node"};
        return result;
    }

    // 🎯 对齐 OpenClaw: return await new Promise<NodeInvokeResult>((resolve, reject) => {
    //   const timer = setTimeout(() => { ... resolve({ ok: false, error: { code: "TIMEOUT" } }) }, timeoutMs);
    //   this.pendingInvokes.set(requestId, { nodeId, command, resolve, reject, timer });
    // })
    // C++ 同步版本: 使用 condition_variable 等待
    std::mutex result_mutex;
    std::condition_variable result_cv;
    NodeInvokeResult invoke_result;
    bool result_ready = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        PendingInvoke pending;
        pending.request_id = request_id;
        pending.node_id = node_id;
        pending.command = command;
        pending.resolve = [&](NodeInvokeResult r) {
            std::lock_guard<std::mutex> lk(result_mutex);
            invoke_result = std::move(r);
            result_ready = true;
            result_cv.notify_one();
        };
        pending.reject = [&](const std::string& err) {
            std::lock_guard<std::mutex> lk(result_mutex);
            invoke_result.ok = false;
            invoke_result.error = NodeInvokeResult::Error{"REJECTED", err};
            result_ready = true;
            result_cv.notify_one();
        };
        pending_invokes_[request_id] = std::move(pending);
    }

    // 等待结果或超时
    int effective_timeout = (timeout_ms > 0) ? timeout_ms : 30000;
    {
        std::unique_lock<std::mutex> lk(result_mutex);
        bool ok = result_cv.wait_for(lk, std::chrono::milliseconds(effective_timeout),
                                     [&] { return result_ready; });
        if (!ok) {
            // 🎯 对齐 OpenClaw: 超时处理
            // this.pendingInvokes.delete(requestId);
            // resolve({ ok: false, error: { code: "TIMEOUT", message: "node invoke timed out" } })
            {
                std::lock_guard<std::mutex> lock2(mutex_);
                pending_invokes_.erase(request_id);
            }
            invoke_result.ok = false;
            invoke_result.error = NodeInvokeResult::Error{"TIMEOUT", "node invoke timed out"};
        }
    }

    return invoke_result;
}

// ============================================================================
// handleInvokeResult — 对齐 OpenClaw: gateway/node-registry.ts:161-185
// ============================================================================

bool NodeRegistry::handle_invoke_result(
    const std::string& request_id,
    const std::string& node_id,
    bool ok,
    const std::string& payload_json,
    const std::optional<NodeInvokeResult::Error>& error) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pending_invokes_.find(request_id);
    if (it == pending_invokes_.end()) {
        return false;
    }

    // 🎯 对齐 OpenClaw: if (pending.nodeId !== params.nodeId) return false
    if (it->second.node_id != node_id) {
        return false;
    }

    auto resolve_fn = std::move(it->second.resolve);
    pending_invokes_.erase(it);

    // 🎯 对齐 OpenClaw: pending.resolve({ ok, payload, payloadJSON, error })
    if (resolve_fn) {
        NodeInvokeResult result;
        result.ok = ok;
        result.payload_json = payload_json.empty() ? std::nullopt : std::optional<std::string>(payload_json);
        result.error = error;
        resolve_fn(std::move(result));
    }

    return true;
}

// ============================================================================
// sendEvent — 对齐 OpenClaw: gateway/node-registry.ts:187-193
// ============================================================================

bool NodeRegistry::send_event(const std::string& node_id, const std::string& event,
                              const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodes_by_id_.find(node_id);
    if (it == nodes_by_id_.end()) {
        return false;
    }
    return send_event_to_session(it->second, event, payload_json);
}

void NodeRegistry::set_send_event_fn(SendEventToSessionFn fn) {
    send_event_fn_ = std::move(fn);
}

// ============================================================================
// sendEventToSession — 对齐 OpenClaw: gateway/node-registry.ts:195-212
// ============================================================================

bool NodeRegistry::send_event_to_session(const NodeSession& session, const std::string& event,
                                          const std::string& payload_json) {
    if (!send_event_fn_) {
        return false;
    }
    try {
        return send_event_fn_(session, event, payload_json);
    } catch (...) {
        return false;
    }
}

} // namespace nodes
} // namespace hunterclaw
