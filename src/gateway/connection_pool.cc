#include "hunterclaw/gateway/connection_pool.h"
#include <spdlog/spdlog.h>
#include <algorithm>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ============================================================================
// Singleton
// ============================================================================

ConnectionPool& ConnectionPool::instance() {
    static ConnectionPool pool;
    return pool;
}

ConnectionPool::ConnectionPool() 
    : next_subscription_id_(1) {
}

ConnectionPool::~ConnectionPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.clear();
    subscriptions_.clear();
}

// ============================================================================
// Client Management
// ============================================================================

void ConnectionPool::add_client(std::shared_ptr<WsClient> client) {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.insert(client);
    
    auto logger = spdlog::get("console");
    if (logger) {
        logger->debug("[ConnectionPool] Added client: {} (total: {})", 
                     client->conn_id, clients_.size());
    }
}

void ConnectionPool::remove_client(const std::string& conn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::find_if(clients_.begin(), clients_.end(),
        [&conn_id](const auto& client) { return client->conn_id == conn_id; });
    
    if (it != clients_.end()) {
        clients_.erase(it);
        
        auto logger = spdlog::get("console");
        if (logger) {
            logger->debug("[ConnectionPool] Removed client: {} (remaining: {})", 
                         conn_id, clients_.size());
        }
    }
}

std::shared_ptr<WsClient> ConnectionPool::get_client(const std::string& conn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::find_if(clients_.begin(), clients_.end(),
        [&conn_id](const auto& client) { return client->conn_id == conn_id; });
    
    return (it != clients_.end()) ? *it : nullptr;
}

std::vector<std::shared_ptr<WsClient>> ConnectionPool::get_all_clients() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::shared_ptr<WsClient>>(clients_.begin(), clients_.end());
}

size_t ConnectionPool::client_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
}

// ============================================================================
// Broadcasting
// ============================================================================

void ConnectionPool::broadcast(
    const std::string& event,
    const json& payload,
    const BroadcastOptions& opts
) {
    broadcast_internal(event, payload, opts, nullptr, nullptr);
}

void ConnectionPool::broadcast_to(
    const std::string& event,
    const json& payload,
    const std::set<std::string>& conn_ids,
    const BroadcastOptions& opts
) {
    broadcast_internal(event, payload, opts, &conn_ids, nullptr);
}

void ConnectionPool::broadcast_if(
    const std::string& event,
    const json& payload,
    std::function<bool(const WsClient&)> filter,
    const BroadcastOptions& opts
) {
    broadcast_internal(event, payload, opts, nullptr, filter);
}

// ============================================================================
// Event Subscription
// ============================================================================

uint64_t ConnectionPool::subscribe(
    const std::string& event_pattern,
    EventCallback callback,
    void* owner
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint64_t sub_id = next_subscription_id_++;
    
    EventSubscription sub;
    sub.event_pattern = event_pattern;
    sub.callback = std::move(callback);
    sub.owner = owner;
    
    subscriptions_[sub_id] = std::move(sub);
    
    auto logger = spdlog::get("console");
    if (logger) {
        logger->debug("[ConnectionPool] Subscribed to event pattern: {} (sub_id: {})", 
                     event_pattern, sub_id);
    }
    
    return sub_id;
}

void ConnectionPool::unsubscribe(uint64_t subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = subscriptions_.find(subscription_id);
    if (it != subscriptions_.end()) {
        subscriptions_.erase(it);
        
        auto logger = spdlog::get("console");
        if (logger) {
            logger->debug("[ConnectionPool] Unsubscribed: {}", subscription_id);
        }
    }
}

void ConnectionPool::unsubscribe_by_owner(void* owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t removed = 0;
    for (auto it = subscriptions_.begin(); it != subscriptions_.end(); ) {
        if (it->second.owner == owner) {
            it = subscriptions_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    
    auto logger = spdlog::get("console");
    if (logger && removed > 0) {
        logger->debug("[ConnectionPool] Unsubscribed {} subscriptions by owner", removed);
    }
}

// ============================================================================
// Statistics
// ============================================================================

ConnectionPool::Statistics ConnectionPool::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return statistics_;
}

void ConnectionPool::reset_statistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    statistics_ = Statistics{};
}

// ============================================================================
// Internal Helpers
// ============================================================================

bool ConnectionPool::has_event_scope(const WsClient& client, const std::string& event) const {
    // 检查客户端是否有权接收该事件
    // TODO: 实现基于 scope 的访问控制
    return true;
}

bool ConnectionPool::match_event_pattern(const std::string& pattern, const std::string& event) const {
    // 简单的通配符匹配
    if (pattern == "*") return true;
    
    // TODO: 实现完整的模式匹配 (支持 agent.*, tool.* 等)
    return pattern == event;
}

std::string ConnectionPool::create_event_frame(
    const std::string& event,
    const json& payload,
    const BroadcastOptions& opts
) const {
    json frame;
    frame["jsonrpc"] = "2.0";
    frame["method"] = event;
    frame["params"] = payload;
    
    return frame.dump();
}

void ConnectionPool::broadcast_internal(
    const std::string& event,
    const json& payload,
    const BroadcastOptions& opts,
    const std::set<std::string>* target_conn_ids,
    std::function<bool(const WsClient&)> filter
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string frame = create_event_frame(event, payload, opts);
    size_t sent_count = 0;
    
    for (const auto& client : clients_) {
        // 检查目标过滤
        if (target_conn_ids && target_conn_ids->count(client->conn_id) == 0) {
            continue;
        }
        
        // 检查自定义过滤器
        if (filter && !filter(*client)) {
            continue;
        }
        
        // 检查 scope 权限
        if (!has_event_scope(*client, event)) {
            continue;
        }
        
        // TODO: 实际发送 WebSocket 消息
        // client->send(frame);
        
        ++sent_count;
    }
    
    // 更新统计
    statistics_.events_sent += sent_count;
    statistics_.events_by_type[event] += sent_count;
    
    auto logger = spdlog::get("console");
    if (logger) {
        logger->debug("[ConnectionPool] Broadcasted event '{}' to {} clients", 
                     event, sent_count);
    }
    
    // 触发订阅回调
    for (const auto& [sub_id, sub] : subscriptions_) {
        if (match_event_pattern(sub.event_pattern, event)) {
            try {
                sub.callback(payload);
            } catch (const std::exception& e) {
                if (logger) {
                    logger->error("[ConnectionPool] Subscription callback failed: {}", e.what());
                }
            }
        }
    }
}

} // namespace gateway
} // namespace hunterclaw
