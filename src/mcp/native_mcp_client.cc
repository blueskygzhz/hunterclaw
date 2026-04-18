// ============================================================================
// Native MCP Client — 纯 C++ MCP 客户端实现
//
// 三层架构:
//   1. McpTransport (接口)
//      ├── StdioTransport: fork 子进程 + JSON-RPC over stdin/stdout
//      └── HttpTransport:  infra::HttpClient + Streamable HTTP/SSE
//   2. McpServerConnection: 单 server 的协议握手 + 工具调用
//   3. NativeMcpClient:    多 server 连接池管理，实现 McporterClient 接口
//
// MCP 协议 (JSON-RPC 2.0):
//   Client → Server: initialize({protocolVersion, capabilities, clientInfo})
//   Server → Client: {protocolVersion, capabilities, serverInfo}
//   Client → Server: notifications/initialized  (通知，无响应)
//   Client → Server: tools/list
//   Server → Client: {tools: [{name, description, inputSchema}]}
//   Client → Server: tools/call({name, arguments})
//   Server → Client: {content: [...], isError: bool}
// ============================================================================

#include "hunterclaw/mcp/native_mcp_client.h"
#include "hunterclaw/mcp/mcp_transport_config.h"
#include "hunterclaw/mcp/json_converter.h"
#include "hunterclaw/core/subprocess.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <sstream>

using json = nlohmann::json;
using hunterclaw::mcp::converters::JsonConverter;

namespace hunterclaw {
namespace mcp {

namespace {

auto log() {
    static auto logger = spdlog::get("mcp-native");
    if (!logger) logger = spdlog::stdout_logger_mt("mcp-native");
    return logger;
}

constexpr const char* MCP_PROTOCOL_VERSION = "2025-03-26";
constexpr const char* CLIENT_NAME = "hunterclaw";
constexpr const char* CLIENT_VERSION = "1.0.0";

} // namespace

// ============================================================================
// StdioTransport 实现
// ============================================================================

StdioTransport::StdioTransport(const Config& config) : config_(config) {}

StdioTransport::~StdioTransport() { close(); }

core::Result<void> StdioTransport::start() {
    if (config_.command.empty()) {
        return core::Result<void>::Err("StdioTransport: command is empty");
    }

    // 构造命令行: command + args
    std::vector<std::string> cmdline;
    cmdline.push_back(config_.command);
    for (const auto& arg : config_.args) cmdline.push_back(arg);

    // TODO: core::subprocess::popen 目前不直接支持 env/cwd 注入，
    // 这里以默认环境启动；完整实现应扩展 popen 接口。
    // 实际 env 可通过 setenv() 临时设置（非线程安全，此处简化）。
    for (const auto& [k, v] : config_.env) {
        setenv(k.c_str(), v.c_str(), 1);
    }

    process_ = core::subprocess::popen(cmdline, /*capture_stdout=*/true, /*capture_stderr=*/true);
    if (!process_) {
        return core::Result<void>::Err(
            "StdioTransport: failed to spawn process: " + config_.command);
    }

    // 启动读取线程
    running_ = true;
    reader_thread_ = std::thread([this]() { reader_loop(); });

    log()->info("[StdioTransport] started: {} (pid={})", config_.command, process_->pid());
    return core::Result<void>::Ok();
}

void StdioTransport::reader_loop() {
    while (running_) {
        if (!process_ || !process_->is_alive()) {
            log()->warn("[StdioTransport] child process exited unexpectedly");
            fail_all_pending("child process exited");
            break;
        }

        // 非阻塞读取 stdout
        std::string chunk = process_->read_stdout(4096);
        if (chunk.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        read_buffer_ += chunk;

        // 尝试解析完整的 JSON-RPC 消息（按行分隔，MCP Stdio 协议）
        while (auto msg = try_parse_message()) {
            try {
                dispatch_message(*msg);
            } catch (const std::exception& e) {
                log()->warn("[StdioTransport] dispatch error: {}", e.what());
            }
        }
    }
    log()->debug("[StdioTransport] reader loop exited");
}

std::optional<json> StdioTransport::try_parse_message() {
    // MCP Stdio: 每条消息一行，以 \n 分隔
    auto newline_pos = read_buffer_.find('\n');
    if (newline_pos == std::string::npos) return std::nullopt;

    std::string line = read_buffer_.substr(0, newline_pos);
    read_buffer_.erase(0, newline_pos + 1);

    // trim
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) return try_parse_message();  // 跳过空行

    try {
        return json::parse(line);
    } catch (const std::exception& e) {
        log()->warn("[StdioTransport] JSON parse error: {} (line: {})", e.what(), line);
        return try_parse_message();  // 跳过坏消息，继续解析
    }
}

void StdioTransport::dispatch_message(const json& msg) {
    // 响应消息: 包含 id + (result | error)
    if (msg.contains("id") && msg["id"].is_number()) {
        int id = msg["id"].get<int>();
        std::shared_ptr<PendingRequest> pending;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                pending = it->second;
                pending_requests_.erase(it);
            }
        }
        if (pending) {
            if (msg.contains("error")) {
                try {
                    std::string err_msg = "MCP error";
                    if (msg["error"].is_object() && msg["error"].contains("message")) {
                        err_msg = msg["error"]["message"].get<std::string>();
                    }
                    pending->promise.set_exception(
                        std::make_exception_ptr(std::runtime_error(err_msg)));
                } catch (...) {}
            } else if (msg.contains("result")) {
                try {
                    pending->promise.set_value(msg["result"]);
                } catch (...) {}
            } else {
                try {
                    pending->promise.set_value(json());
                } catch (...) {}
            }
        }
        return;
    }

    // 服务端发来的通知/请求（如 progress updates）——当前简化为仅记录
    if (msg.contains("method")) {
        log()->debug("[StdioTransport] server notification: {}",
                     msg["method"].get<std::string>());
    }
}

core::Result<void> StdioTransport::write_message(const json& msg) {
    if (!process_ || !process_->is_alive()) {
        return core::Result<void>::Err("process not alive");
    }

    std::string payload = msg.dump() + "\n";  // MCP Stdio: 行分隔

    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!process_->write_stdin(payload)) {
        return core::Result<void>::Err("failed to write to stdin");
    }
    return core::Result<void>::Ok();
}

core::Result<json> StdioTransport::send_request(
    const std::string& method, const json& params, int timeout_ms) {

    if (!running_ || !process_ || !process_->is_alive()) {
        return core::Result<json>::Err("transport not running");
    }

    int id = next_id_.fetch_add(1);
    auto pending = std::make_shared<PendingRequest>();
    pending->deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_[id] = pending;
    }

    json request = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

    auto write_result = write_message(request);
    if (write_result.is_err()) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(id);
        return core::Result<json>::Err("write failed: " + write_result.error);
    }

    // 等待响应
    auto future = pending->promise.get_future();
    auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status != std::future_status::ready) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(id);
        return core::Result<json>::Err(
            "request timeout after " + std::to_string(timeout_ms) + "ms: " + method);
    }

    try {
        return core::Result<json>::Ok(future.get());
    } catch (const std::exception& e) {
        return core::Result<json>::Err(std::string("MCP error: ") + e.what());
    }
}

core::Result<void> StdioTransport::send_notification(
    const std::string& method, const json& params) {

    if (!running_ || !process_ || !process_->is_alive()) {
        return core::Result<void>::Err("transport not running");
    }

    // 通知: 没有 id 字段
    json notification = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };

    return write_message(notification);
}

bool StdioTransport::is_alive() const {
    return running_ && process_ && process_->is_alive();
}

int StdioTransport::pid() const {
    return process_ ? process_->pid() : -1;
}

void StdioTransport::fail_all_pending(const std::string& error) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& [id, pending] : pending_requests_) {
        try {
            pending->promise.set_exception(
                std::make_exception_ptr(std::runtime_error(error)));
        } catch (...) {}
    }
    pending_requests_.clear();
}

void StdioTransport::close() {
    if (!running_.exchange(false)) return;  // 已关闭

    fail_all_pending("transport closed");

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }

    if (process_) {
        // 发送 SIGTERM → 等待 → 如果还活着就 SIGKILL（popen 析构处理）
        delete process_;
        process_ = nullptr;
    }

    log()->debug("[StdioTransport] closed");
}

// ============================================================================
// HttpTransport 实现 (Streamable HTTP / SSE)
// ============================================================================

HttpTransport::HttpTransport(const Config& config) : config_(config) {
    http_client_ = infra::HttpClient::create();
}

HttpTransport::~HttpTransport() { close(); }

core::Result<void> HttpTransport::start() {
    if (config_.url.empty()) {
        return core::Result<void>::Err("HttpTransport: url is empty");
    }
    if (!http_client_) {
        return core::Result<void>::Err("HttpTransport: http client creation failed");
    }

    // SSE 传输类型：启动后台流监听线程
    if (config_.transport_type == McpTransportType::Sse) {
        start_sse_stream();
    }

    log()->info("[HttpTransport] started: {} (type={})",
                config_.url,
                config_.transport_type == McpTransportType::Sse ? "sse" : "streamable-http");
    return core::Result<void>::Ok();
}

core::Result<json> HttpTransport::send_http_request(const json& message, int timeout_ms) {
    infra::HttpRequestOptions options;
    options.timeout_ms = timeout_ms;
    options.body = message.dump();

    // 标准 MCP headers
    options.headers["Content-Type"] = "application/json";
    options.headers["Accept"] = "application/json, text/event-stream";

    // 用户自定义 headers (如 Authorization)
    for (const auto& [k, v] : config_.headers) {
        options.headers[k] = v;
    }

    // MCP Session ID (StreamableHttp 会话保持)
    if (session_id_.has_value()) {
        options.headers["Mcp-Session-Id"] = *session_id_;
    }

    auto resp_result = http_client_->post(config_.url, options);
    if (resp_result.is_err()) {
        return core::Result<json>::Err("HTTP request failed: " + resp_result.error);
    }

    const auto& resp = resp_result.get();

    // 首次 initialize 响应中提取 Mcp-Session-Id
    if (!session_id_.has_value()) {
        auto it = resp.headers.find("mcp-session-id");
        if (it == resp.headers.end()) it = resp.headers.find("Mcp-Session-Id");
        if (it != resp.headers.end()) {
            session_id_ = it->second;
            log()->debug("[HttpTransport] session id = {}", *session_id_);
        }
    }

    if (!resp.ok) {
        return core::Result<json>::Err(
            "HTTP " + std::to_string(resp.status_code) + ": " + resp.body);
    }

    // 响应可能是:
    //   1. 单条 JSON 响应 (Content-Type: application/json)
    //   2. SSE 流 (Content-Type: text/event-stream)
    auto content_type_it = resp.headers.find("content-type");
    if (content_type_it == resp.headers.end()) content_type_it = resp.headers.find("Content-Type");

    if (content_type_it != resp.headers.end() &&
        content_type_it->second.find("text/event-stream") != std::string::npos) {
        // SSE 响应: 解析 data: 行
        auto event = parse_sse_event(resp.body);
        if (event) return core::Result<json>::Ok(*event);
        return core::Result<json>::Err("SSE response has no valid event");
    }

    // 标准 JSON 响应
    try {
        auto parsed = json::parse(resp.body);
        // 提取 result 或 error
        if (parsed.contains("error")) {
            std::string msg = "MCP error";
            if (parsed["error"].is_object() && parsed["error"].contains("message")) {
                msg = parsed["error"]["message"].get<std::string>();
            }
            return core::Result<json>::Err(msg);
        }
        if (parsed.contains("result")) {
            return core::Result<json>::Ok(parsed["result"]);
        }
        return core::Result<json>::Ok(parsed);
    } catch (const std::exception& e) {
        return core::Result<json>::Err(std::string("JSON parse error: ") + e.what());
    }
}

std::optional<json> HttpTransport::parse_sse_event(const std::string& data) {
    // SSE 格式:
    //   event: message
    //   data: {"jsonrpc":"2.0","id":1,"result":{...}}
    //   (blank line)
    std::istringstream stream(data);
    std::string line;
    std::string event_data;

    while (std::getline(stream, line)) {
        // trim \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.substr(0, 5) == "data:") {
            std::string d = line.substr(5);
            if (!d.empty() && d.front() == ' ') d.erase(0, 1);
            event_data += d;
        }
    }

    if (event_data.empty()) return std::nullopt;

    try {
        return json::parse(event_data);
    } catch (...) {
        return std::nullopt;
    }
}

core::Result<json> HttpTransport::send_request(
    const std::string& method, const json& params, int timeout_ms) {

    if (closed_.load()) {
        return core::Result<json>::Err("transport closed");
    }

    int id = next_id_.fetch_add(1);
    json request = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

    return send_http_request(request, timeout_ms);
}

core::Result<void> HttpTransport::send_notification(
    const std::string& method, const json& params) {

    if (closed_.load()) {
        return core::Result<void>::Err("transport closed");
    }

    json notification = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };

    // 通知不需要响应，但仍发送 HTTP POST
    infra::HttpRequestOptions options;
    options.timeout_ms = config_.connection_timeout_ms;
    options.body = notification.dump();
    options.headers["Content-Type"] = "application/json";
    for (const auto& [k, v] : config_.headers) options.headers[k] = v;
    if (session_id_.has_value()) options.headers["Mcp-Session-Id"] = *session_id_;

    auto resp = http_client_->post(config_.url, options);
    if (resp.is_err()) {
        return core::Result<void>::Err("notification failed: " + resp.error);
    }
    return core::Result<void>::Ok();
}

void HttpTransport::start_sse_stream() {
    sse_running_ = true;
    sse_thread_ = std::thread([this]() {
        while (sse_running_) {
            infra::HttpRequestOptions options;
            options.timeout_ms = 0;  // 长连接
            options.headers["Accept"] = "text/event-stream";
            for (const auto& [k, v] : config_.headers) options.headers[k] = v;

            // SSE GET 端点通常与 POST 不同，这里简化使用同一 URL
            http_client_->post_stream(config_.url, options,
                [](const std::string& line) {
                    // TODO: 解析 SSE 事件，匹配到 pending request
                    (void)line;
                });

            if (sse_running_) {
                std::this_thread::sleep_for(std::chrono::seconds(1));  // 重连等待
            }
        }
    });
}

bool HttpTransport::is_alive() const {
    return !closed_.load();
}

void HttpTransport::close() {
    if (closed_.exchange(true)) return;

    sse_running_ = false;
    if (sse_thread_.joinable()) sse_thread_.join();

    log()->debug("[HttpTransport] closed");
}

// ============================================================================
// McpServerConnection 实现
// ============================================================================

McpServerConnection::McpServerConnection(const Config& config) : config_(config) {}

McpServerConnection::~McpServerConnection() { disconnect(); }

core::Result<std::unique_ptr<McpTransport>> McpServerConnection::create_transport() {
    const auto& sc = config_.server_config;

    // 构造 raw_server map，复用 mcp_transport_config.cc 的解析逻辑
    std::map<std::string, std::string> raw;
    if (sc.command) raw["command"] = *sc.command;
    if (sc.url) raw["url"] = *sc.url;
    if (sc.transport) raw["transport"] = *sc.transport;
    if (sc.cwd) raw["cwd"] = *sc.cwd;
    if (sc.working_directory) raw["workingDirectory"] = *sc.working_directory;
    if (sc.connection_timeout_ms) {
        raw["connectionTimeoutMs"] = std::to_string(*sc.connection_timeout_ms);
    }

    auto resolved = resolve_mcp_transport_config(config_.name, raw);
    if (!resolved.has_value()) {
        return core::Result<std::unique_ptr<McpTransport>>::Err(
            "cannot resolve transport config for server: " + config_.name);
    }

    if (resolved->kind == ResolvedMcpTransportConfig::Kind::Stdio) {
        StdioTransport::Config tc;
        tc.command = resolved->command;
        tc.args = sc.args;           // 直接从原配置取 args
        tc.env = sc.env;
        tc.cwd = resolved->cwd;
        tc.connection_timeout_ms = resolved->connection_timeout_ms;
        return core::Result<std::unique_ptr<McpTransport>>::Ok(
            std::make_unique<StdioTransport>(tc));
    } else {
        HttpTransport::Config tc;
        tc.url = resolved->url;
        tc.headers = sc.headers;
        tc.transport_type = resolved->transport_type;
        tc.connection_timeout_ms = resolved->connection_timeout_ms;
        return core::Result<std::unique_ptr<McpTransport>>::Ok(
            std::make_unique<HttpTransport>(tc));
    }
}

core::Result<void> McpServerConnection::connect() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ == McpConnectionState::Ready) {
        return core::Result<void>::Ok();  // 已连接
    }

    state_ = McpConnectionState::Connecting;

    // 1. 创建传输
    auto transport_result = create_transport();
    if (transport_result.is_err()) {
        state_ = McpConnectionState::Error;
        return core::Result<void>::Err(transport_result.error);
    }
    transport_ = std::move(transport_result.get());

    // 2. 启动传输
    auto start_result = transport_->start();
    if (start_result.is_err()) {
        state_ = McpConnectionState::Error;
        return core::Result<void>::Err("transport start failed: " + start_result.error);
    }

    // 3. 执行 MCP 握手
    auto handshake_result = perform_handshake();
    if (handshake_result.is_err()) {
        state_ = McpConnectionState::Error;
        transport_->close();
        return core::Result<void>::Err("handshake failed: " + handshake_result.error);
    }

    state_ = McpConnectionState::Ready;
    log()->info("[McpServerConnection] {} ready (protocol={}, server={} v{})",
                config_.name, capabilities_.protocol_version,
                capabilities_.server_name, capabilities_.server_version);
    return core::Result<void>::Ok();
}

core::Result<void> McpServerConnection::perform_handshake() {
    // Step 1: initialize 请求
    json init_params = {
        {"protocolVersion", MCP_PROTOCOL_VERSION},
        {"capabilities", {
            {"tools", json::object()},
            {"resources", json::object()}
        }},
        {"clientInfo", {
            {"name", CLIENT_NAME},
            {"version", CLIENT_VERSION}
        }}
    };

    auto init_result = transport_->send_request(
        "initialize", init_params, config_.connection_timeout_ms);
    if (init_result.is_err()) {
        return core::Result<void>::Err(init_result.error);
    }

    const auto& init_response = init_result.get();

    // 解析 server capabilities
    if (init_response.contains("protocolVersion")) {
        capabilities_.protocol_version = init_response["protocolVersion"].get<std::string>();
    }
    if (init_response.contains("serverInfo")) {
        const auto& info = init_response["serverInfo"];
        if (info.contains("name")) capabilities_.server_name = info["name"].get<std::string>();
        if (info.contains("version")) capabilities_.server_version = info["version"].get<std::string>();
    }
    if (init_response.contains("capabilities")) {
        const auto& caps = init_response["capabilities"];
        capabilities_.tools = caps.contains("tools");
        capabilities_.resources = caps.contains("resources");
        capabilities_.prompts = caps.contains("prompts");
    }

    // Step 2: notifications/initialized 通知
    transport_->send_notification("notifications/initialized", json::object());

    return core::Result<void>::Ok();
}

core::Result<std::vector<McpToolDefinition>> McpServerConnection::list_tools() {
    if (!is_ready()) {
        return core::Result<std::vector<McpToolDefinition>>::Err("not connected");
    }

    auto result = transport_->send_request(
        "tools/list", json::object(), config_.call_timeout_ms);
    if (result.is_err()) {
        return core::Result<std::vector<McpToolDefinition>>::Err(result.error);
    }

    const auto& response = result.get();
    std::vector<McpToolDefinition> tools;

    if (!response.contains("tools") || !response["tools"].is_array()) {
        return core::Result<std::vector<McpToolDefinition>>::Ok(tools);
    }

    for (const auto& tool : response["tools"]) {
        McpToolDefinition def;
        def.server_name = config_.name;
        if (tool.contains("name")) def.tool_name = tool["name"].get<std::string>();
        def.name = config_.name + "." + def.tool_name;
        if (tool.contains("description")) def.description = tool["description"].get<std::string>();
        if (tool.contains("inputSchema")) {
            def.input_schema = JsonConverter::from_nlohmann(tool["inputSchema"]);
        }
        tools.push_back(std::move(def));
    }

    return core::Result<std::vector<McpToolDefinition>>::Ok(tools);
}

core::Result<json> McpServerConnection::call_tool(
    const std::string& tool_name, const json& arguments) {

    if (!is_ready()) {
        return core::Result<json>::Err("not connected");
    }

    json params = {
        {"name", tool_name},
        {"arguments", arguments}
    };

    return transport_->send_request("tools/call", params, config_.call_timeout_ms);
}

void McpServerConnection::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (transport_) {
        transport_->close();
        transport_.reset();
    }
    state_ = McpConnectionState::Closed;
}

// ============================================================================
// NativeMcpClient 实现
// ============================================================================

NativeMcpClient::NativeMcpClient(const McporterConfig& config) : config_(config) {}

NativeMcpClient::~NativeMcpClient() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (auto& [name, conn] : connections_) {
        conn->disconnect();
    }
    connections_.clear();
}

// ========== Daemon 管理 (原生模式下 = 连接池管理) ==========

core::Result<void> NativeMcpClient::start_daemon() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    started_ = true;
    log()->info("[NativeMcpClient] started (native mode, no mcporter daemon)");
    return core::Result<void>::Ok();
}

core::Result<void> NativeMcpClient::stop_daemon() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (auto& [name, conn] : connections_) {
        conn->disconnect();
    }
    connections_.clear();
    started_ = false;
    return core::Result<void>::Ok();
}

core::Result<bool> NativeMcpClient::is_daemon_running() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return core::Result<bool>::Ok(started_);
}

core::Result<void> NativeMcpClient::restart_daemon() {
    auto stop_r = stop_daemon();
    (void)stop_r;
    return start_daemon();
}

core::Result<void> NativeMcpClient::ensure_daemon_started() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (!started_) started_ = true;
    return core::Result<void>::Ok();
}

// ========== 内部辅助 ==========

std::map<std::string, McpServerConfig> NativeMcpClient::load_server_configs() {
    std::map<std::string, McpServerConfig> result;

    auto read_result = list_configured_mcp_servers();
    if (!read_result.ok) return result;

    for (const auto& [name, raw_json] : read_result.mcp_servers) {
        try {
            result[name] = McpServerConfig::from_json(raw_json);
        } catch (const std::exception& e) {
            log()->warn("[NativeMcpClient] invalid config for {}: {}", name, e.what());
        }
    }
    return result;
}

std::pair<std::string, std::string> NativeMcpClient::split_server_tool(
    const std::string& server_and_tool) {
    // 支持 "server.tool" 或 "server:tool"
    auto dot_pos = server_and_tool.find('.');
    auto colon_pos = server_and_tool.find(':');
    size_t split_pos = std::string::npos;

    if (dot_pos != std::string::npos && colon_pos != std::string::npos) {
        split_pos = std::min(dot_pos, colon_pos);
    } else if (dot_pos != std::string::npos) {
        split_pos = dot_pos;
    } else if (colon_pos != std::string::npos) {
        split_pos = colon_pos;
    }

    if (split_pos == std::string::npos) {
        return {server_and_tool, ""};  // 无分隔符，整串作为 server 名
    }
    return {
        server_and_tool.substr(0, split_pos),
        server_and_tool.substr(split_pos + 1)
    };
}

core::Result<std::shared_ptr<McpServerConnection>> NativeMcpClient::get_or_connect(
    const std::string& server_name) {

    // 检查缓存
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto it = connections_.find(server_name);
        if (it != connections_.end() && it->second->is_ready()) {
            return core::Result<std::shared_ptr<McpServerConnection>>::Ok(it->second);
        }
    }

    // 加载配置
    auto server_configs = load_server_configs();
    auto cfg_it = server_configs.find(server_name);
    if (cfg_it == server_configs.end()) {
        return core::Result<std::shared_ptr<McpServerConnection>>::Err(
            "server not configured: " + server_name);
    }

    // 创建连接
    McpServerConnection::Config conn_config;
    conn_config.name = server_name;
    conn_config.server_config = cfg_it->second;
    conn_config.connection_timeout_ms = config_.timeout_ms;
    conn_config.call_timeout_ms = config_.timeout_ms;

    auto conn = std::make_shared<McpServerConnection>(conn_config);
    auto connect_result = conn->connect();
    if (connect_result.is_err()) {
        return core::Result<std::shared_ptr<McpServerConnection>>::Err(
            connect_result.error);
    }

    // 缓存连接
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        connections_[server_name] = conn;
    }

    return core::Result<std::shared_ptr<McpServerConnection>>::Ok(conn);
}

// ========== 工具发现 ==========

core::Result<std::vector<McpServerInfo>> NativeMcpClient::list_servers() {
    auto configs = load_server_configs();
    std::vector<McpServerInfo> result;

    for (const auto& [name, cfg] : configs) {
        McpServerInfo info;
        info.name = name;
        info.command = cfg.command.value_or(cfg.url.value_or(""));
        info.lifecycle = "keep-alive";  // 原生模式始终保持连接
        info.metadata = core::JsonValue(cfg.to_json().dump());
        result.push_back(std::move(info));
    }

    return core::Result<std::vector<McpServerInfo>>::Ok(result);
}

core::Result<std::vector<McpToolDefinition>> NativeMcpClient::list_tools(
    const std::string& server_name, bool /*with_schema*/) {

    auto conn_result = get_or_connect(server_name);
    if (conn_result.is_err()) {
        return core::Result<std::vector<McpToolDefinition>>::Err(conn_result.error);
    }

    return conn_result.get()->list_tools();
}

core::Result<std::vector<McpToolDefinition>> NativeMcpClient::discover_all_tools() {
    ensure_daemon_started();

    auto servers_result = list_servers();
    if (servers_result.is_err()) {
        return core::Result<std::vector<McpToolDefinition>>::Err(servers_result.error);
    }

    std::vector<McpToolDefinition> all_tools;
    for (const auto& server : servers_result.get()) {
        // 白名单过滤
        if (!config_.allowed_servers.empty()) {
            if (std::find(config_.allowed_servers.begin(),
                          config_.allowed_servers.end(),
                          server.name) == config_.allowed_servers.end()) {
                log()->debug("[NativeMcpClient] skipping server {} (not in whitelist)",
                             server.name);
                continue;
            }
        }

        auto tools_r = list_tools(server.name, true);
        if (tools_r.is_ok()) {
            const auto& tools = tools_r.get();
            all_tools.insert(all_tools.end(), tools.begin(), tools.end());
        } else {
            log()->warn("[NativeMcpClient] failed to list tools for {}: {}",
                        server.name, tools_r.error);
        }
    }

    log()->info("[NativeMcpClient] discovered {} tools from {} servers",
                all_tools.size(), servers_result.get().size());
    return core::Result<std::vector<McpToolDefinition>>::Ok(all_tools);
}

// ========== 工具调用 ==========

core::Result<McporterResult> NativeMcpClient::call_tool(
    const std::string& server_and_tool, const core::JsonValue& args) {

    ensure_daemon_started();

    auto [server_name, tool_name] = split_server_tool(server_and_tool);
    if (server_name.empty() || tool_name.empty()) {
        McporterResult r;
        r.success = false;
        r.error = "invalid tool name (expected 'server.tool'): " + server_and_tool;
        return core::Result<McporterResult>::Ok(r);
    }

    auto conn_result = get_or_connect(server_name);
    if (conn_result.is_err()) {
        McporterResult r;
        r.success = false;
        r.error = "connection failed: " + conn_result.error;
        return core::Result<McporterResult>::Ok(r);
    }

    auto args_json = JsonConverter::to_nlohmann(args);
    auto call_result = conn_result.get()->call_tool(tool_name, args_json);

    McporterResult mcp_result;
    if (call_result.is_err()) {
        mcp_result.success = false;
        mcp_result.error = call_result.error;
        return core::Result<McporterResult>::Ok(mcp_result);
    }

    const auto& response = call_result.get();
    mcp_result.success = true;
    mcp_result.exit_code = 0;

    // MCP tools/call 响应格式: { content: [...], isError: bool }
    if (response.contains("isError") && response["isError"].get<bool>()) {
        mcp_result.success = false;
        if (response.contains("content") && response["content"].is_array() &&
            !response["content"].empty()) {
            const auto& first = response["content"][0];
            if (first.contains("text")) {
                mcp_result.error = first["text"].get<std::string>();
            }
        }
    }

    // 优先取 structuredContent，否则取 content
    if (response.contains("structuredContent")) {
        mcp_result.structured_content =
            JsonConverter::from_nlohmann(response["structuredContent"]);
    } else {
        mcp_result.structured_content = JsonConverter::from_nlohmann(response);
    }

    return core::Result<McporterResult>::Ok(mcp_result);
}

core::Result<std::vector<McporterResult>> NativeMcpClient::call_tools(
    const std::vector<McporterCallArgs>& calls) {

    std::vector<McporterResult> results;
    results.reserve(calls.size());

    // 串行执行（每个 server 的连接内部不支持并发请求的场景下最安全）
    // 未来可改为按 server 分组并发
    for (const auto& call : calls) {
        auto r = call_tool(call.server_and_tool, call.args);
        if (r.is_ok()) {
            results.push_back(r.get());
        } else {
            McporterResult err;
            err.success = false;
            err.error = r.error;
            results.push_back(err);
        }
    }

    return core::Result<std::vector<McporterResult>>::Ok(results);
}

// ========== 配置管理（直接操作 config.json）==========

core::Result<core::JsonValue> NativeMcpClient::get_config() {
    auto read_result = list_configured_mcp_servers();
    if (!read_result.ok) {
        return core::Result<core::JsonValue>::Err(read_result.error);
    }

    json config;
    config["servers"] = json::object();
    for (const auto& [name, server_json] : read_result.mcp_servers) {
        config["servers"][name] = server_json;
    }
    return core::Result<core::JsonValue>::Ok(
        JsonConverter::from_nlohmann(config));
}

core::Result<void> NativeMcpClient::add_server(
    const std::string& name, const std::string& command,
    const core::JsonValue& options) {

    json server_config;
    server_config["command"] = command;

    auto opts_json = JsonConverter::to_nlohmann(options);
    if (opts_json.is_object()) {
        if (opts_json.contains("args")) server_config["args"] = opts_json["args"];
        if (opts_json.contains("env")) server_config["env"] = opts_json["env"];
        if (opts_json.contains("cwd")) server_config["cwd"] = opts_json["cwd"];
    }

    auto write_result = set_configured_mcp_server(name, server_config);
    if (!write_result.ok) {
        return core::Result<void>::Err(write_result.error);
    }

    log()->info("[NativeMcpClient] added server: {}", name);
    return core::Result<void>::Ok();
}

core::Result<void> NativeMcpClient::remove_server(const std::string& name) {
    // 断开可能存在的连接
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto it = connections_.find(name);
        if (it != connections_.end()) {
            it->second->disconnect();
            connections_.erase(it);
        }
    }

    auto write_result = unset_configured_mcp_server(name);
    if (!write_result.ok) {
        return core::Result<void>::Err(write_result.error);
    }

    log()->info("[NativeMcpClient] removed server: {}", name);
    return core::Result<void>::Ok();
}

core::Result<void> NativeMcpClient::import_config(const std::string& /*config_path*/) {
    // 原生模式下，config 直接从 ~/.hunterclaw/config.json 读取，
    // 不需要导入到 mcporter 的内部存储
    return core::Result<void>::Ok();
}

// ========== 健康检查 ==========

core::Result<bool> NativeMcpClient::health_check() {
    // 原生模式总是"健康"（无外部依赖）
    return core::Result<bool>::Ok(true);
}

core::Result<std::string> NativeMcpClient::get_version() {
    return core::Result<std::string>::Ok(
        std::string("native-mcp-") + CLIENT_VERSION);
}

// ============================================================================
// 工厂函数
// ============================================================================
//
// 注意: NativeMcpClient 的直接工厂 create_native_mcp_client() 以及接口层
// 工厂 McporterClient::create() / create_mcporter_client() 都位于独立的
// mcporter_client.cc 文件中,以避免实现层 (本文件) 与工厂入口耦合。
//
// 本文件只负责 NativeMcpClient 的类实现;所有外部可见的工厂调用都通过
// mcporter_client.cc 完成。
//
// ============================================================================

} // namespace mcp
} // namespace hunterclaw
