#pragma once

#include "hunterclaw/core/types.h"
#include <memory>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <map>

namespace hunterclaw {
namespace gateway {

/**
 * 🎯 100% 对齐 OpenClaw: WebSocket 心跳机制
 * 
 * 功能:
 * - 定期发送 Ping 帧
 * - 检测 Pong 响应
 * - 超时断开连接
 * - 半死连接检测 (Stale Socket)
 * 
 * OpenClaw 参考:
 * - src/gateway/channel-health-monitor.ts
 * - ui/src/ui/gateway.ts (客户端心跳)
 */

/**
 * 连接健康状态
 */
struct ConnectionHealth {
    std::string conn_id;
    int64_t last_connected_at_ms = 0;   // 最后连接时间
    int64_t last_event_at_ms = 0;        // 最后事件时间
    int64_t last_ping_sent_at_ms = 0;    // 最后 Ping 发送时间
    int64_t last_pong_received_at_ms = 0; // 最后 Pong 接收时间
    int reconnect_attempts = 0;           // 重连尝试次数
    std::string last_error;               // 最后错误信息
    
    bool healthy = true;                  // 总体健康状态
    std::string unhealthy_reason;         // 不健康原因
};

/**
 * 心跳配置
 * 
 * OpenClaw 对齐:
 * ```typescript
 * const DEFAULT_HEARTBEAT_INTERVAL_MS = 30_000;  // 30秒
 * const DEFAULT_PONG_TIMEOUT_MS = 10_000;         // 10秒
 * const DEFAULT_STALE_EVENT_THRESHOLD_MS = 5 * 60 * 1000;  // 5分钟
 * ```
 */
struct HeartbeatConfig {
    int64_t interval_ms = 30000;              // Ping 间隔 (默认 30秒)
    int64_t pong_timeout_ms = 10000;          // Pong 超时 (默认 10秒)
    int64_t stale_event_threshold_ms = 300000; // 事件过期阈值 (默认 5分钟)
    int64_t connection_grace_ms = 60000;      // 连接建立宽限期 (默认 1分钟)
    bool enabled = true;                       // 总开关
};

/**
 * 健康检查策略
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export function evaluateChannelHealth(
 *   status: ChannelHealthMetrics,
 *   policy: { staleEventThresholdMs: number; channelConnectGraceMs: number }
 * ): { healthy: boolean; reason?: string } {
 *   const now = Date.now();
 *   
 *   // 检测 stale socket（连接活着但无事件）
 *   if (status.lastConnectedAt && status.lastEventAt) {
 *     const staleDuration = now - status.lastEventAt;
 *     if (staleDuration > policy.staleEventThresholdMs) {
 *       return { healthy: false, reason: "stale-socket" };
 *     }
 *   }
 *   
 *   // 检测连接失败
 *   if (status.reconnectAttempts >= 10) {
 *     return { healthy: false, reason: "gave-up" };
 *   }
 *   
 *   return { healthy: true };
 * }
 * ```
 */
inline ConnectionHealth evaluate_connection_health(
    const ConnectionHealth& status,
    const HeartbeatConfig& config
) {
    ConnectionHealth result = status;
    auto now = std::chrono::system_clock::now();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    
    // 1. 检测 Pong 超时
    if (status.last_ping_sent_at_ms > 0) {
        int64_t pong_delay = now_ms - status.last_ping_sent_at_ms;
        if (pong_delay > config.pong_timeout_ms) {
            result.healthy = false;
            result.unhealthy_reason = "pong-timeout";
            return result;
        }
    }
    
    // 2. 检测 Stale Socket（连接活着但无事件）
    if (status.last_connected_at_ms > 0 && status.last_event_at_ms > 0) {
        // 跳过宽限期
        if (now_ms - status.last_connected_at_ms > config.connection_grace_ms) {
            int64_t stale_duration = now_ms - status.last_event_at_ms;
            if (stale_duration > config.stale_event_threshold_ms) {
                result.healthy = false;
                result.unhealthy_reason = "stale-socket";
                return result;
            }
        }
    }
    
    // 3. 检测重连失败
    if (status.reconnect_attempts >= 10) {
        result.healthy = false;
        result.unhealthy_reason = "gave-up";
        return result;
    }
    
    result.healthy = true;
    result.unhealthy_reason = "";
    return result;
}

/**
 * 心跳监控器
 * 
 * 职责:
 * - 定期发送 Ping
 * - 跟踪 Pong 响应
 * - 健康检查
 * - 超时断开
 */
class HeartbeatMonitor {
public:
    using PingSendCallback = std::function<void(const std::string& conn_id)>;
    using DisconnectCallback = std::function<void(const std::string& conn_id, const std::string& reason)>;
    
    explicit HeartbeatMonitor(const HeartbeatConfig& config = {})
        : config_(config) {}
    
    /**
     * 注册连接
     */
    void register_connection(const std::string& conn_id);
    
    /**
     * 注销连接
     */
    void unregister_connection(const std::string& conn_id);
    
    /**
     * 记录事件接收（重置 stale 计数）
     */
    void record_event(const std::string& conn_id);
    
    /**
     * 记录 Ping 发送
     */
    void record_ping_sent(const std::string& conn_id);
    
    /**
     * 记录 Pong 接收
     */
    void record_pong_received(const std::string& conn_id);
    
    /**
     * 获取连接健康状态
     */
    ConnectionHealth get_health(const std::string& conn_id) const;
    
    /**
     * 执行一次健康检查周期
     * 
     * 返回需要断开的连接列表 (conn_id, reason)
     */
    std::vector<std::pair<std::string, std::string>> check_and_ping();
    
    /**
     * 设置回调函数
     */
    void set_ping_callback(PingSendCallback callback) {
        ping_callback_ = callback;
    }
    
    void set_disconnect_callback(DisconnectCallback callback) {
        disconnect_callback_ = callback;
    }
    
    /**
     * 启动/停止监控器
     */
    void start();
    void stop();
    bool is_running() const { return running_; }
    
private:
    HeartbeatConfig config_;
    std::map<std::string, ConnectionHealth> connections_;
    mutable std::mutex mutex_;
    
    PingSendCallback ping_callback_;
    DisconnectCallback disconnect_callback_;
    
    bool running_ = false;
    std::thread monitor_thread_;
    
    void monitor_loop();
    int64_t now_ms() const;
};

} // namespace gateway
} // namespace hunterclaw
