// 🎯 对齐 OpenClaw: gateway/server-node-subscriptions.ts (完整实现)
// Node 到 Session 的订阅管理

#include "hunterclaw/nodes/node_subscriptions.h"

namespace hunterclaw {
namespace nodes {

// ============================================================================
// subscribe (对齐 OpenClaw line 39-62)
// ============================================================================

void NodeSubscriptionManager::subscribe(const std::string& node_id, const std::string& session_key) {
    std::string nid = node_id;
    std::string sk = session_key;
    // trim
    while (!nid.empty() && nid.front() == ' ') nid.erase(nid.begin());
    while (!nid.empty() && nid.back() == ' ') nid.pop_back();
    while (!sk.empty() && sk.front() == ' ') sk.erase(sk.begin());
    while (!sk.empty() && sk.back() == ' ') sk.pop_back();
    if (nid.empty() || sk.empty()) return;

    // 🎯 对齐 OpenClaw: nodeSubscriptions → set<sessionKey>
    auto& node_set = node_subscriptions_[nid];
    if (node_set.count(sk)) return;
    node_set.insert(sk);

    // 🎯 对齐 OpenClaw: sessionSubscribers → set<nodeId>
    session_subscribers_[sk].insert(nid);
}

// ============================================================================
// unsubscribe (对齐 OpenClaw line 64-82)
// ============================================================================

void NodeSubscriptionManager::unsubscribe(const std::string& node_id, const std::string& session_key) {
    std::string nid = node_id;
    std::string sk = session_key;
    while (!nid.empty() && nid.front() == ' ') nid.erase(nid.begin());
    while (!nid.empty() && nid.back() == ' ') nid.pop_back();
    while (!sk.empty() && sk.front() == ' ') sk.erase(sk.begin());
    while (!sk.empty() && sk.back() == ' ') sk.pop_back();
    if (nid.empty() || sk.empty()) return;

    auto nit = node_subscriptions_.find(nid);
    if (nit != node_subscriptions_.end()) {
        nit->second.erase(sk);
        if (nit->second.empty()) node_subscriptions_.erase(nit);
    }

    auto sit = session_subscribers_.find(sk);
    if (sit != session_subscribers_.end()) {
        sit->second.erase(nid);
        if (sit->second.empty()) session_subscribers_.erase(sit);
    }
}

// ============================================================================
// unsubscribeAll (对齐 OpenClaw line 84-98)
// ============================================================================

void NodeSubscriptionManager::unsubscribe_all(const std::string& node_id) {
    std::string nid = node_id;
    while (!nid.empty() && nid.front() == ' ') nid.erase(nid.begin());
    while (!nid.empty() && nid.back() == ' ') nid.pop_back();

    auto nit = node_subscriptions_.find(nid);
    if (nit == node_subscriptions_.end()) return;

    // 🎯 对齐 OpenClaw: 清除此 node 在所有 session 中的订阅
    for (const auto& sk : nit->second) {
        auto sit = session_subscribers_.find(sk);
        if (sit != session_subscribers_.end()) {
            sit->second.erase(nid);
            if (sit->second.empty()) session_subscribers_.erase(sit);
        }
    }
    node_subscriptions_.erase(nit);
}

// ============================================================================
// sendToSession (对齐 OpenClaw line 100-119)
// ============================================================================

void NodeSubscriptionManager::send_to_session(
    const std::string& session_key, const std::string& event,
    const std::string& payload_json, NodeSendEventFn send_event) {

    std::string sk = session_key;
    while (!sk.empty() && sk.front() == ' ') sk.erase(sk.begin());
    while (!sk.empty() && sk.back() == ' ') sk.pop_back();
    if (sk.empty() || !send_event) return;

    auto sit = session_subscribers_.find(sk);
    if (sit == session_subscribers_.end() || sit->second.empty()) return;

    for (const auto& nid : sit->second) {
        send_event(nid, event, payload_json);
    }
}

// ============================================================================
// sendToAllSubscribed (对齐 OpenClaw line 121-133)
// ============================================================================

void NodeSubscriptionManager::send_to_all_subscribed(
    const std::string& event, const std::string& payload_json, NodeSendEventFn send_event) {

    if (!send_event) return;
    for (const auto& [nid, _] : node_subscriptions_) {
        send_event(nid, event, payload_json);
    }
}

// ============================================================================
// sendToAllConnected (对齐 OpenClaw line 135-148)
// ============================================================================

void NodeSubscriptionManager::send_to_all_connected(
    const std::string& event, const std::string& payload_json,
    const std::vector<std::string>& connected_node_ids,
    NodeSendEventFn send_event) {

    if (!send_event) return;
    for (const auto& nid : connected_node_ids) {
        send_event(nid, event, payload_json);
    }
}

// ============================================================================
// clear (对齐 OpenClaw line 150-153)
// ============================================================================

void NodeSubscriptionManager::clear() {
    node_subscriptions_.clear();
    session_subscribers_.clear();
}

} // namespace nodes
} // namespace hunterclaw
