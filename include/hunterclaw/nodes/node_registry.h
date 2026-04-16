#pragma once

// 🎯 对齐 OpenClaw: gateway/node-registry.ts (NodeRegistry class)
// 核心功能: register/unregister/listConnected/get/invoke/handleInvokeResult/sendEvent

#include "hunterclaw/nodes/node.h"
#include "hunterclaw/core/result.h"
#include <memory>
#include <functional>
#include <optional>
#include <mutex>
#include <atomic>
#include <map>
#include <string>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// PendingInvoke (对齐 OpenClaw gateway/node-registry.ts:25-31)
// ============================================================================

struct PendingInvoke {
    std::string request_id;
    std::string node_id;
    std::string command;
    std::function<void(NodeInvokeResult)> resolve;
    std::function<void(const std::string&)> reject;
    int timer_id = 0;
};

// ============================================================================
// SendEventFn — 向 Node 发送 WebSocket 事件的回调
// ============================================================================

using SendEventToSessionFn = std::function<bool(
    const NodeSession& session,
    const std::string& event,
    const std::string& payload_json)>;

// ============================================================================
// NodeRegistry (对齐 OpenClaw gateway/node-registry.ts:40-213)
// ============================================================================

class NodeRegistry {
public:
    NodeRegistry();
    ~NodeRegistry() = default;

    // 🎯 对齐 OpenClaw: register(client, opts) → NodeSession
    NodeSession register_node(
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
        const std::string& path_env);

    // 🎯 对齐 OpenClaw: unregister(connId) → string | null
    // 断开连接时清理，拒绝所有 pending invokes
    std::optional<std::string> unregister(const std::string& conn_id);

    // 🎯 对齐 OpenClaw: listConnected() → NodeSession[]
    std::vector<NodeSession> list_connected() const;

    // 🎯 对齐 OpenClaw: get(nodeId) → NodeSession | undefined
    std::optional<NodeSession> get(const std::string& node_id) const;

    // 🎯 对齐 OpenClaw: invoke(params) → Promise<NodeInvokeResult>
    // 发送 node.invoke.request 到 Node，等待结果（同步版本，带超时）
    NodeInvokeResult invoke(
        const std::string& node_id,
        const std::string& command,
        const std::string& params_json,
        int timeout_ms = 30000,
        const std::string& idempotency_key = "");

    // 🎯 对齐 OpenClaw: handleInvokeResult(params) → boolean
    bool handle_invoke_result(
        const std::string& request_id,
        const std::string& node_id,
        bool ok,
        const std::string& payload_json,
        const std::optional<NodeInvokeResult::Error>& error);

    // 🎯 对齐 OpenClaw: sendEvent(nodeId, event, payload) → boolean
    bool send_event(const std::string& node_id, const std::string& event,
                    const std::string& payload_json = "");

    // 设置发送事件到 WebSocket session 的回调
    void set_send_event_fn(SendEventToSessionFn fn);

private:
    bool send_event_to_session(const NodeSession& session, const std::string& event,
                               const std::string& payload_json);

    mutable std::mutex mutex_;
    std::map<std::string, NodeSession> nodes_by_id_;      // nodeId → NodeSession
    std::map<std::string, std::string> nodes_by_conn_;    // connId → nodeId
    std::map<std::string, PendingInvoke> pending_invokes_; // requestId → PendingInvoke
    SendEventToSessionFn send_event_fn_;

    std::atomic<int> next_timer_id_{0};
};

} // namespace nodes
} // namespace hunterclaw
