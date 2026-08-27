#pragma once

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/protocol.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <functional>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>

namespace httplib {
    class Server;
    struct Request;
    struct Response;
}

namespace hunterclaw {

// Forward declarations
namespace llm { class LLMClient; }

namespace gateway {

// Forward declarations
class OpenAIAdapter;

namespace http {
    class OpenAICompat;
}

// ============================================================================
// HTTP Gateway Options
// ============================================================================

struct HttpGatewayOptions {
    int port = 18790;                      // HTTP 端口 (WebSocket + 1)
    std::string host = "127.0.0.1";        // 绑定地址
    bool enable_cors = true;                // CORS 支持
    bool enable_openai_api = true;          // OpenAI API 兼容层
    bool enable_metrics = true;             // Prometheus Metrics
    
    // CORS 配置
    struct CorsConfig {
        std::string allow_origin = "*";
        std::string allow_methods = "GET, POST, PUT, DELETE, OPTIONS";
        std::string allow_headers = "Content-Type, Authorization, x-api-key, x-hunterclaw-session-key, x-openclaw-session-key";
        int max_age = 3600;
    } cors;
    
    // 认证配置（继承 Gateway 认证）
    std::string bearer_token;
    
    // 超时配置
    int read_timeout_seconds = 30;
    int write_timeout_seconds = 30;
    
    // 请求大小限制
    size_t max_request_body_size = 10 * 1024 * 1024;  // 10MB
    
    // 🎯 对齐 OpenClaw: WebSocket upstream URL (用于 CWE-319 安全检查)
    std::string ws_upstream_url;
    
    // 🎯 对齐 OpenClaw: 允许 private network 的明文 ws:// (break-glass 开关)
    // 等价于 OPENCLAW_ALLOW_INSECURE_PRIVATE_WS=1
    bool allow_insecure_private_ws = false;
};

// ============================================================================
// Middleware System (中间件系统)
// ============================================================================

class HttpMiddleware {
public:
    using NextHandler = std::function<void(const httplib::Request&, httplib::Response&)>;
    
    virtual ~HttpMiddleware() = default;
    
    /// Execute middleware logic
    virtual void execute(
        const httplib::Request& req,
        httplib::Response& res,
        NextHandler next
    ) = 0;
};

/// Logging Middleware
class LoggingMiddleware : public HttpMiddleware {
public:
    void execute(const httplib::Request& req, httplib::Response& res, NextHandler next) override;
};

/// CORS Middleware
class CorsMiddleware : public HttpMiddleware {
public:
    explicit CorsMiddleware(const HttpGatewayOptions::CorsConfig& config);
    void execute(const httplib::Request& req, httplib::Response& res, NextHandler next) override;
    
private:
    HttpGatewayOptions::CorsConfig config_;
};

/// Authentication Middleware
class AuthMiddleware : public HttpMiddleware {
public:
    explicit AuthMiddleware(const std::string& bearer_token);
    void execute(const httplib::Request& req, httplib::Response& res, NextHandler next) override;

    /// 注册一个【绕过共享 bearer_token 校验】的路径前缀。
    ///
    /// ⚠ 只允许用于「自带一次性凭据」的端点。目前唯一用例是
    ///   remote_a2a 的回调（/remote-a2a/callback/<handle>）——
    ///   远端带的是我们发给该任务的一次性 token，不是 gateway 共享 token；
    ///   不旁路会让所有回调 401，表现为「回调配了但从不生效」。
    ///
    /// ★ 旁路只跳过本中间件，不等于无鉴权：被旁路的 handler
    ///   【必须】自己完成凭据校验 + 失败节流（见 callback_endpoint.h 三道闸门）。
    ///
    /// ⚠ 前缀必须以 '/' 开头且不得为 "/"（否则等于关闭全局鉴权）。
    ///   非法前缀会被静默忽略。
    void add_public_prefix(const std::string& prefix);

    /// 路径是否命中旁路前缀（単测与诊断用）
    bool is_public_path(const std::string& path) const;

private:
    std::string bearer_token_;
    std::vector<std::string> public_prefixes_;
};

/// Middleware Chain
class MiddlewareChain {
public:
    void use(std::shared_ptr<HttpMiddleware> middleware);
    
    void execute(
        const httplib::Request& req,
        httplib::Response& res,
        std::function<void(const httplib::Request&, httplib::Response&)> final_handler
    );
    
private:
    std::vector<std::shared_ptr<HttpMiddleware>> middlewares_;
};

// ============================================================================
// HTTP Gateway Server
// ============================================================================

class HttpGateway {
public:
    explicit HttpGateway(
        std::shared_ptr<GatewayServer> gateway_server,
        const HttpGatewayOptions& options = HttpGatewayOptions{}
    );
    ~HttpGateway();
    
    // Lifecycle
    core::Result<bool> start();
    core::Result<bool> stop();
    bool is_running() const;
    
    // Set LLM client for OpenAI API compatibility
    void set_llm_client(std::shared_ptr<llm::LLMClient> llm_client);
    
    // Route registration
    void register_route(
        const std::string& method,
        const std::string& path,
        std::function<void(const httplib::Request&, httplib::Response&)> handler
    );
    
    // Middleware registration
    void use_middleware(std::shared_ptr<HttpMiddleware> middleware);

    /// 注册一个绕过 AuthMiddleware 的路径前缀。
    /// ★ 见 AuthMiddleware::add_public_prefix 的约束说明。
    /// 未启用鉴权（bearer_token 为空）时本调用无副作用。
    void add_auth_bypass_prefix(const std::string& prefix);

private:
    // ========== REST API Endpoints (对齐 OpenClaw) ==========
    
    /// GET /health - Health check endpoint
    void handle_health(const httplib::Request& req, httplib::Response& res);
    
    /// GET /status - Server status endpoint
    void handle_status(const httplib::Request& req, httplib::Response& res);
    
    /// GET /metrics - Prometheus metrics endpoint
    void handle_metrics(const httplib::Request& req, httplib::Response& res);
    
    /// POST /api/v1/rpc - HTTP JSON-RPC endpoint
    void handle_rpc(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/sessions - List sessions
    void handle_list_sessions(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/sessions/:id - Get session
    void handle_get_session(const httplib::Request& req, httplib::Response& res);
    
    /// POST /api/v1/sessions - Create session
    void handle_create_session(const httplib::Request& req, httplib::Response& res);
    
    /// DELETE /api/v1/sessions/:id - Delete session
    void handle_delete_session(const httplib::Request& req, httplib::Response& res);
    
    /// PATCH /api/v1/sessions/:id - Update session (🎯 OpenClaw 对齐)
    void handle_update_session(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/channels - List channels
    void handle_list_channels(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/channels/:id - Get channel
    void handle_get_channel(const httplib::Request& req, httplib::Response& res);
    
    /// POST /api/v1/channels/:id/start - Start channel
    void handle_start_channel(const httplib::Request& req, httplib::Response& res);
    
    /// POST /api/v1/channels/:id/stop - Stop channel
    void handle_stop_channel(const httplib::Request& req, httplib::Response& res);
    
    // ========== Agent CRUD Endpoints (🎯 OpenClaw 对齐 P1) ==========
    
    /// POST /api/v1/agents - Create agent
    void handle_create_agent(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/agents - List agents
    void handle_list_agents(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/agents/:id - Get agent
    void handle_get_agent(const httplib::Request& req, httplib::Response& res);
    
    /// PATCH /api/v1/agents/:id - Update agent
    void handle_update_agent(const httplib::Request& req, httplib::Response& res);
    
    /// DELETE /api/v1/agents/:id - Delete agent
    void handle_delete_agent(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/skills - List skills
    void handle_list_skills(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/skills/:name - Get skill
    void handle_get_skill(const httplib::Request& req, httplib::Response& res);
    
    // ========== OpenAI API Compatible Endpoints ==========
    
    /// POST /v1/chat/completions - OpenAI chat completions (streaming supported)
    void handle_chat_completions(const httplib::Request& req, httplib::Response& res);
    
    /// GET /v1/models - List available models
    void handle_list_models(const httplib::Request& req, httplib::Response& res);
    
    // ========== Webhook Endpoints (🎯 OpenClaw 对齐) ==========
    
    /// POST /hooks/:mapping - Webhook hook endpoint
    void handle_webhook_hook(const httplib::Request& req, httplib::Response& res);

    // ========== ACP Server Endpoints (🎯 OpenClaw 对齐) ==========

    /// POST /acp/:method - ACP protocol handler (对齐 OC: serveAcpGateway)
    void handle_acp_request(const httplib::Request& req, httplib::Response& res);
    
    // ========== Helper Methods ==========
    
    /// Setup all routes
    void setup_routes();
    
    /// Send JSON response
    void send_json(httplib::Response& res, int status, const nlohmann::json& data);
    
    /// Send error response (simple format)
    void send_error(httplib::Response& res, int status, const std::string& error);
    
    /// Send OpenAI-compatible error response
    void send_openai_error(
        httplib::Response& res, 
        int status, 
        const std::string& message,
        const std::string& type = "server_error",
        const std::string& code = ""
    );
    
    /// Send SSE (Server-Sent Events) chunk
    void send_sse_chunk(httplib::Response& res, const std::string& data);
    
    /// Invoke RPC method through Gateway
    core::Result<nlohmann::json> invoke_rpc(
        const std::string& method,
        const nlohmann::json& params
    );
    
    /// 🎯 OpenClaw 对齐: 外部内容安全包装
    std::string wrap_external_content(
        const std::string& content,
        const std::string& source,
        const std::string& from,
        const std::string& channel
    );
    
    /// 🎯 OpenClaw 对齐: 生成唯一标记 ID
    std::string generate_marker_id();
    
    std::shared_ptr<GatewayServer> gateway_server_;
    HttpGatewayOptions options_;
    std::unique_ptr<httplib::Server> http_server_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    
    MiddlewareChain middleware_chain_;
    /// ★ 保留 AuthMiddleware 的引用以便注册旁路前缀。
    ///   为空表示未启用鉴权（此时旁路无意义）。
    std::shared_ptr<AuthMiddleware> auth_middleware_;
    
    // ✅ 对齐 OpenClaw: hookAuthLimiter (20 failures / 60s)
    mutable std::mutex auth_limiter_mutex_;
    std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>> auth_failure_records_;
    
    // OpenAI API compatibility layer (initialized when LLM client is set)
    std::shared_ptr<http::OpenAICompat> openai_compat_;
};

// ============================================================================
// Factory Functions
// ============================================================================

/// Create HTTP Gateway with default options
core::Result<std::shared_ptr<HttpGateway>> create_http_gateway(
    std::shared_ptr<GatewayServer> gateway_server,
    const HttpGatewayOptions& options = HttpGatewayOptions{}
);

} // namespace gateway
} // namespace hunterclaw
