#pragma once

#include "hunterclaw/gateway/protocol.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <memory>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <future>
#include <ixwebsocket/IXWebSocket.h>

namespace hunterclaw {
namespace gateway {

/// Gateway 客户端选项 (100% 对齐 OpenClaw)
struct GatewayClientOptions {
    std::string url = "ws://127.0.0.1:18789";
    
    // 连接配置
    int64_t connect_delay_ms = 0;
    int64_t tick_watch_min_interval_ms = 30000;
    
    // 认证
    std::string token;
    std::string device_token;
    std::string password;
    std::string instance_id;
    
    // 客户端标识 (100% 对齐 OpenClaw)
    std::string client_name = "hunterclaw";
    std::string client_display_name;
    std::string client_version = "1.0.0";
    std::string platform;
    std::string device_family;
    
    // 角色和权限
    std::string mode;  // "interactive" | "headless"
    std::string role;  // "admin" | "user" | "guest"
    std::vector<std::string> scopes;
    std::vector<std::string> caps;
    std::vector<std::string> commands;
    std::map<std::string, bool> permissions;
    
    // 环境
    std::string path_env;
    
    // 设备标识
    struct DeviceIdentity {
        std::string device_id;
        std::string device_name;
        std::string device_type;
    } device_identity;
    
    // 协议版本
    int min_protocol = 1;
    int max_protocol = 1;
    std::string tls_fingerprint;
    
    // 回调 (100% 对齐 OpenClaw)
    std::function<void(const EventFrame&)> on_event;
    std::function<void(const HelloOk&)> on_hello_ok;
    std::function<void(const std::string&)> on_connect_error;
    std::function<void(int code, const std::string& reason)> on_close;
    std::function<void(int64_t expected, int64_t received)> on_gap;
};

/// Pending RPC 请求
struct PendingRequest {
    std::string id;
    std::promise<nlohmann::json> promise;
    bool expect_final = false;  // 对于流式响应
    int64_t sent_at;
};

/// Gateway 客户端 (100% 对齐 OpenClaw)
class GatewayClient {
public:
    explicit GatewayClient(const GatewayClientOptions& options);
    ~GatewayClient();
    
    // 生命周期 (100% 对齐 OpenClaw)
    void start();
    void stop();
    bool is_connected() const;
    
    // RPC 调用 (100% 对齐 OpenClaw)
    template<typename T = nlohmann::json>
    core::Result<T> request(const std::string& method, 
                           const nlohmann::json& params = nlohmann::json::object());
    
    // 异步 RPC
    std::future<core::Result<nlohmann::json>> request_async(
        const std::string& method,
        const nlohmann::json& params = nlohmann::json::object()
    );
    
    // 流式 RPC (期望 accepted + final 响应)
    std::future<core::Result<nlohmann::json>> request_streaming(
        const std::string& method,
        const nlohmann::json& params = nlohmann::json::object()
    );
    
    // 连接信息
    std::optional<std::string> get_conn_id() const;
    std::optional<HelloOk> get_hello_ok() const;
    
    // 统计信息
    struct Statistics {
        int64_t connected_at;
        int64_t last_tick_at;
        int64_t reconnect_count;
        int64_t total_requests;
        int64_t total_events;
        int64_t pending_requests;
    };
    Statistics get_statistics() const;
    
private:
    // WebSocket 回调
    void on_message(const ix::WebSocketMessagePtr& msg);
    void on_open();
    void on_close(uint16_t code, const std::string& reason);
    void on_error(const std::string& error);
    
    // 消息处理
    void handle_message(const std::string& raw);
    void handle_event(const EventFrame& evt);
    void handle_response(const ResponseFrame& res);
    
    // 连接管理 (100% 对齐 OpenClaw)
    void schedule_reconnect();
    void send_connect();
    
    // 心跳监控 (100% 对齐 OpenClaw)
    void start_tick_watch();
    void stop_tick_watch();
    void check_tick_timeout();
    
    // 发送
    void send_frame(const nlohmann::json& frame);
    
    // 清理
    void flush_pending_errors(const std::string& error_message);
    
    GatewayClientOptions options_;
    
    std::unique_ptr<ix::WebSocket> ws_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> connected_{false};
    
    // 连接状态 (100% 对齐 OpenClaw)
    std::string conn_id_;
    std::optional<HelloOk> hello_ok_;
    std::string connect_nonce_;
    bool connect_sent_ = false;
    
    // 重连退避 (100% 对齐 OpenClaw)
    int64_t backoff_ms_ = 1000;
    static constexpr int64_t MAX_BACKOFF_MS = 30000;
    
    // 心跳 (100% 对齐 OpenClaw)
    std::optional<int64_t> last_tick_;
    int64_t tick_interval_ms_ = 30000;
    std::thread tick_watch_thread_;
    std::atomic<bool> tick_watch_running_{false};
    
    // Pending 请求
    mutable std::mutex pending_mutex_;
    std::map<std::string, std::shared_ptr<PendingRequest>> pending_;
    
    // 序列号追踪 (gap detection)
    std::optional<int64_t> last_seq_;
    
    // 统计
    std::atomic<int64_t> connected_at_{0};
    std::atomic<int64_t> reconnect_count_{0};
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> total_events_{0};
};

// Template 实现
template<typename T>
core::Result<T> GatewayClient::request(const std::string& method, 
                                       const nlohmann::json& params) {
    auto future = request_async(method, params);
    auto result = future.get();
    
    if (!result.is_ok()) {
        return core::Result<T>::err(result.error);
    }
    
    if constexpr (std::is_same_v<T, nlohmann::json>) {
        return core::Result<T>::Ok(result.value.value());
    } else {
        try {
            T value = result.value.value().get<T>();
            return core::Result<T>::Ok(std::move(value));
        } catch (const std::exception& e) {
            return core::Result<T>::err("Failed to parse response: " + std::string(e.what()));
        }
    }
}

/// Gateway 客户端工厂
core::Result<std::shared_ptr<GatewayClient>> create_gateway_client(
    const GatewayClientOptions& options
);

} // namespace gateway
} // namespace hunterclaw
