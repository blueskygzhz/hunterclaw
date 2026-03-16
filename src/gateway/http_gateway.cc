#include "hunterclaw/gateway/http_gateway.h"
#include "hunterclaw/gateway/openai_adapter.h"
#include "hunterclaw/gateway/session_device_id.h"  // ✅ Device ID 工具
#include "hunterclaw/core/utils.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

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
        if (auth_header == expected) {
            authenticated = true;
        }
    }
    
    // 2. API Key Header (x-api-key: <token>)
    if (!authenticated && req.has_header("x-api-key")) {
        std::string api_key = req.get_header_value("x-api-key");
        if (api_key == bearer_token_) {
            authenticated = true;
        }
    }
    
    if (!authenticated) {
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
    openai_adapter_ = create_openai_adapter(llm_client);
    log_info("[HTTP Gateway] OpenAI API adapter initialized");
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
    
    // Skills
    register_route("GET", "/api/v1/skills", [this](const auto& req, auto& res) {
        handle_list_skills(req, res);
    });
    
    http_server_->Get("/api/v1/skills/([\\w-]+)", [this](const auto& req, auto& res) {
        handle_get_skill(req, res);
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
            send_json(res, 200, result.value);
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
        send_json(res, 200, result.value);
    } else {
        send_error(res, 500, result.error);
    }
}

void HttpGateway::handle_get_session(const httplib::Request& req, httplib::Response& res) {
    std::string session_id = req.matches[1];
    
    json params = {{"session_id", session_id}};
    auto result = invoke_rpc("session.get", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value);
    } else {
        send_error(res, 404, result.error);
    }
}

void HttpGateway::handle_create_session(const httplib::Request& req, httplib::Response& res) {
    try {
        auto body = json::parse(req.body);
        auto result = invoke_rpc("session.create", body);
        
        if (result.is_ok()) {
            send_json(res, 201, result.value);
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
        send_json(res, 200, result.value);
    } else {
        send_error(res, 404, result.error);
    }
}

// ========== Channel Endpoints ==========

void HttpGateway::handle_list_channels(const httplib::Request& req, httplib::Response& res) {
    auto result = invoke_rpc("channel.list", json::object());
    
    if (result.is_ok()) {
        send_json(res, 200, result.value);
    } else {
        send_error(res, 500, result.error);
    }
}

void HttpGateway::handle_get_channel(const httplib::Request& req, httplib::Response& res) {
    std::string channel_id = req.matches[1];
    
    json params = {{"channel_id", channel_id}};
    auto result = invoke_rpc("channel.get", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value);
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
        send_json(res, 200, result.value);
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
        send_json(res, 200, result.value);
    } else {
        send_error(res, 400, result.error);
    }
}

// ========== Skill Endpoints ==========

void HttpGateway::handle_list_skills(const httplib::Request& req, httplib::Response& res) {
    auto result = invoke_rpc("skill.list", json::object());
    
    if (result.is_ok()) {
        send_json(res, 200, result.value);
    } else {
        send_error(res, 500, result.error);
    }
}

void HttpGateway::handle_get_skill(const httplib::Request& req, httplib::Response& res) {
    std::string skill_name = req.matches[1];
    
    json params = {{"name", skill_name}};
    auto result = invoke_rpc("skill.get", params);
    
    if (result.is_ok()) {
        send_json(res, 200, result.value);
    } else {
        send_error(res, 404, result.error);
    }
}

// ========== OpenAI API Endpoints ==========

void HttpGateway::handle_chat_completions(const httplib::Request& req, httplib::Response& res) {
    if (!openai_adapter_) {
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
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, request](size_t offset, httplib::DataSink& sink) {
                    bool stream_complete = false;
                    
                    auto result = openai_adapter_->create_chat_completion_stream(
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
            auto result = openai_adapter_->create_chat_completion(request);
            
            if (result.is_ok()) {
                send_json(res, 200, result.value.to_json());
            } else {
                send_error(res, 500, result.error);
            }
        }
        
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("Invalid request: ") + e.what());
    }
}

void HttpGateway::handle_list_models(const httplib::Request& req, httplib::Response& res) {
    if (!openai_adapter_) {
        send_openai_error(res, 503, 
            "LLM client not configured",
            "server_error", "llm_not_configured");
        return;
    }
    
    auto models = openai_adapter_->list_models();
    
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

void HttpGateway::send_sse_chunk(httplib::Response& res, const std::string& data) {
    res.set_content("data: " + data + "\n\n", "text/event-stream");
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
    return state->method_handler(method, params);
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
