#pragma once

#include "hunterclaw/gateway/protocol.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <memory>
#include <functional>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <ixwebsocket/IXWebSocketServer.h>

namespace hunterclaw {

// Forward declarations
namespace channels { class ChannelManager; }
namespace skills { class SkillManager; }
namespace plugins { class PluginManager; }

namespace gateway {

/// Gateway 绑定模式 (100% 对齐 OpenClaw)
enum class GatewayBindMode {
    Loopback,  // 127.0.0.1 only
    Lan,       // 0.0.0.0 (LAN accessible)
    Tailnet,   // Tailscale funnel/serve
    Auto       // Auto-detect
};

/// Gateway 认证模式 (100% 对齐 OpenClaw)
enum class GatewayAuthMode {
    Token,     // Bearer token
    Password,  // Password auth
    Device,    // Device pairing
    None       // No auth (development only)
};

/// RPC 处理器签名
using RpcHandler = std::function<core::Result<nlohmann::json>(
    const std::string& conn_id,
    const nlohmann::json& params
)>;

/// 事件回调
using EventCallback = std::function<void(const EventFrame&)>;

/// 客户端连接信息 (100% 对齐 OpenClaw)
struct ClientConnection {
    std::string conn_id;           // UUID
    std::string device_id;
    std::string client_name;
    std::string client_version;
    std::string platform;
    int protocol_version;
    
    int64_t connected_at;
    int64_t last_activity_at;
    
    std::string remote_ip;
    int remote_port;
    
    // WebSocket 连接 (不序列化)
    std::shared_ptr<ix::WebSocket> ws;
    
    // 权限和角色
    std::vector<std::string> scopes;
    std::vector<std::string> caps;
    std::map<std::string, bool> permissions;
};

/// Gateway 认证配置 (100% 对齐 OpenClaw)
struct GatewayAuthConfig {
    GatewayAuthMode mode = GatewayAuthMode::Token;
    std::string token;
    std::string password;
    
    // 限流配置
    struct RateLimitConfig {
        int max_attempts = 20;
        int64_t window_ms = 60000;
        bool exempt_loopback = true;
    } rate_limit;
};

/// Tailscale 配置 (100% 对齐 OpenClaw)
struct GatewayTailscaleConfig {
    bool enabled = false;
    enum class Mode {
        Serve,  // tailscale serve (private)
        Funnel  // tailscale funnel (public)
    } mode = Mode::Serve;
    bool reset_on_exit = true;
};

/// Gateway 服务器选项 (100% 对齐 OpenClaw)
struct GatewayServerOptions {
    int port = 18789;
    GatewayBindMode bind = GatewayBindMode::Loopback;
    std::string host;  // 高级覆盖
    
    bool control_ui_enabled = true;
    bool open_ai_chat_completions_enabled = false;
    bool open_responses_enabled = false;
    
    GatewayAuthConfig auth;
    GatewayTailscaleConfig tailscale;
    
    // TLS 配置
    struct TlsConfig {
        bool enabled = false;
        std::string cert_path;
        std::string key_path;
    } tls;
    
    // 心跳配置
    struct HeartbeatConfig {
        int64_t tick_interval_ms = 30000;        // 30 秒
        int64_t tick_timeout_threshold_ms = 60000;  // 2x tick_interval
    } heartbeat;
    
    // Channel 健康检查
    int channel_health_check_minutes = 5;
};

/// Gateway 运行时状态
struct GatewayRuntimeState {
    std::atomic<int64_t> start_time{0};
    std::atomic<int64_t> next_seq{1};
    
    std::atomic<int> presence_version{0};
    std::atomic<int> health_version{0};
    
    std::mutex clients_mutex;
    std::map<std::string, ClientConnection> clients;  // conn_id -> connection
    
    std::mutex handlers_mutex;
    std::map<std::string, RpcHandler> rpc_handlers;
    
    // 统计信息
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> total_events{0};
    std::atomic<uint64_t> total_errors{0};
};

/// Gateway 服务器
class GatewayServer {
public:
    explicit GatewayServer(const GatewayServerOptions& options);
    ~GatewayServer();
    
    // 生命周期
    core::Result<bool> start();
    core::Result<bool> stop();
    bool is_running() const;
    
    // RPC 处理器注册
    void register_handler(const std::string& method, RpcHandler handler);
    void unregister_handler(const std::string& method);
    
    // 事件广播 (100% 对齐 OpenClaw)
    void broadcast(const std::string& event, const nlohmann::json& payload);
    void broadcast_to_conn_ids(
        const std::vector<std::string>& conn_ids,
        const std::string& event,
        const nlohmann::json& payload
    );
    
    // 可选: drop_if_slow (跳过缓冲区过大的慢客户端)
    void broadcast_with_options(
        const std::string& event,
        const nlohmann::json& payload,
        bool drop_if_slow = false,
        const std::vector<std::string>& conn_ids = {}
    );
    
    // 客户端管理
    std::vector<ClientConnection> list_clients() const;
    std::optional<ClientConnection> get_client(const std::string& conn_id) const;
    bool disconnect_client(const std::string& conn_id);
    
    // 统计信息
    struct Statistics {
        int64_t uptime_seconds;
        int active_connections;
        uint64_t total_requests;
        uint64_t total_events;
        uint64_t total_errors;
    };
    Statistics get_statistics() const;
    
    // 运行时状态访问
    std::shared_ptr<GatewayRuntimeState> runtime_state() const { return state_; }
    
private:
    // WebSocket 回调
    void on_client_connect(std::shared_ptr<ix::ConnectionState> conn_state,
                          ix::WebSocket& ws,
                          const ix::WebSocketMessagePtr& msg);
    
    void on_client_message(const std::string& conn_id,
                          const std::string& message);
    
    void on_client_disconnect(const std::string& conn_id, int code, const std::string& reason);
    
    // 消息处理
    void handle_request_frame(const std::string& conn_id, const RequestFrame& req);
    void handle_connect_request(const std::string& temp_conn_id, const RequestFrame& req);
    
    // 认证
    bool authenticate_connection(const std::string& device_token);
    void send_connect_challenge(const std::string& temp_conn_id);
    
    // 心跳
    void start_heartbeat();
    void stop_heartbeat();
    void send_tick();
    
    // 响应发送
    void send_response(const std::string& conn_id, const ResponseFrame& res);
    void send_event(const std::string& conn_id, const EventFrame& evt);
    
    GatewayServerOptions options_;
    std::shared_ptr<GatewayRuntimeState> state_;
    
    std::unique_ptr<ix::WebSocketServer> ws_server_;
    std::atomic<bool> running_{false};
    
    // 心跳定时器
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
    
    // 临时连接 (等待 connect 完成)
    std::mutex temp_connections_mutex_;
    std::map<std::string, std::shared_ptr<ix::WebSocket>> temp_connections_;
    
    // Challenge nonces
    std::mutex nonces_mutex_;
    std::map<std::string, std::string> pending_nonces_;  // temp_conn_id -> nonce
};

/// Gateway 服务器工厂
core::Result<std::shared_ptr<GatewayServer>> create_gateway_server(
    const GatewayServerOptions& options
);

} // namespace gateway
} // namespace hunterclaw
