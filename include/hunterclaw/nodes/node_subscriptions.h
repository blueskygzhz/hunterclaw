#pragma once

// 🎯 对齐 OpenClaw: gateway/server-node-subscriptions.ts
// Node 到 Session 的订阅管理器

#include <string>
#include <map>
#include <set>
#include <functional>
#include <vector>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// NodeSendEventFn (对齐 OpenClaw line 1-5)
// ============================================================================

using NodeSendEventFn = std::function<void(
    const std::string& node_id,
    const std::string& event,
    const std::string& payload_json)>;

// ============================================================================
// NodeSubscriptionManager (对齐 OpenClaw line 9-31)
// ============================================================================

class NodeSubscriptionManager {
public:
    NodeSubscriptionManager() = default;

    // 🎯 对齐 OpenClaw: subscribe(nodeId, sessionKey)
    void subscribe(const std::string& node_id, const std::string& session_key);

    // 🎯 对齐 OpenClaw: unsubscribe(nodeId, sessionKey)
    void unsubscribe(const std::string& node_id, const std::string& session_key);

    // 🎯 对齐 OpenClaw: unsubscribeAll(nodeId)
    void unsubscribe_all(const std::string& node_id);

    // 🎯 对齐 OpenClaw: sendToSession(sessionKey, event, payload, sendEvent?)
    void send_to_session(const std::string& session_key, const std::string& event,
                         const std::string& payload_json, NodeSendEventFn send_event);

    // 🎯 对齐 OpenClaw: sendToAllSubscribed(event, payload, sendEvent?)
    void send_to_all_subscribed(const std::string& event,
                                const std::string& payload_json, NodeSendEventFn send_event);

    // 🎯 对齐 OpenClaw: sendToAllConnected(event, payload, listConnected?, sendEvent?)
    void send_to_all_connected(
        const std::string& event, const std::string& payload_json,
        const std::vector<std::string>& connected_node_ids,
        NodeSendEventFn send_event);

    // 🎯 对齐 OpenClaw: clear()
    void clear();

private:
    std::map<std::string, std::set<std::string>> node_subscriptions_;    // nodeId → set<sessionKey>
    std::map<std::string, std::set<std::string>> session_subscribers_;   // sessionKey → set<nodeId>
};

} // namespace nodes
} // namespace hunterclaw
