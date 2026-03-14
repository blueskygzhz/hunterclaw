#ifndef HUNTERCLAW_GATEWAY_WEBSOCKET_SERVER_H
#define HUNTERCLAW_GATEWAY_WEBSOCKET_SERVER_H

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <set>
#include <mutex>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// WebSocket 客户端连接管理 (严格对齐 OpenClaw ws-connection.ts)
// ============================================================================

/**
 * @brief WebSocket 客户端状态
 * 
 * 对齐 OpenClaw:
 * - gateway/server/ws-connection.ts
 * - gateway/server/ws-types.ts
 */
struct WebSocketClient {
    std::string conn_id;           // 连接 UUID
    std::string session_key;       // 认证后的 session key
    std::string device_id;         // 设备 ID
    std::string node_id;           // 节点 ID (mobile, desktop, etc.)
    std::string remote_address;    // 远程 IP
    std::string user_agent;        // User-Agent
    std::string origin;            // Origin header
    bool authenticated{false};     // 是否已认证
    bool is_webchat{false};        // 是否为 Web Chat 客户端
    bool is_browser{false};        // 是否为浏览器客户端
    int64_t opened_at{0};          // 连接打开时间
    int64_t last_activity{0};      // 最后活动时间
    std::string handshake_state;   // "pending" | "connected" | "failed"
    
    // Rate limiting
    int request_count{0};          // 请求计数
    int64_t window_start{0};       // 时间窗口开始
    
    // Connection 指针 (实际类型由实现决定)
    void* ws_handle{nullptr};
};

// ============================================================================
// WebSocket 消息协议 (严格对齐 OpenClaw RPC Protocol)
// ============================================================================

/**
 * @brief WebSocket 请求帧格式
 * 
 * 对齐 OpenClaw Gateway RPC Protocol:
 * - gateway/server-methods/types.ts
 * - gateway/protocol/
 * 
 * 格式:
 * {
 *   "type": "request",
 *   "id": "<uuid>",
 *   "method": "chat",
 *   "params": {...}
 * }
 */
struct WebSocketRequest {
    std::string type;              // "request"
    std::string id;                // 请求 UUID
    std::string method;            // 方法名 (chat, agent, config, etc.)
    nlohmann::json params;         // 方法参数
    
    static WebSocketRequest from_json(const nlohmann::json& j);
    nlohmann::json to_json() const;
};

/**
 * @brief WebSocket 响应帧格式
 * 
 * 格式:
 * {
 *   "type": "response",
 *   "id": "<uuid>",
 *   "result": {...}
 * }
 * 或错误:
 * {
 *   "type": "error",
 *   "id": "<uuid>",
 *   "error": {
 *     "code": "method_not_found",
 *     "message": "...",
 *     "data": {...}
 *   }
 * }
 */
struct WebSocketResponse {
    std::string type;              // "response" | "error"
    std::string id;                // 请求 ID
    nlohmann::json result;         // 成功结果
    nlohmann::json error;          // 错误信息
    
    static WebSocketResponse success(const std::string& id, const nlohmann::json& result);
    static WebSocketResponse error(const std::string& id, const std::string& code, 
                                   const std::string& message, const nlohmann::json& data = {});
    nlohmann::json to_json() const;
};

/**
 * @brief WebSocket 事件帧格式
 * 
 * 格式:
 * {
 *   "type": "event",
 *   "event": "agent.text",
 *   "payload": {...}
 * }
 */
struct WebSocketEvent {
    std::string type{"event"};
    std::string event;             // 事件名
    nlohmann::json payload;        // 事件负载
    
    nlohmann::json to_json() const;
};

// ============================================================================
// WebSocket 方法处理器接口
// ============================================================================

/**
 * @brief 请求上下文
 */
struct RequestContext {
    std::shared_ptr<WebSocketClient> client;
    std::string request_id;
    std::string method;
    nlohmann::json params;
    int64_t start_time{0};
};

/**
 * @brief 方法处理器接口
 */
class MethodHandler {
public:
    virtual ~MethodHandler() = default;
    
    /**
     * @brief 处理 WebSocket RPC 请求
     * @param ctx 请求上下文
     * @return 响应结果 (JSON)
     */
    virtual nlohmann::json handle(const RequestContext& ctx) = 0;
    
    /**
     * @brief 获取方法名
     */
    virtual std::string method_name() const = 0;
};

// ============================================================================
// WebSocket Server 配置
// ============================================================================

struct WebSocketServerOptions {
    std::string host{"127.0.0.1"};       // 绑定地址
    int port{8080};                      // 监听端口
    bool enable_ssl{false};              // 启用 SSL/TLS
    std::string ssl_cert_path;           // SSL 证书路径
    std::string ssl_key_path;            // SSL 私钥路径
    
    // 认证配置
    std::string bearer_token;            // Bearer Token
    bool require_auth{true};             // 是否要求认证
    
    // 超时配置
    int handshake_timeout_ms{10000};     // 握手超时 (10s)
    int ping_interval_ms{30000};         // Ping 间隔 (30s)
    int idle_timeout_ms{300000};         // 空闲超时 (5min)
    
    // Rate Limiting
    bool enable_rate_limit{true};
    int rate_limit_requests{100};        // 每窗口请求数
    int rate_limit_window_ms{60000};     // 时间窗口 (1min)
    
    // 其他配置
    int max_payload_size{10 * 1024 * 1024}; // 最大负载 10MB
    bool log_connections{true};
    bool log_requests{false};            // 是否记录所有请求 (调试用)
};

// ============================================================================
// WebSocket Server 主类
// ============================================================================

/**
 * @brief WebSocket 服务器
 * 
 * 严格对齐 OpenClaw Gateway:
 * - gateway/server.impl.ts
 * - gateway/server-ws-runtime.ts
 * - gateway/server/ws-connection.ts
 * 
 * 核心功能:
 * 1. WebSocket 连接管理
 * 2. RPC 协议处理
 * 3. 方法路由和分发
 * 4. 认证和授权
 * 5. Rate Limiting
 * 6. 广播和事件推送
 */
class WebSocketServer {
public:
    explicit WebSocketServer(const WebSocketServerOptions& options);
    ~WebSocketServer();
    
    // 禁止拷贝
    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;
    
    /**
     * @brief 启动服务器
     */
    bool start();
    
    /**
     * @brief 停止服务器
     * @param timeout_ms 优雅关闭超时 (毫秒)
     */
    void stop(int timeout_ms = 5000);
    
    /**
     * @brief 是否正在运行
     */
    bool is_running() const;
    
    // ========================================================================
    // 方法注册 (对齐 OpenClaw server-methods.ts)
    // ========================================================================
    
    /**
     * @brief 注册 RPC 方法处理器
     * @param handler 方法处理器 (必须继承 MethodHandler)
     */
    void register_method(std::shared_ptr<MethodHandler> handler);
    
    /**
     * @brief 注销方法
     */
    void unregister_method(const std::string& method);
    
    /**
     * @brief 获取所有注册的方法名
     */
    std::vector<std::string> list_methods() const;
    
    // ========================================================================
    // 广播和事件 (对齐 OpenClaw server-broadcast.ts)
    // ========================================================================
    
    /**
     * @brief 向所有已认证客户端广播事件
     * @param event 事件名
     * @param payload 事件负载
     * @param drop_if_slow 慢客户端是否丢弃 (防止阻塞)
     */
    void broadcast_event(const std::string& event, const nlohmann::json& payload, 
                        bool drop_if_slow = false);
    
    /**
     * @brief 向指定客户端发送事件
     * @param conn_id 连接 ID
     * @param event 事件名
     * @param payload 事件负载
     */
    bool send_event(const std::string& conn_id, const std::string& event, 
                    const nlohmann::json& payload);
    
    /**
     * @brief 向指定 session 发送事件
     * @param session_key Session Key
     * @param event 事件名
     * @param payload 事件负载
     */
    void send_to_session(const std::string& session_key, const std::string& event, 
                        const nlohmann::json& payload);
    
    // ========================================================================
    // 连接管理
    // ========================================================================
    
    /**
     * @brief 获取所有连接的客户端
     */
    std::vector<std::shared_ptr<WebSocketClient>> get_clients() const;
    
    /**
     * @brief 获取已认证客户端数量
     */
    size_t get_authenticated_count() const;
    
    /**
     * @brief 获取客户端信息
     */
    std::shared_ptr<WebSocketClient> get_client(const std::string& conn_id) const;
    
    /**
     * @brief 断开客户端连接
     * @param conn_id 连接 ID
     * @param code WebSocket 关闭码 (1000 = 正常关闭)
     * @param reason 关闭原因
     */
    void disconnect_client(const std::string& conn_id, int code = 1000, 
                          const std::string& reason = "");
    
    // ========================================================================
    // 统计信息
    // ========================================================================
    
    struct Stats {
        size_t total_connections{0};
        size_t active_connections{0};
        size_t authenticated_connections{0};
        uint64_t total_requests{0};
        uint64_t total_events{0};
        uint64_t total_errors{0};
        int64_t uptime_ms{0};
    };
    
    Stats get_stats() const;
    
private:
    // 内部实现 (pImpl idiom)
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gateway
} // namespace hunterclaw

#endif // HUNTERCLAW_GATEWAY_WEBSOCKET_SERVER_H
