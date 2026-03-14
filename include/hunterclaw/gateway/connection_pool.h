#pragma once

#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <functional>
#include <string>
#include <set>
#include <mutex>
#include <map>

namespace hunterclaw {
namespace gateway {

// Forward declarations
class WebSocketConnection;

// ============================================================================
// Broadcast Options
// ============================================================================

struct BroadcastOptions {
    /// Drop event if client's send buffer is full (prevent slow consumer blocking)
    bool drop_if_slow = false;
    
    /// State version for optimistic updates
    struct {
        uint64_t presence = 0;
        uint64_t health = 0;
    } state_version;
    
    /// Event sequence number (auto-generated if not set)
    uint64_t seq = 0;
};

// ============================================================================
// WebSocket Client
// ============================================================================

struct WsClient {
    /// Unique connection ID
    std::string conn_id;
    
    /// WebSocket connection
    std::shared_ptr<WebSocketConnection> socket;
    
    /// Client role (e.g., "operator", "agent", "channel")
    std::string role;
    
    /// Permission scopes
    std::vector<std::string> scopes;
    
    /// Connection metadata
    struct ConnectInfo {
        std::string session_key;
        std::string device_id;
        std::string client_version;
        std::string platform;
    } connect_info;
    
    /// Connection timestamp
    std::chrono::steady_clock::time_point connected_at;
    
    /// Last activity timestamp
    std::chrono::steady_clock::time_point last_activity;
};

// ============================================================================
// Event Subscription
// ============================================================================

using EventCallback = std::function<void(const nlohmann::json& payload)>;

struct EventSubscription {
    std::string event_pattern;  // e.g., "agent.*", "tool.result"
    EventCallback callback;
    void* owner;  // For unsubscribing by owner
};

// ============================================================================
// Connection Pool (Singleton)
// ============================================================================

/**
 * @brief Global WebSocket connection pool for direct broadcast
 * 
 * 🎯 OpenClaw 对齐核心组件:
 * 
 * - 维护所有 WebSocket 客户端连接
 * - 支持直接广播 (无需 Gateway 转发)
 * - 慢速消费者保护 (slow consumer detection)
 * - 事件订阅机制 (pub-sub pattern)
 * 
 * @example
 * ```cpp
 * // Agent 直接推送
 * ConnectionPool::instance().broadcast("agent", {
 *     {"type", "assistant_text"},
 *     {"sessionId", session_id},
 *     {"text", "Hello!"}
 * }, { .drop_if_slow = true });
 * 
 * // Channel 订阅事件
 * ConnectionPool::instance().subscribe("agent.*", [](const json& payload) {
 *     // Handle agent event
 * });
 * ```
 */
class ConnectionPool {
public:
    /// Get singleton instance
    static ConnectionPool& instance();
    
    // ========== Client Management ==========
    
    /// Add a client connection
    void add_client(std::shared_ptr<WsClient> client);
    
    /// Remove a client connection
    void remove_client(const std::string& conn_id);
    
    /// Get client by connection ID
    std::shared_ptr<WsClient> get_client(const std::string& conn_id) const;
    
    /// Get all active clients
    std::vector<std::shared_ptr<WsClient>> get_all_clients() const;
    
    /// Get client count
    size_t client_count() const;
    
    // ========== Broadcasting ==========
    
    /**
     * @brief Broadcast event to all connected clients
     * 
     * 🎯 OpenClaw 对齐: 直接发送,无需 Gateway 转发
     * 
     * @param event Event name (e.g., "agent", "memory.recall")
     * @param payload Event payload (JSON)
     * @param opts Broadcast options
     */
    void broadcast(
        const std::string& event,
        const nlohmann::json& payload,
        const BroadcastOptions& opts = {}
    );
    
    /**
     * @brief Broadcast event to specific clients
     * 
     * @param event Event name
     * @param payload Event payload
     * @param conn_ids Connection IDs to target
     * @param opts Broadcast options
     */
    void broadcast_to(
        const std::string& event,
        const nlohmann::json& payload,
        const std::set<std::string>& conn_ids,
        const BroadcastOptions& opts = {}
    );
    
    /**
     * @brief Broadcast event to clients matching filter
     * 
     * @param event Event name
     * @param payload Event payload
     * @param filter Filter function (return true to include client)
     * @param opts Broadcast options
     */
    void broadcast_if(
        const std::string& event,
        const nlohmann::json& payload,
        std::function<bool(const WsClient&)> filter,
        const BroadcastOptions& opts = {}
    );
    
    // ========== Event Subscription (Pub-Sub) ==========
    
    /**
     * @brief Subscribe to events
     * 
     * 🎯 OpenClaw 对齐: Channel 可订阅 Agent 事件
     * 
     * @param event_pattern Event pattern (supports wildcards)
     * @param callback Callback function
     * @param owner Owner pointer (for bulk unsubscribe)
     * @return Subscription ID
     */
    uint64_t subscribe(
        const std::string& event_pattern,
        EventCallback callback,
        void* owner = nullptr
    );
    
    /// Unsubscribe by subscription ID
    void unsubscribe(uint64_t subscription_id);
    
    /// Unsubscribe all by owner
    void unsubscribe_by_owner(void* owner);
    
    // ========== Statistics ==========
    
    struct Statistics {
        size_t total_clients = 0;
        size_t events_sent = 0;
        size_t events_dropped = 0;  // Slow consumers
        size_t slow_consumers_disconnected = 0;
        std::map<std::string, size_t> events_by_type;
    };
    
    Statistics get_statistics() const;
    void reset_statistics();
    
private:
    ConnectionPool() = default;
    ~ConnectionPool() = default;
    
    // Disable copy/move
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    
    // ========== Internal Helpers ==========
    
    /// Check if client has required scope for event
    bool has_event_scope(const WsClient& client, const std::string& event) const;
    
    /// Check if event pattern matches event name
    bool match_event_pattern(const std::string& pattern, const std::string& event) const;
    
    /// Create event frame (JSON-RPC 2.0 format)
    std::string create_event_frame(
        const std::string& event,
        const nlohmann::json& payload,
        const BroadcastOptions& opts
    ) const;
    
    /// Broadcast internal implementation
    void broadcast_internal(
        const std::string& event,
        const nlohmann::json& payload,
        const BroadcastOptions& opts,
        const std::set<std::string>* target_conn_ids = nullptr,
        std::function<bool(const WsClient&)> filter = nullptr
    );
    
    // ========== Data Members ==========
    
    mutable std::mutex mutex_;
    std::set<std::shared_ptr<WsClient>> clients_;
    
    // Event subscriptions
    std::map<uint64_t, EventSubscription> subscriptions_;
    uint64_t next_subscription_id_ = 1;
    
    // Statistics
    mutable Statistics statistics_;
    
    // Event sequence counter
    std::atomic<uint64_t> event_seq_{0};
    
    // Event scope guards (event -> required scopes)
    std::map<std::string, std::vector<std::string>> event_scope_guards_;
};

// ============================================================================
// Helper Functions
// ============================================================================

/// Create a slow consumer protection configuration
inline BroadcastOptions make_drop_if_slow() {
    BroadcastOptions opts;
    opts.drop_if_slow = true;
    return opts;
}

/// Broadcast to all clients in a session
void broadcast_to_session(
    const std::string& session_id,
    const std::string& event,
    const nlohmann::json& payload,
    const BroadcastOptions& opts = {}
);

/// Broadcast to all clients with a specific role
void broadcast_to_role(
    const std::string& role,
    const std::string& event,
    const nlohmann::json& payload,
    const BroadcastOptions& opts = {}
);

} // namespace gateway
} // namespace hunterclaw
