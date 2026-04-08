#include "hunterclaw/gateway/http_gateway.h"
#include "hunterclaw/gateway/http/openai_compat.h"
#include "hunterclaw/gateway/session_device_id.h"  // ✅ Device ID 工具
#include "hunterclaw/security/timing_safe.h"       // 🎯 OpenClaw 对齐: 常量时间比较
#include "hunterclaw/core/utils.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>                                   // 🎯 时间戳
#include <iomanip>                                  // 🎯 格式化输出
#include <sstream>                                  // 🎯 字符串流
#include <algorithm>                                // 🎯 std::transform

using json = nlohmann::json;

// ✅ Import OpenAI compat types
using hunterclaw::gateway::http::ChatCompletionRequest;
using hunterclaw::gateway::http::ChatCompletionChunk;

// Helper for logging - use a simple no-op for now if logger not available
namespace {
    inline void log_info(const std::string& msg) { 
        auto logger = spdlog::get("console");
        if (logger) logger->info(msg);
    }
    template<typename... Args>
    inline void log_info(const char* fmt, Args&&... args) {
        auto logger = spdlog::get("console");
        if (logger) logger->info(fmt, std::forward<Args>(args)...);
    }
    inline void log_error(const std::string& msg) {
        auto logger = spdlog::get("console");
        if (logger) logger->error(msg);
    }
    template<typename... Args>
    inline void log_error(const char* fmt, Args&&... args) {
        auto logger = spdlog::get("console");
        if (logger) logger->error(fmt, std::forward<Args>(args)...);
    }
    inline void log_debug(const std::string& msg) {
        auto logger = spdlog::get("console");
        if (logger) logger->debug(msg);
    }
    template<typename... Args>
    inline void log_debug(const char* fmt, Args&&... args) {
        auto logger = spdlog::get("console");
        if (logger) logger->debug(fmt, std::forward<Args>(args)...);
    }
}

namespace hunterclaw {
namespace gateway {

// ============================================================================
// 🎯 对齐 OpenClaw net.ts: isSecureWebSocketUrl (CWE-319)
// ============================================================================

/**
 * 检查主机名是否为 loopback 地址
 * 对齐 OpenClaw: isLoopbackHost
 */
static bool is_loopback_host(const std::string& host) {
    if (host.empty()) return false;
    std::string h = host;
    // 小写化
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    if (h == "localhost") return true;
    if (h == "127.0.0.1") return true;
    if (h == "::1") return true;
    if (h == "[::1]") return true;
    // IPv4-mapped IPv6 loopback
    if (h == "::ffff:127.0.0.1") return true;
    // 127.x.x.x
    if (h.size() >= 4 && h.substr(0, 4) == "127.") return true;
    return false;
}

/**
 * 检查主机名是否为 RFC1918 / link-local / loopback 私有地址
 * 对齐 OpenClaw: isPrivateOrLoopbackHost
 */
static bool is_private_or_loopback_host(const std::string& host) {
    if (is_loopback_host(host)) return true;
    // 简化: IPv4 private ranges
    if (host.size() >= 3 && host.substr(0, 3) == "10.") return true;
    if (host.size() >= 8 && host.substr(0, 8) == "192.168.") return true;
    if (host.size() >= 4 && host.substr(0, 4) == "172.") {
        // 172.16.0.0 - 172.31.255.255
        try {
            auto dot = host.find('.', 4);
            if (dot != std::string::npos) {
                int second_octet = std::stoi(host.substr(4, dot - 4));
                if (second_octet >= 16 && second_octet <= 31) return true;
            }
        } catch (...) {}
    }
    // CGNAT 100.64/10
    if (host.size() >= 4 && host.substr(0, 4) == "100.") {
        try {
            auto dot = host.find('.', 4);
            if (dot != std::string::npos) {
                int second_octet = std::stoi(host.substr(4, dot - 4));
                if (second_octet >= 64 && second_octet <= 127) return true;
            }
        } catch (...) {}
    }
    return false;
}

/**
 * 🎯 严格对齐 OpenClaw net.ts: isSecureWebSocketUrl
 * 
 * CWE-319: Cleartext Transmission of Sensitive Information
 * 
 * - wss:// (TLS) 总是安全
 * - ws:// 仅 loopback 地址安全
 * - 可选: 允许 private network 的 ws://
 * 
 * @param url WebSocket URL
 * @param allow_private_ws 是否允许 private network 的 ws://
 * @return true 如果 URL 安全
 */
static bool is_secure_websocket_url(
    const std::string& url,
    bool allow_private_ws = false
) {
    // 解析 scheme
    std::string scheme;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    scheme = url.substr(0, scheme_end);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);
    
    // 🎯 对齐 OpenClaw: http(s) → ws(s) 归一化
    if (scheme == "https") scheme = "wss";
    else if (scheme == "http") scheme = "ws";
    
    if (scheme == "wss") return true;
    if (scheme != "ws") return false;
    
    // 提取 hostname
    std::string host_part = url.substr(scheme_end + 3);
    // 移除 path/query
    auto slash_pos = host_part.find('/');
    if (slash_pos != std::string::npos) host_part = host_part.substr(0, slash_pos);
    // 移除 port
    auto colon_pos = host_part.rfind(':');
    if (colon_pos != std::string::npos && host_part.find('[') == std::string::npos) {
        host_part = host_part.substr(0, colon_pos);
    }
    // 处理 bracketed IPv6
    if (host_part.front() == '[' && host_part.back() == ']') {
        host_part = host_part.substr(1, host_part.size() - 2);
    }
    
    // 默认策略: 仅 loopback 的 ws:// 安全
    if (is_loopback_host(host_part)) return true;
    
    // 可选 break-glass: private network
    if (allow_private_ws) {
        if (is_private_or_loopback_host(host_part)) return true;
    }
    
    return false;
}

// ============================================================================
// Middleware Implementations
// ============================================================================

void LoggingMiddleware::execute(
    const httplib::Request& req,
    httplib::Response& res,
    NextHandler next
) {
    auto start = std::chrono::steady_clock::now();
    
    log_info("[HTTP] {} {} from {}", req.method, req.path, req.remote_addr);
    
    next(req, res);
    
    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    log_info("[HTTP] {} {} -> {} ({}ms)", 
        req.method, req.path, res.status, duration_ms);
}

CorsMiddleware::CorsMiddleware(const HttpGatewayOptions::CorsConfig& config)
    : config_(config) {}

void CorsMiddleware::execute(
    const httplib::Request& req,
    httplib::Response& res,
    NextHandler next
) {
    // Set CORS headers
    res.set_header("Access-Control-Allow-Origin", config_.allow_origin);
    res.set_header("Access-Control-Allow-Methods", config_.allow_methods);
    res.set_header("Access-Control-Allow-Headers", config_.allow_headers);
    res.set_header("Access-Control-Max-Age", std::to_string(config_.max_age));
    
    // Handle preflight OPTIONS request
    if (req.method == "OPTIONS") {
        res.status = 204;  // No Content
        return;
    }
    
    next(req, res);
}

AuthMiddleware::AuthMiddleware(const std::string& bearer_token)
    : bearer_token_(bearer_token) {}

void AuthMiddleware::execute(
    const httplib::Request& req,
    httplib::Response& res,
    NextHandler next
) {
    // Skip auth for health/metrics endpoints
    if (req.path == "/health" || req.path == "/metrics") {
        next(req, res);
        return;
    }
    
    // Skip if no token configured
    if (bearer_token_.empty()) {
        next(req, res);
        return;
    }
    
    // 🎯 OpenClaw 对齐: 支持多种认证模式
    bool authenticated = false;
    
    // 1. Bearer Token (Authorization: Bearer <token>)
    if (req.has_header("Authorization")) {
        std::string auth_header = req.get_header_value("Authorization");
        std::string expected = "Bearer " + bearer_token_;
        // 🎯 OpenClaw 对齐: 使用常量时间比较防止时序攻击
        if (hunterclaw::security::timing_safe_equal(auth_header, expected)) {
            authenticated = true;
        }
    }
    
    // 2. API Key Header (x-api-key: <token>)
    if (!authenticated && req.has_header("x-api-key")) {
        std::string api_key = req.get_header_value("x-api-key");
        // 🎯 OpenClaw 对齐: 使用常量时间比较防止时序攻击
        if (hunterclaw::security::timing_safe_equal(api_key, bearer_token_)) {
            authenticated = true;
        }
    }
    
    if (!authenticated) {
        // 🎯 OpenClaw 对齐: 认证失败时关闭连接 (shouldKeepAlive = false)
        res.set_header("Connection", "close");
        
        // 返回标准 OpenAI 错误格式
        json error_response = {
            {"error", {
                {"message", "Invalid or missing authentication credentials"},
                {"type", "invalid_request_error"},
                {"code", "invalid_api_key"}
            }}
        };
        res.status = 401;
        res.set_content(error_response.dump(), "application/json");
        return;
    }
    
    next(req, res);
}

void MiddlewareChain::use(std::shared_ptr<HttpMiddleware> middleware) {
    middlewares_.push_back(middleware);
}

void MiddlewareChain::execute(
    const httplib::Request& req,
    httplib::Response& res,
    std::function<void(const httplib::Request&, httplib::Response&)> final_handler
) {
    if (middlewares_.empty()) {
        final_handler(req, res);
        return;
    }
    
    // Build recursive middleware chain
    size_t index = 0;
    std::function<void(const httplib::Request&, httplib::Response&)> next;
    
    next = [this, &index, &final_handler, &next](const httplib::Request& req, httplib::Response& res) {
        if (index >= middlewares_.size()) {
            final_handler(req, res);
        } else {
            auto middleware = middlewares_[index++];
            middleware->execute(req, res, [&next](const httplib::Request& r, httplib::Response& s) {
                next(r, s);
            });
        }
    };
    
    next(req, res);
}

// ============================================================================
// HTTP Gateway Implementation
// ============================================================================

HttpGateway::HttpGateway(
    std::shared_ptr<GatewayServer> gateway_server,
    const HttpGatewayOptions& options
) : gateway_server_(gateway_server),
    options_(options),
    http_server_(std::make_unique<httplib::Server>())
{
    // Configure server
    http_server_->set_read_timeout(options_.read_timeout_seconds, 0);
    http_server_->set_write_timeout(options_.write_timeout_seconds, 0);
    http_server_->set_payload_max_length(options_.max_request_body_size);
    
    // Setup middleware
    if (options_.enable_cors) {
        use_middleware(std::make_shared<CorsMiddleware>(options_.cors));
    }
    
    use_middleware(std::make_shared<LoggingMiddleware>());
    
    if (!options_.bearer_token.empty()) {
        use_middleware(std::make_shared<AuthMiddleware>(options_.bearer_token));
    }
    
    // Setup routes
    setup_routes();
}

HttpGateway::~HttpGateway() {
    if (running_.load()) {
        stop();
    }
}

core::Result<bool> HttpGateway::start() {
    if (running_.load()) {
        return core::Result<bool>::Err("HTTP Gateway already running");
    }
    
    // 🎯 对齐 OpenClaw call.ts: isSecureWebSocketUrl 安全检查 (CWE-319)
    // 如果配置了 WebSocket upstream URL，验证其安全性
    if (!options_.ws_upstream_url.empty()) {
        bool allow_private = options_.allow_insecure_private_ws;
        if (!is_secure_websocket_url(options_.ws_upstream_url, allow_private)) {
            std::string error_msg = 
                "SECURITY ERROR: Gateway WebSocket URL \"" + options_.ws_upstream_url + 
                "\" uses plaintext ws:// to a non-loopback address. "
                "Both credentials and chat data would be exposed to network interception. "
                "Fix: Use wss:// for remote gateway URLs, or keep loopback binding.";
            log_error(error_msg);
            return core::Result<bool>::Err(error_msg);
        }
    }
    
    log_info("[HTTP Gateway] Starting on {}:{}", options_.host, options_.port);
    
    // Start server in background thread
    server_thread_ = std::thread([this]() {
        if (!http_server_->listen(options_.host.c_str(), options_.port)) {
            log_error("[HTTP Gateway] Failed to start server");
            return;
        }
    });
    
    running_.store(true);
    log_info("[HTTP Gateway] Started successfully");
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> HttpGateway::stop() {
    if (!running_.load()) {
        return core::Result<bool>::Err("HTTP Gateway not running");
    }
    
    log_info("[HTTP Gateway] Stopping...");
    
    http_server_->stop();
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    
    running_.store(false);
    log_info("[HTTP Gateway] Stopped");
    
    return core::Result<bool>::Ok(true);
}

bool HttpGateway::is_running() const {
    return running_.load();
}

void HttpGateway::set_llm_client(std::shared_ptr<llm::LLMClient> llm_client) {
    openai_compat_ = http::create_openai_compat(llm_client);
    log_info("[HTTP Gateway] OpenAI API compatibility layer initialized");
}

void HttpGateway::register_route(
    const std::string& method,
    const std::string& path,
    std::function<void(const httplib::Request&, httplib::Response&)> handler
) {
    auto wrapped_handler = [this, handler](const httplib::Request& req, httplib::Response& res) {
        middleware_chain_.execute(req, res, handler);
    };
    
    if (method == "GET") {
        http_server_->Get(path.c_str(), wrapped_handler);
    } else if (method == "POST") {
        http_server_->Post(path.c_str(), wrapped_handler);
    } else if (method == "PUT") {
        http_server_->Put(path.c_str(), wrapped_handler);
    } else if (method == "DELETE") {
        http_server_->Delete(path.c_str(), wrapped_handler);
    }
}

void HttpGateway::use_middleware(std::shared_ptr<HttpMiddleware> middleware) {
    middleware_chain_.use(middleware);
}

void HttpGateway::setup_routes() {
    // ========== Core Endpoints ==========
    
    register_route("GET", "/health", [this](const auto& req, auto& res) {
        handle_health(req, res);
    });
    
    register_route("GET", "/status", [this](const auto& req, auto& res) {
        handle_status(req, res);
    });
    
    if (options_.enable_metrics) {
        register_route("GET", "/metrics", [this](const auto& req, auto& res) {
            handle_metrics(req, res);
        });
    }
    
    // ========== RPC Endpoint ==========
    
    register_route("POST", "/api/v1/rpc", [this](const auto& req, auto& res) {
        handle_rpc(req, res);
    });
    
    // ========== REST API Endpoints ==========
    
    // Sessions
    register_route("GET", "/api/v1/sessions", [this](const auto& req, auto& res) {
        handle_list_sessions(req, res);
    });
    
    register_route("POST", "/api/v1/sessions", [this](const auto& req, auto& res) {
        handle_create_session(req, res);
    });
    
    http_server_->Get("/api/v1/sessions/(\\w+)", [this](const auto& req, auto& res) {
        handle_get_session(req, res);
    });
    
    http_server_->Delete("/api/v1/sessions/(\\w+)", [this](const auto& req, auto& res) {
        handle_delete_session(req, res);
    });
    
    // 🎯 OpenClaw 对齐: PATCH /api/v1/sessions/:id (sessions.patch)
    http_server_->Patch("/api/v1/sessions/([\\w:]+)", [this](const auto& req, auto& res) {
        handle_update_session(req, res);
    });
    
    // Channels
    register_route("GET", "/api/v1/channels", [this](const auto& req, auto& res) {
        handle_list_channels(req, res);
    });
    
    http_server_->Get("/api/v1/channels/(\\w+)", [this](const auto& req, auto& res) {
        handle_get_channel(req, res);
    });
    
    http_server_->Post("/api/v1/channels/(\\w+)/start", [this](const auto& req, auto& res) {
        handle_start_channel(req, res);
    });
    
    http_server_->Post("/api/v1/channels/(\\w+)/stop", [this](const auto& req, auto& res) {
        handle_stop_channel(req, res);
    });
    
    // ========== Agent CRUD Endpoints (🎯 OpenClaw 对齐 P1) ==========
    
    register_route("POST", "/api/v1/agents", [this](const auto& req, auto& res) {
        handle_create_agent(req, res);
    });
    
    register_route("GET", "/api/v1/agents", [this](const auto& req, auto& res) {
        handle_list_agents(req, res);
    });
    
    http_server_->Get("/api/v1/agents/([\\w\\-:]+)", [this](const auto& req, auto& res) {
        handle_get_agent(req, res);
    });
    
    http_server_->Patch("/api/v1/agents/([\\w\\-:]+)", [this](const auto& req, auto& res) {
        handle_update_agent(req, res);
    });
    
    http_server_->Delete("/api/v1/agents/([\\w\\-:]+)", [this](const auto& req, auto& res) {
        handle_delete_agent(req, res);
    });
    
    // Skills
    register_route("GET", "/api/v1/skills", [this](const auto& req, auto& res) {
        handle_list_skills(req, res);
    });
    
    http_server_->Get("/api/v1/skills/([\\w-]+)", [this](const auto& req, auto& res) {
        handle_get_skill(req, res);
    });
    
    // ========== Webhook Endpoints (🎯 OpenClaw 对齐) ==========
    
    http_server_->Post("/hooks/(.+)", [this](const auto& req, auto& res) {
        handle_webhook_hook(req, res);
    });
    
    // ========== OpenAI API Endpoints ==========
    
    if (options_.enable_openai_api) {
        register_route("POST", "/v1/chat/completions", [this](const auto& req, auto& res) {
            handle_chat_completions(req, res);
        });
        
        register_route("GET", "/v1/models", [this](const auto& req, auto& res) {
            handle_list_models(req, res);
        });
    }
}

// ========== Core Endpoint Implementations ==========

void HttpGateway::handle_health(const httplib::Request& req, httplib::Response& res) {
    json response = {
        {"status", "ok"},
        {"timestamp", core::utils::now().time_since_epoch().count()}
    };
    send_json(res, 200, response);
}

void HttpGateway::handle_status(const httplib::Request& req, httplib::Response& res) {
    // Simple status without detailed statistics
    json response = {
        {"running", gateway_server_->is_running()},
        {"service", "hunterclaw-gateway"},
        {"version", "0.1.0"}
    };
    
    send_json(res, 200, response);
}

void HttpGateway::handle_metrics(const httplib::Request& req, httplib::Response& res) {
    // Simple metrics in Prometheus format
    std::ostringstream oss;
    oss << "# HELP hunterclaw_running Server running status\n";
    oss << "# TYPE hunterclaw_running gauge\n";
    oss << "hunterclaw_running " << (gateway_server_->is_running() ? 1 : 0) << "\n";
    
    res.set_content(oss.str(), "text/plain; version=0.0.4");
}

void HttpGateway::handle_rpc(const httplib::Request& req, httplib::Response& res) {
    try {
        auto body = json::parse(req.body);
        
        if (!body.contains("method")) {
            send_error(res, 400, "Missing 'method' field");
            return;
        }
        
        std::string method = body["method"];
        json params = body.value("params", json::object());
        
        auto result = invoke_rpc(method, params);
        
        if (result.is_ok()) {
            send_json(res, 200, result.value.value_or(json::object()));
        } else {
            send_error(res, 500, result.error);
        }
        
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("Invalid request: ") + e.what());
    }
}

// ========== Session Endpoints ==========

void HttpGateway::handle_list_sessions(const httplib::Request& req, httplib::Response& res) {
    auto result = invoke_rpc("session.list", json::object());
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 500, result.error);
    }
}

void HttpGateway::handle_get_session(const httplib::Request& req, httplib::Response& res) {
    std::string session_id = req.matches[1];
    
    json params = {{"session_id", session_id}};
    auto result = invoke_rpc("session.get", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 404, result.error);
    }
}

void HttpGateway::handle_create_session(const httplib::Request& req, httplib::Response& res) {
    try {
        auto body = json::parse(req.body);
        auto result = invoke_rpc("session.create", body);
        
        if (result.is_ok()) {
            send_json(res, 201, result.value.value_or(json::object()));
        } else {
            send_error(res, 400, result.error);
        }
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("Invalid request body: ") + e.what());
    }
}

void HttpGateway::handle_delete_session(const httplib::Request& req, httplib::Response& res) {
    std::string session_id = req.matches[1];
    
    json params = {{"session_id", session_id}};
    auto result = invoke_rpc("session.delete", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 404, result.error);
    }
}

// 🎯 OpenClaw 对齐: PATCH /api/v1/sessions/:id (sessions.patch)
void HttpGateway::handle_update_session(const httplib::Request& req, httplib::Response& res) {
    std::string session_id = req.matches[1];
    
    try {
        auto body = json::parse(req.body);
        
        // 构建 session.patch RPC 参数
        json params = {{"key", session_id}};
        
        // 可选字段 (对齐 OpenClaw sessions.patch)
        if (body.contains("label")) {
            params["label"] = body["label"];
        }
        if (body.contains("model")) {
            params["model"] = body["model"];
        }
        if (body.contains("agentId")) {
            params["agentId"] = body["agentId"];
        }
        if (body.contains("toolPolicy")) {
            params["toolPolicy"] = body["toolPolicy"];
        }
        if (body.contains("temperature")) {
            params["temperature"] = body["temperature"];
        }
        if (body.contains("maxTokens")) {
            params["maxTokens"] = body["maxTokens"];
        }
        if (body.contains("topP")) {
            params["topP"] = body["topP"];
        }
        
        log_debug("PATCH /api/v1/sessions/{} with params: {}", session_id, params.dump());
        
        // 调用 session.patch RPC
        auto result = invoke_rpc("session.patch", params);
        
        if (result.is_ok()) {
            send_json(res, 200, result.value.value_or(json::object()));
        } else {
            send_error(res, 400, result.error);
        }
        
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("Invalid request body: ") + e.what());
    }
}

// ========== Channel Endpoints ==========

void HttpGateway::handle_list_channels(const httplib::Request& req, httplib::Response& res) {
    auto result = invoke_rpc("channel.list", json::object());
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 500, result.error);
    }
}

void HttpGateway::handle_get_channel(const httplib::Request& req, httplib::Response& res) {
    std::string channel_id = req.matches[1];
    
    json params = {{"channel_id", channel_id}};
    auto result = invoke_rpc("channel.get", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 404, result.error);
    }
}

void HttpGateway::handle_start_channel(const httplib::Request& req, httplib::Response& res) {
    std::string channel_id = req.matches[1];
    
    json params = {{"channel_id", channel_id}};
    
    try {
        if (!req.body.empty()) {
            auto body = json::parse(req.body);
            if (body.contains("account_id")) {
                params["account_id"] = body["account_id"];
            }
        }
    } catch (...) {}
    
    auto result = invoke_rpc("channel.start", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 400, result.error);
    }
}

void HttpGateway::handle_stop_channel(const httplib::Request& req, httplib::Response& res) {
    std::string channel_id = req.matches[1];
    
    json params = {{"channel_id", channel_id}};
    
    try {
        if (!req.body.empty()) {
            auto body = json::parse(req.body);
            if (body.contains("account_id")) {
                params["account_id"] = body["account_id"];
            }
        }
    } catch (...) {}
    
    auto result = invoke_rpc("channel.stop", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 400, result.error);
    }
}

// ========== Agent CRUD Endpoints (🎯 OpenClaw 对齐 P1) ==========

// POST /api/v1/agents (agents.create)
void HttpGateway::handle_create_agent(const httplib::Request& req, httplib::Response& res) {
    try {
        auto body = json::parse(req.body);
        
        // 必需字段验证
        if (!body.contains("agentId")) {
            send_error(res, 400, "Missing required field: agentId");
            return;
        }
        
        // 构建 agents.create RPC 参数
        json params = {
            {"agentId", body["agentId"]}
        };
        
        // 可选字段 (对齐 OpenClaw agents.create)
        if (body.contains("name")) params["name"] = body["name"];
        if (body.contains("model")) params["model"] = body["model"];
        if (body.contains("systemPrompt")) params["systemPrompt"] = body["systemPrompt"];
        if (body.contains("temperature")) params["temperature"] = body["temperature"];
        if (body.contains("maxTokens")) params["maxTokens"] = body["maxTokens"];
        if (body.contains("topP")) params["topP"] = body["topP"];
        if (body.contains("toolPolicy")) params["toolPolicy"] = body["toolPolicy"];
        if (body.contains("tools")) params["tools"] = body["tools"];
        if (body.contains("skills")) params["skills"] = body["skills"];
        if (body.contains("metadata")) params["metadata"] = body["metadata"];
        
        log_debug("POST /api/v1/agents: {}", params.dump());
        
        auto result = invoke_rpc("agents.create", params);
        
        if (result.is_ok() && result.value.has_value()) {
            send_json(res, 201, result.value.value());
        } else if (result.is_ok()) {
            send_json(res, 201, json::object());
        } else {
            send_error(res, 400, result.error);
        }
        
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("Invalid request: ") + e.what());
    }
}

// GET /api/v1/agents (agents.list)
void HttpGateway::handle_list_agents(const httplib::Request& req, httplib::Response& res) {
    json params = json::object();
    
    // 可选查询参数
    if (req.has_param("limit")) {
        try {
            params["limit"] = std::stoi(req.get_param_value("limit"));
        } catch (...) {
            send_error(res, 400, "Invalid limit parameter");
            return;
        }
    }
    if (req.has_param("offset")) {
        try {
            params["offset"] = std::stoi(req.get_param_value("offset"));
        } catch (...) {
            send_error(res, 400, "Invalid offset parameter");
            return;
        }
    }
    if (req.has_param("filter")) {
        params["filter"] = req.get_param_value("filter");
    }
    
    log_debug("GET /api/v1/agents: {}", params.dump());
    
    auto result = invoke_rpc("agents.list", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 500, result.error);
    }
}

// GET /api/v1/agents/:id (agents.get)
void HttpGateway::handle_get_agent(const httplib::Request& req, httplib::Response& res) {
    std::string agent_id = req.matches[1];
    
    json params = {{"agentId", agent_id}};
    log_debug("GET /api/v1/agents/{}", agent_id);
    
    auto result = invoke_rpc("agents.get", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 404, result.error);
    }
}

// PATCH /api/v1/agents/:id (agents.patch)
void HttpGateway::handle_update_agent(const httplib::Request& req, httplib::Response& res) {
    std::string agent_id = req.matches[1];
    
    try {
        auto body = json::parse(req.body);
        
        json params = {{"agentId", agent_id}};
        
        // 可选字段 (对齐 OpenClaw agents.patch)
        if (body.contains("name")) params["name"] = body["name"];
        if (body.contains("model")) params["model"] = body["model"];
        if (body.contains("systemPrompt")) params["systemPrompt"] = body["systemPrompt"];
        if (body.contains("temperature")) params["temperature"] = body["temperature"];
        if (body.contains("maxTokens")) params["maxTokens"] = body["maxTokens"];
        if (body.contains("topP")) params["topP"] = body["topP"];
        if (body.contains("toolPolicy")) params["toolPolicy"] = body["toolPolicy"];
        if (body.contains("tools")) params["tools"] = body["tools"];
        if (body.contains("skills")) params["skills"] = body["skills"];
        if (body.contains("metadata")) params["metadata"] = body["metadata"];
        
        log_debug("PATCH /api/v1/agents/{}: {}", agent_id, params.dump());
        
        auto result = invoke_rpc("agents.patch", params);
        
        if (result.is_ok()) {
            send_json(res, 200, result.value.value_or(json::object()));
        } else {
            send_error(res, 400, result.error);
        }
        
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("Invalid request: ") + e.what());
    }
}

// DELETE /api/v1/agents/:id (agents.delete)
void HttpGateway::handle_delete_agent(const httplib::Request& req, httplib::Response& res) {
    std::string agent_id = req.matches[1];
    
    json params = {{"agentId", agent_id}};
    log_debug("DELETE /api/v1/agents/{}", agent_id);
    
    auto result = invoke_rpc("agents.delete", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 404, result.error);
    }
}

// ========== Skill Endpoints ==========

void HttpGateway::handle_list_skills(const httplib::Request& req, httplib::Response& res) {
    auto result = invoke_rpc("skill.list", json::object());
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 500, result.error);
    }
}

void HttpGateway::handle_get_skill(const httplib::Request& req, httplib::Response& res) {
    std::string skill_name = req.matches[1];
    
    json params = {{"name", skill_name}};
    auto result = invoke_rpc("skill.get", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value.value_or(json::object()));
    } else {
        send_error(res, 404, result.error);
    }
}

// ========== OpenAI API Endpoints ==========

void HttpGateway::handle_chat_completions(const httplib::Request& req, httplib::Response& res) {
    if (!openai_compat_) {
        send_error(res, 503, "LLM client not configured. Please set LLM client first.");
        return;
    }
    
    try {
        // ========== 🎯 OpenClaw 对齐: Device ID + Session Key 解析 ==========
        
        std::string session_key;
        
        // 方式 1: 显式指定 session key (兼容旧版)
        if (req.has_header("x-hunterclaw-session-key") || req.has_header("x-openclaw-session-key")) {
            session_key = req.has_header("x-hunterclaw-session-key") 
                ? req.get_header_value("x-hunterclaw-session-key")
                : req.get_header_value("x-openclaw-session-key");
            
            log_debug("[OpenAI API] Using explicit session key: {}", session_key);
        }
        // 方式 2: 基于 Device ID 自动构造 session key ✅
        else {
            // 提取 device_id
            std::string device_id = extract_device_id_from_request(req);
            
            if (!device_id.empty() && is_valid_device_id(device_id)) {
                // 构造 device-based session key
                // 格式: agent:main:device:{deviceId}
                session_key = build_device_session_key("main", device_id);
                
                log_debug("[OpenAI API] Built device-based session key: {} (device_id: {})", 
                         session_key, device_id);
            } else {
                log_debug("[OpenAI API] Invalid or missing device_id, using default session");
            }
        }
        
        auto body = json::parse(req.body);
        auto request = ChatCompletionRequest::from_json(body);
        
        // 将 session_key 传递给 adapter (后续在 context 中使用)
        if (!session_key.empty()) {
            // 暂存在 request 的 user 字段 (临时方案,后续可以扩展 ChatCompletionRequest)
            request.user = session_key;
        }
        
        bool stream = request.stream.value_or(false);
        
        if (stream) {
            // SSE streaming response
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");  // Disable nginx buffering
            
            // Use chunked content provider for streaming
            auto compat = openai_compat_;  // Capture by value to avoid lambda issues
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, request, compat](size_t offset, httplib::DataSink& sink) {
                    bool stream_complete = false;
                    
                    if (!compat) {
                        return false;  // End streaming if no compat layer
                    }
                    
                    auto result = compat->create_chat_completion_stream(
                        request,
                        // On chunk callback
                        [&](const ChatCompletionChunk& chunk) {
                            std::string sse_data = chunk.to_sse();
                            sink.write(sse_data.data(), sse_data.size());
                        },
                        // On error callback
                        [&](const std::string& error) {
                            log_error("[OpenAI API] Stream error: {}", error);
                            std::string error_sse = "data: {\"error\":\"" + error + "\"}\n\n";
                            sink.write(error_sse.data(), error_sse.size());
                            stream_complete = true;
                        }
                    );
                    
                    if (result.is_ok()) {
                        // Send [DONE] marker
                        const char* done_marker = "data: [DONE]\n\n";
                        sink.write(done_marker, strlen(done_marker));
                    }
                    
                    stream_complete = true;
                    return !stream_complete;  // Return false to end streaming
                }
            );
            
        } else {
            // Non-streaming response
            if (!openai_compat_) {
                send_json(res, 500, {{"error", "OpenAI compatibility layer not initialized"}});
                return;
            }
            
            auto result = openai_compat_->create_chat_completion(request);
            
            if (result.is_ok()) {
                if (result.value.has_value()) {
                    send_json(res, 200, result.value.value().to_json());
                } else {
                    send_error(res, 500, "Empty response from LLM");
                }
            } else {
                send_error(res, 500, result.error);
            }
        }
        
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("Invalid request: ") + e.what());
    }
}

void HttpGateway::handle_list_models(const httplib::Request& req, httplib::Response& res) {
    if (!openai_compat_) {
        send_openai_error(res, 503, 
            "LLM client not configured",
            "server_error", "llm_not_configured");
        return;
    }
    
    auto models = openai_compat_->list_models();
    
    json::array_t models_json;
    for (const auto& model : models) {
        models_json.push_back(model.to_json());
    }
    
    json response = {
        {"object", "list"},
        {"data", models_json}
    };
    
    send_json(res, 200, response);
}

// ========== Helper Methods ==========

void HttpGateway::send_json(httplib::Response& res, int status, const json& data) {
    res.status = status;
    res.set_content(data.dump(), "application/json");
}

void HttpGateway::send_error(httplib::Response& res, int status, const std::string& error) {
    json response = {{"error", error}};
    send_json(res, status, response);
}

void HttpGateway::send_openai_error(
    httplib::Response& res,
    int status,
    const std::string& message,
    const std::string& type,
    const std::string& code
) {
    // 🎯 OpenClaw 对齐: 标准 OpenAI 错误响应格式
    json error_response = {
        {"error", {
            {"message", message},
            {"type", type.empty() ? "invalid_request_error" : type},
            {"code", code.empty() ? "invalid_request" : code}
        }}
    };
    send_json(res, status, error_response);
}

void HttpGateway::send_sse_chunk(httplib::Response& res, const std::string& data) {
    res.set_content("data: " + data + "\n\n", "text/event-stream");
}

// ========== Webhook Endpoints (🎯 OpenClaw 严格对齐) ==========
// 对齐 OpenClaw server-http.ts createHooksRequestHandler + server/hooks.ts

void HttpGateway::handle_webhook_hook(const httplib::Request& req, httplib::Response& res) {
    std::string hook_path = req.matches[1];

    log_info("Webhook hook: POST /hooks/{}", hook_path);

    // ✅ Aligned with openclaw: token must NOT be in query parameters
    // server-http.ts: if (url.searchParams.has("token")) → 400
    if (req.has_param("token")) {
        res.status = 400;
        res.set_header("Content-Type", "text/plain; charset=utf-8");
        res.set_content(
            "Hook token must be provided via Authorization: Bearer <token> or "
            "X-OpenClaw-Token header (query parameters are not allowed).",
            "text/plain"
        );
        return;
    }

    // ✅ Aligned with openclaw: extractHookToken(req)
    // Priority: Authorization: Bearer <token> > X-OpenClaw-Token
    std::string provided_token;

    // Method 1: Authorization: Bearer <token>
    if (req.has_header("Authorization")) {
        std::string auth_header = req.get_header_value("Authorization");
        // Case-insensitive "bearer " prefix check
        std::string lower_auth = auth_header;
        std::transform(lower_auth.begin(), lower_auth.end(), lower_auth.begin(), ::tolower);
        if (lower_auth.size() > 7 && lower_auth.substr(0, 7) == "bearer ") {
            provided_token = auth_header.substr(7);
            // trim leading/trailing whitespace
            provided_token.erase(0, provided_token.find_first_not_of(" \t"));
            provided_token.erase(provided_token.find_last_not_of(" \t") + 1);
        }
    }

    // Method 2: X-OpenClaw-Token header (aligned with openclaw)
    if (provided_token.empty() && req.has_header("X-OpenClaw-Token")) {
        provided_token = req.get_header_value("X-OpenClaw-Token");
        provided_token.erase(0, provided_token.find_first_not_of(" \t"));
        provided_token.erase(provided_token.find_last_not_of(" \t") + 1);
    }

    // ✅ Aligned with openclaw: safeEqualSecret(token, hooksConfig.token)
    // Constant-time comparison to prevent timing attacks.
    bool authenticated = false;
    if (!provided_token.empty() && !options_.bearer_token.empty()) {
        authenticated = hunterclaw::security::timing_safe_equal(provided_token, options_.bearer_token);
    }

    if (!authenticated) {
        // \u2705 \u5bf9\u9f50 OpenClaw: hookAuthLimiter.recordFailure + 401\n        // hookAuthLimiter: 20 failures / 60s \u6ed1\u52a8\u7a97\u53e3\u9650\u6d41\n        {\n            std::lock_guard<std::mutex> lock(auth_limiter_mutex_);\n            std::string client_key = req.remote_addr;\n            auto now = std::chrono::steady_clock::now();\n            auto& record = auth_failure_records_[client_key];\n            // \u6e05\u9664 60s \u4ee5\u524d\u7684\u8bb0\u5f55\n            record.erase(std::remove_if(record.begin(), record.end(),\n                [&now](const auto& t) {\n                    return std::chrono::duration_cast<std::chrono::seconds>(now - t).count() > 60;\n                }), record.end());\n            record.push_back(now);\n            // \u8d85\u8fc7 20 \u6b21\u5931\u8d25\uff0c\u8fd4\u56de 429\n            if (record.size() > 20) {\n                res.set_header(\"Connection\", \"close\");\n                res.status = 429;\n                res.set_header(\"Content-Type\", \"text/plain; charset=utf-8\");\n                res.set_content(\"Too Many Requests\", \"text/plain\");\n                return;\n            }\n        }
        res.set_header("Connection", "close");
        res.status = 401;
        res.set_header("Content-Type", "text/plain; charset=utf-8");
        res.set_content("Unauthorized", "text/plain");
        return;
    }
    // \u2705 \u5bf9\u9f50 OpenClaw: hookAuthLimiter.reset(clientKey) on success\n    {\n        std::lock_guard<std::mutex> lock(auth_limiter_mutex_);\n        auth_failure_records_.erase(req.remote_addr);\n    }

    // ✅ Aligned with openclaw: subPath check — must be non-empty
    if (hook_path.empty()) {
        res.status = 404;
        res.set_header("Content-Type", "text/plain; charset=utf-8");
        res.set_content("Not Found", "text/plain");
        return;
    }

    // ✅ Aligned with openclaw: readJsonBody(req, hooksConfig.maxBodyBytes)
    // OpenClaw default: 256KB (DEFAULT_HOOKS_MAX_BODY_BYTES = 256 * 1024)
    static constexpr size_t DEFAULT_HOOKS_MAX_BODY_BYTES = 256 * 1024;
    if (req.body.size() > DEFAULT_HOOKS_MAX_BODY_BYTES) {
        json error_body = {{"ok", false}, {"error", "payload too large"}};
        res.status = 413;
        res.set_header("Content-Type", "application/json; charset=utf-8");
        res.set_content(error_body.dump(), "application/json");
        return;
    }

    // ✅ Aligned with openclaw: parse body, default to {} on empty/non-object
    json payload = json::object();
    if (!req.body.empty()) {
        try {
            auto parsed = json::parse(req.body);
            if (parsed.is_object()) {
                payload = parsed;
            }
        } catch (const std::exception& e) {
            json error_body = {{"ok", false}, {"error", std::string("invalid json: ") + e.what()}};
            res.status = 400;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.set_content(error_body.dump(), "application/json");
            return;
        }
    }

    // ✅ Aligned with openclaw: resolveHookIdempotencyKey
    // Priority: Idempotency-Key header > X-OpenClaw-Idempotency-Key header > payload.idempotencyKey
    std::string idempotency_key;
    if (req.has_header("Idempotency-Key")) {
        idempotency_key = req.get_header_value("Idempotency-Key");
    } else if (req.has_header("X-OpenClaw-Idempotency-Key")) {
        idempotency_key = req.get_header_value("X-OpenClaw-Idempotency-Key");
    } else if (payload.contains("idempotencyKey") && payload["idempotencyKey"].is_string()) {
        idempotency_key = payload["idempotencyKey"].get<std::string>();
    }
    // Trim and enforce max length (aligned with openclaw: MAX_HOOK_IDEMPOTENCY_KEY_LENGTH = 256)
    idempotency_key.erase(0, idempotency_key.find_first_not_of(" \t"));
    idempotency_key.erase(idempotency_key.find_last_not_of(" \t") + 1);
    if (idempotency_key.size() > 256) {
        idempotency_key.clear();
    }

    // ✅ Aligned with openclaw: subPath === "wake"
    // normalizeWakePayload: text (required), mode ("now" | "next-heartbeat")
    if (hook_path == "wake") {
        std::string text;
        if (payload.contains("text") && payload["text"].is_string()) {
            text = payload["text"].get<std::string>();
            // trim
            text.erase(0, text.find_first_not_of(" \t\n\r"));
            text.erase(text.find_last_not_of(" \t\n\r") + 1);
        }
        if (text.empty()) {
            json error_body = {{"ok", false}, {"error", "text required"}};
            res.status = 400;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.set_content(error_body.dump(), "application/json");
            return;
        }
        std::string mode = "now";
        if (payload.contains("mode") && payload["mode"].is_string() &&
            payload["mode"].get<std::string>() == "next-heartbeat") {
            mode = "next-heartbeat";
        }
        // ✅ Aligned with openclaw: dispatchWakeHook → enqueueSystemEvent + requestHeartbeatNow
        json wake_params = {{"text", text}, {"mode", mode}};
        invoke_rpc("hook.wake", wake_params);  // internal dispatch
        json response = {{"ok", true}, {"mode", mode}};
        res.status = 200;
        res.set_header("Content-Type", "application/json; charset=utf-8");
        res.set_content(response.dump(), "application/json");
        return;
    }

    // ✅ Aligned with openclaw: subPath === "agent"
    // normalizeAgentPayload: message (required), name (default "Hook"), agentId, wakeMode,
    //   sessionKey, channel, to, model, thinking, timeoutSeconds, deliver
    if (hook_path == "agent") {
        // message: required, non-empty
        std::string message;
        if (payload.contains("message") && payload["message"].is_string()) {
            message = payload["message"].get<std::string>();
            message.erase(0, message.find_first_not_of(" \t\n\r"));
            message.erase(message.find_last_not_of(" \t\n\r") + 1);
        }
        if (message.empty()) {
            json error_body = {{"ok", false}, {"error", "message required"}};
            res.status = 400;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.set_content(error_body.dump(), "application/json");
            return;
        }

        // name: optional, default "Hook"
        std::string name = "Hook";
        if (payload.contains("name") && payload["name"].is_string()) {
            std::string raw_name = payload["name"].get<std::string>();
            raw_name.erase(0, raw_name.find_first_not_of(" \t"));
            raw_name.erase(raw_name.find_last_not_of(" \t") + 1);
            if (!raw_name.empty()) name = raw_name;
        }

        // agentId: optional
        std::string agent_id;
        if (payload.contains("agentId") && payload["agentId"].is_string()) {
            agent_id = payload["agentId"].get<std::string>();
            agent_id.erase(0, agent_id.find_first_not_of(" \t"));
            agent_id.erase(agent_id.find_last_not_of(" \t") + 1);
        }

        // wakeMode: "now" | "next-heartbeat", default "now"
        std::string wake_mode = "now";
        if (payload.contains("wakeMode") && payload["wakeMode"].is_string() &&
            payload["wakeMode"].get<std::string>() == "next-heartbeat") {
            wake_mode = "next-heartbeat";
        }

        // sessionKey: optional
        std::string session_key;
        if (payload.contains("sessionKey") && payload["sessionKey"].is_string()) {
            session_key = payload["sessionKey"].get<std::string>();
            session_key.erase(0, session_key.find_first_not_of(" \t"));
            session_key.erase(session_key.find_last_not_of(" \t") + 1);
        }

        // channel: optional, default "last"
        std::string channel = "last";
        if (payload.contains("channel") && payload["channel"].is_string()) {
            std::string raw_ch = payload["channel"].get<std::string>();
            raw_ch.erase(0, raw_ch.find_first_not_of(" \t"));
            raw_ch.erase(raw_ch.find_last_not_of(" \t") + 1);
            if (!raw_ch.empty()) channel = raw_ch;
        }

        // to: optional
        std::string to;
        if (payload.contains("to") && payload["to"].is_string()) {
            to = payload["to"].get<std::string>();
            to.erase(0, to.find_first_not_of(" \t"));
            to.erase(to.find_last_not_of(" \t") + 1);
        }

        // model: optional, if provided must be non-empty
        std::string model;
        if (payload.contains("model")) {
            if (payload["model"].is_string()) {
                model = payload["model"].get<std::string>();
                model.erase(0, model.find_first_not_of(" \t"));
                model.erase(model.find_last_not_of(" \t") + 1);
                if (model.empty()) {
                    // ✅ Aligned with openclaw: if modelRaw !== undefined && !model → error
                    json error_body = {{"ok", false}, {"error", "model required"}};
                    res.status = 400;
                    res.set_header("Content-Type", "application/json; charset=utf-8");
                    res.set_content(error_body.dump(), "application/json");
                    return;
                }
            }
        }

        // thinking: optional
        std::string thinking;
        if (payload.contains("thinking") && payload["thinking"].is_string()) {
            thinking = payload["thinking"].get<std::string>();
            thinking.erase(0, thinking.find_first_not_of(" \t"));
            thinking.erase(thinking.find_last_not_of(" \t") + 1);
        }

        // timeoutSeconds: optional, must be finite positive number
        std::optional<int> timeout_seconds;
        if (payload.contains("timeoutSeconds") && payload["timeoutSeconds"].is_number()) {
            double raw_timeout = payload["timeoutSeconds"].get<double>();
            if (std::isfinite(raw_timeout) && raw_timeout > 0) {
                timeout_seconds = static_cast<int>(raw_timeout);
            }
        }

        // deliver: optional, default true (resolveHookDeliver: raw !== false)
        bool deliver = true;
        if (payload.contains("deliver") && payload["deliver"].is_boolean()) {
            deliver = payload["deliver"].get<bool>();
        }

        // ✅ Aligned with openclaw: dispatchAgentHook → runCronIsolatedAgentTurn
        // Build dispatch params for internal RPC
        json dispatch_params = {
            {"message", message},
            {"name", name},
            {"wakeMode", wake_mode},
            {"deliver", deliver},
            {"channel", channel},
            {"idempotencyKey", idempotency_key}
        };
        if (!agent_id.empty())    dispatch_params["agentId"] = agent_id;
        if (!session_key.empty()) dispatch_params["sessionKey"] = session_key;
        if (!to.empty())          dispatch_params["to"] = to;
        if (!model.empty())       dispatch_params["model"] = model;
        if (!thinking.empty())    dispatch_params["thinking"] = thinking;
        if (timeout_seconds)      dispatch_params["timeoutSeconds"] = *timeout_seconds;

        log_debug("Dispatching hook agent: {}", dispatch_params.dump());

        auto result = invoke_rpc("hook.agent", dispatch_params);

        if (result.is_ok()) {
            std::string run_id = result.value.has_value() && result.value->contains("runId")
                ? result.value->at("runId").get<std::string>()
                : "";
            json response = {{"ok", true}, {"runId", run_id}};
            res.status = 200;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.set_content(response.dump(), "application/json");
        } else {
            json error_body = {{"ok", false}, {"error", result.error}};
            res.status = 400;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.set_content(error_body.dump(), "application/json");
        }
        return;
    }

    // ✅ Aligned with openclaw: unknown subPath → 404
    // (mappings support is a future extension)
    res.status = 404;
    res.set_header("Content-Type", "text/plain; charset=utf-8");
    res.set_content("Not Found", "text/plain");
}

core::Result<json> HttpGateway::invoke_rpc(
    const std::string& method,
    const json& params
) {
    // Invoke RPC through GatewayMethods (完整实现 - 对齐 OpenClaw)
    if (!gateway_server_) {
        return core::Result<json>::Err("Gateway server not initialized");
    }
    
    // Get runtime state to access method handler
    auto state = gateway_server_->runtime_state();
    if (!state) {
        return core::Result<json>::Err("Gateway runtime state not available");
    }
    
    // Check if method handler is set
    if (!state->method_handler) {
        return core::Result<json>::Err("Gateway method handler not initialized");
    }
    
    // Invoke the method through the handler
    // HTTP gateway uses a default ClientContext (no WebSocket session)
    ClientContext http_ctx;
    http_ctx.conn_id = "http";
    http_ctx.authenticated = true;  // HTTP gateway is pre-authenticated
    return state->method_handler(method, params, http_ctx);
}

// ============================================================================
// Factory Functions
// ============================================================================

core::Result<std::shared_ptr<HttpGateway>> create_http_gateway(
    std::shared_ptr<GatewayServer> gateway_server,
    const HttpGatewayOptions& options
) {
    try {
        auto http_gateway = std::make_shared<HttpGateway>(gateway_server, options);
        return core::Result<std::shared_ptr<HttpGateway>>::Ok(http_gateway);
    } catch (const std::exception& e) {
        return core::Result<std::shared_ptr<HttpGateway>>::Err(
            std::string("Failed to create HTTP Gateway: ") + e.what()
        );
    }
}

} // namespace gateway
} // namespace hunterclaw
