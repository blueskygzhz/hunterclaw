#include "hunterclaw/gateway/connection_pool.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <shared_mutex>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// ConnectionPool Implementation
// ============================================================================

ConnectionPool& ConnectionPool::instance() {
    static ConnectionPool pool;
    return pool;
}

ConnectionPool::ConnectionPool() 
    : next_subscription_id_(1),
      next_connection_id_(1) {
}

ConnectionPool::~ConnectionPool() {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    connections_.clear();
    subscriptions_.clear();
}

uint64_t ConnectionPool::register_connection(
    const std::string& session_id,
    const std::string& channel_id,
    SendCallback send_callback
) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    uint64_t conn_id = next_connection_id_++;
    
    Connection conn;
    conn.id = conn_id;
    conn.session_id = session_id;
    conn.channel_id = channel_id;
    conn.send_callback = std::move(send_callback);
    conn.connected_at = std::chrono::steady_clock::now();
    conn.last_sent_at = conn.connected_at;
    
    connections_[conn_id] = std::move(conn);
    
    // 更新索引
    session_connections_[session_id].insert(conn_id);
    if (!channel_id.empty()) {
        channel_connections_[channel_id].insert(conn_id);
    }
    
    spdlog::debug("[ConnectionPool] Registered connection {} (session: {}, channel: {})",
        conn_id, session_id, channel_id);
    
    return conn_id;
}

void ConnectionPool::unregister_connection(uint64_t conn_id) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    auto it = connections_.find(conn_id);
    if (it == connections_.end()) {
        return;
    }
    
    const auto& conn = it->second;
    
    // 移除索引
    session_connections_[conn.session_id].erase(conn_id);
    if (session_connections_[conn.session_id].empty()) {
        session_connections_.erase(conn.session_id);
    }
    
    if (!conn.channel_id.empty()) {
        channel_connections_[conn.channel_id].erase(conn_id);
        if (channel_connections_[conn.channel_id].empty()) {
            channel_connections_.erase(conn.channel_id);
        }
    }
    
    spdlog::debug("[ConnectionPool] Unregistered connection {} (session: {})",
        conn_id, conn.session_id);
    
    connections_.erase(it);
}

void ConnectionPool::broadcast(
    const std::string& event,
    const json& payload,
    const BroadcastOptions& options
) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto now = std::chrono::steady_clock::now();
    
    // 收集目标连接
    std::vector<uint64_t> target_conn_ids;
    
    if (!options.target_sessions.empty()) {
        // 仅发送到指定 session
        for (const auto& session_id : options.target_sessions) {
            auto it = session_connections_.find(session_id);
            if (it != session_connections_.end()) {
                for (uint64_t conn_id : it->second) {
                    target_conn_ids.push_back(conn_id);
                }
            }
        }
    } else if (!options.target_channels.empty()) {
        // 仅发送到指定 channel
        for (const auto& channel_id : options.target_channels) {
            auto it = channel_connections_.find(channel_id);
            if (it != channel_connections_.end()) {
                for (uint64_t conn_id : it->second) {
                    target_conn_ids.push_back(conn_id);
                }
            }
        }
    } else {
        // 发送到所有连接
        for (const auto& [conn_id, _] : connections_) {
            target_conn_ids.push_back(conn_id);
        }
    }
    
    // 发送消息
    size_t sent_count = 0;
    size_t dropped_count = 0;
    
    for (uint64_t conn_id : target_conn_ids) {
        auto conn_it = connections_.find(conn_id);
        if (conn_it == connections_.end()) {
            continue;
        }
        
        auto& conn = conn_it->second;
        
        // 慢速客户端检测
        if (options.drop_if_slow) {
            auto time_since_last_send = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - conn.last_sent_at
            ).count();
            
            if (time_since_last_send > options.slow_threshold_ms) {
                spdlog::warn("[ConnectionPool] Dropping message to slow connection {} "
                    "({}ms since last send)", conn_id, time_since_last_send);
                dropped_count++;
                continue;
            }
        }
        
        // 发送消息
        try {
            conn.send_callback(event, payload);
            conn.last_sent_at = now;
            conn.messages_sent++;
            sent_count++;
        } catch (const std::exception& e) {
            spdlog::error("[ConnectionPool] Failed to send to connection {}: {}",
                conn_id, e.what());
        }
    }
    
    spdlog::debug("[ConnectionPool] Broadcast '{}': sent={}, dropped={}, targets={}",
        event, sent_count, dropped_count, target_conn_ids.size());
    
    // 触发订阅者
    lock.unlock();
    notify_subscribers(event, payload);
}

uint64_t ConnectionPool::subscribe(
    const std::string& event_pattern,
    EventCallback callback,
    void* owner
) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    uint64_t sub_id = next_subscription_id_++;
    
    Subscription sub;
    sub.id = sub_id;
    sub.event_pattern = event_pattern;
    sub.callback = std::move(callback);
    sub.owner = owner;
    sub.created_at = std::chrono::steady_clock::now();
    
    subscriptions_[sub_id] = std::move(sub);
    
    spdlog::debug("[ConnectionPool] Subscribed to '{}' (id: {})", event_pattern, sub_id);
    
    return sub_id;
}

void ConnectionPool::unsubscribe(uint64_t subscription_id) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    auto it = subscriptions_.find(subscription_id);
    if (it != subscriptions_.end()) {
        auto logger = spdlog::get("connection_pool");
        if (logger) {
            logger->debug("[ConnectionPool] Unsubscribed (id: {})", subscription_id);
        }
        subscriptions_.erase(it);
    }
}

// TODO: 删除孤立的代码片段
/*
// bool ConnectionPool::match_event_pattern(
//     const std::string& event,
    const std::string& pattern
) const {
    // 简单的通配符匹配: "agent.*" 匹配 "agent.text", "agent.tool_call" 等
    if (pattern == "*") {
        return true;
    }
    
    if (pattern.back() == '*') {
        // 前缀匹配
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        return event.compare(0, prefix.size(), prefix) == 0;
    }
    
    // 精确匹配
    return event == pattern;
}
*/

} // namespace gateway
} // namespace hunterclaw
