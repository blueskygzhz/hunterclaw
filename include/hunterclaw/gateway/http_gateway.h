#pragma once

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/protocol.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <functional>
#include <string>
#include <map>

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
    
private:
    std::string bearer_token_;
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
    
    /// GET /api/v1/channels - List channels
    void handle_list_channels(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/channels/:id - Get channel
    void handle_get_channel(const httplib::Request& req, httplib::Response& res);
    
    /// POST /api/v1/channels/:id/start - Start channel
    void handle_start_channel(const httplib::Request& req, httplib::Response& res);
    
    /// POST /api/v1/channels/:id/stop - Stop channel
    void handle_stop_channel(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/skills - List skills
    void handle_list_skills(const httplib::Request& req, httplib::Response& res);
    
    /// GET /api/v1/skills/:name - Get skill
    void handle_get_skill(const httplib::Request& req, httplib::Response& res);
    
    // ========== OpenAI API Compatible Endpoints ==========
    
    /// POST /v1/chat/completions - OpenAI chat completions (streaming supported)
    void handle_chat_completions(const httplib::Request& req, httplib::Response& res);
    
    /// GET /v1/models - List available models
    void handle_list_models(const httplib::Request& req, httplib::Response& res);
    
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
    
    std::shared_ptr<GatewayServer> gateway_server_;
    HttpGatewayOptions options_;
    std::unique_ptr<httplib::Server> http_server_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    
    MiddlewareChain middleware_chain_;
    
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
