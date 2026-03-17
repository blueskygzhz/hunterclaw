#include "hunterclaw/gateway/heartbeat_monitor.h"
#include "hunterclaw/core/utils.h"
#include <thread>
#include <mutex>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace gateway {

namespace {
// Logger
auto get_logger() {
    static auto logger = spdlog::get("gateway");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("gateway");
    }
    return logger;
}
} // anonymous namespace

int64_t HeartbeatMonitor::now_ms() const {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
}

void HeartbeatMonitor::register_connection(const std::string& conn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    ConnectionHealth health;
    health.conn_id = conn_id;
    health.last_connected_at_ms = now_ms();
    health.last_event_at_ms = now_ms();  // 连接建立也是一个事件
    health.reconnect_attempts = 0;
    health.healthy = true;
    
    connections_[conn_id] = health;
    
    get_logger()->debug("[HeartbeatMonitor] Registered connection: {}", conn_id);
}

void HeartbeatMonitor::unregister_connection(const std::string& conn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    connections_.erase(conn_id);
    
    get_logger()->debug("[HeartbeatMonitor] Unregistered connection: {}", conn_id);
}

void HeartbeatMonitor::record_event(const std::string& conn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = connections_.find(conn_id);
    if (it != connections_.end()) {
        it->second.last_event_at_ms = now_ms();
    }
}

void HeartbeatMonitor::record_ping_sent(const std::string& conn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = connections_.find(conn_id);
    if (it != connections_.end()) {
        it->second.last_ping_sent_at_ms = now_ms();
    }
}

void HeartbeatMonitor::record_pong_received(const std::string& conn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = connections_.find(conn_id);
    if (it != connections_.end()) {
        it->second.last_pong_received_at_ms = now_ms();
        // Pong 也是一个事件，重置 stale 计数
        it->second.last_event_at_ms = now_ms();
    }
}

ConnectionHealth HeartbeatMonitor::get_health(const std::string& conn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = connections_.find(conn_id);
    if (it != connections_.end()) {
        return evaluate_connection_health(it->second, config_);
    }
    
    // 未找到连接
    ConnectionHealth health;
    health.conn_id = conn_id;
    health.healthy = false;
    health.unhealthy_reason = "connection-not-found";
    return health;
}

std::vector<std::pair<std::string, std::string>> HeartbeatMonitor::check_and_ping() {
    std::vector<std::pair<std::string, std::string>> to_disconnect;
    std::vector<std::string> to_ping;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t now = now_ms();
        
        for (auto& [conn_id, status] : connections_) {
            // 1. 健康检查
            ConnectionHealth health = evaluate_connection_health(status, config_);
            
            if (!health.healthy) {
                to_disconnect.push_back({conn_id, health.unhealthy_reason});
                continue;
            }
            
            // 2. 检查是否需要发送 Ping
            int64_t time_since_last_ping = now - status.last_ping_sent_at_ms;
            if (time_since_last_ping >= config_.interval_ms) {
                to_ping.push_back(conn_id);
            }
        }
    }
    
    // 发送 Ping（锁外执行，避免死锁）
    if (ping_callback_) {
        for (const auto& conn_id : to_ping) {
            try {
                ping_callback_(conn_id);
                record_ping_sent(conn_id);
                
                get_logger()->debug("[HeartbeatMonitor] Sent ping to: {}", conn_id);
            } catch (const std::exception& e) {
                get_logger()->warn("[HeartbeatMonitor] Failed to send ping to {}: {}",
                                  conn_id, e.what());
            }
        }
    }
    
    // 断开不健康连接（锁外执行）
    if (disconnect_callback_) {
        for (const auto& [conn_id, reason] : to_disconnect) {
            try {
                disconnect_callback_(conn_id, reason);
                
                get_logger()->info("[HeartbeatMonitor] Disconnecting {} due to: {}",
                                  conn_id, reason);
            } catch (const std::exception& e) {
                get_logger()->warn("[HeartbeatMonitor] Failed to disconnect {}: {}",
                                  conn_id, e.what());
            }
        }
    }
    
    // 清理已断开的连接
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [conn_id, _] : to_disconnect) {
            connections_.erase(conn_id);
        }
    }
    
    return to_disconnect;
}

void HeartbeatMonitor::monitor_loop() {
    get_logger()->info("[HeartbeatMonitor] Monitor loop started");
    
    while (running_) {
        try {
            // 执行一次检查
            check_and_ping();
            
            // 休眠（Ping 间隔的一半，更快响应）
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.interval_ms / 2)
            );
        } catch (const std::exception& e) {
            get_logger()->error("[HeartbeatMonitor] Error in monitor loop: {}", e.what());
        }
    }
    
    get_logger()->info("[HeartbeatMonitor] Monitor loop stopped");
}

void HeartbeatMonitor::start() {
    if (!config_.enabled) {
        get_logger()->info("[HeartbeatMonitor] Disabled by config");
        return;
    }
    
    if (running_) {
        get_logger()->warn("[HeartbeatMonitor] Already running");
        return;
    }
    
    running_ = true;
    monitor_thread_ = std::thread(&HeartbeatMonitor::monitor_loop, this);
    
    get_logger()->info("[HeartbeatMonitor] Started (interval={}ms, timeout={}ms)",
                      config_.interval_ms, config_.pong_timeout_ms);
}

void HeartbeatMonitor::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    
    get_logger()->info("[HeartbeatMonitor] Stopped");
}

} // namespace gateway
} // namespace hunterclaw
