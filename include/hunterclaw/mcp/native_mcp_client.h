#pragma once

// ============================================================================
// Native MCP Client — 纯 C++ MCP 客户端，无外部依赖
//
// 替代 mcporter CLI 桥接方案，直接在进程内实现 MCP 协议：
//   1. StdioTransport: fork 子进程 + JSON-RPC over stdin/stdout
//   2. HttpTransport: StreamableHttp / SSE（使用 infra/http_client）
//   3. MCP 协议握手: initialize → tools/list → tools/call
//   4. 子进程生命周期: 启动/健康检查/超时杀死/僵尸回收
//   5. 连接池: 多 Server 并发管理，keep-alive 模式
//
// 实现 McporterClient 接口，支持无缝替换。
// ============================================================================

#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/mcp/mcp_config.h"
#include "hunterclaw/mcp/mcp_transport_config.h"  // McpTransportType
#include "hunterclaw/core/subprocess.h"
#include "hunterclaw/core/result.h"
#include "hunterclaw/infra/http_client.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// JSON-RPC 类型 (MCP 协议基础)
// ============================================================================

struct JsonRpcMessage {
    std::string jsonrpc = "2.0";
    std::optional<int> id;
    std::string method;
    nlohmann::json params;
    // Response fields
    std::optional<nlohmann::json> result;
    std::optional<nlohmann::json> error;
};

// ============================================================================
// MCP Server 连接状态
// ============================================================================

enum class McpConnectionState {
    Disconnected,   // 未连接
    Connecting,     // 正在初始化（等待 initialize 响应）
    Ready,          // 已就绪（可以调用 tools）
    Error,          // 连接错误
    Closed          // 已关闭
};

// ============================================================================
// MCP Server 能力 (initialize 响应)
// ============================================================================

struct McpServerCapabilities {
    bool tools = false;
    bool resources = false;
    bool prompts = false;
    std::string protocol_version;
    std::string server_name;
    std::string server_version;
};

// ============================================================================
// McpTransport — 传输层抽象接口
// ============================================================================

class McpTransport {
public:
    virtual ~McpTransport() = default;

    // 启动传输
    virtual core::Result<void> start() = 0;

    // 发送 JSON-RPC 请求并等待响应
    virtual core::Result<nlohmann::json> send_request(
        const std::string& method,
        const nlohmann::json& params,
        int timeout_ms) = 0;

    // 发送通知（不等待响应）
    virtual core::Result<void> send_notification(
        const std::string& method,
        const nlohmann::json& params) = 0;

    // 是否存活
    virtual bool is_alive() const = 0;

    // 关闭
    virtual void close() = 0;
};

// ============================================================================
// StdioTransport — 通过子进程 stdin/stdout 通信
// ============================================================================

class StdioTransport : public McpTransport {
public:
    struct Config {
        std::string command;
        std::vector<std::string> args;
        std::map<std::string, std::string> env;
        std::string cwd;
        int connection_timeout_ms = 30000;
    };

    explicit StdioTransport(const Config& config);
    ~StdioTransport() override;

    core::Result<void> start() override;
    core::Result<nlohmann::json> send_request(
        const std::string& method,
        const nlohmann::json& params,
        int timeout_ms) override;
    core::Result<void> send_notification(
        const std::string& method,
        const nlohmann::json& params) override;
    bool is_alive() const override;
    void close() override;
    int pid() const;

private:
    Config config_;
    core::subprocess::Process* process_ = nullptr;
    std::atomic<int> next_id_{1};

    // 待处理的响应 (id → promise)
    struct PendingRequest {
        std::promise<nlohmann::json> promise;
        std::chrono::steady_clock::time_point deadline;
    };
    std::map<int, std::shared_ptr<PendingRequest>> pending_requests_;
    std::mutex pending_mutex_;

    // 读取线程
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
    std::string read_buffer_;
    mutable std::mutex write_mutex_;

    // 内部方法
    void reader_loop();
    void dispatch_message(const nlohmann::json& msg);
    core::Result<void> write_message(const nlohmann::json& msg);
    std::optional<nlohmann::json> try_parse_message();
    void fail_all_pending(const std::string& error);
};

// ============================================================================
// HttpTransport — 通过 HTTP POST + SSE 通信 (MCP StreamableHttp/SSE)
// ============================================================================

class HttpTransport : public McpTransport {
public:
    struct Config {
        std::string url;                                  // e.g. "https://mcp.example.com/rpc"
        std::map<std::string, std::string> headers;       // 自定义 headers (如 Authorization)
        McpTransportType transport_type = McpTransportType::StreamableHttp;
        int connection_timeout_ms = 30000;
    };

    explicit HttpTransport(const Config& config);
    ~HttpTransport() override;

    core::Result<void> start() override;
    core::Result<nlohmann::json> send_request(
        const std::string& method,
        const nlohmann::json& params,
        int timeout_ms) override;
    core::Result<void> send_notification(
        const std::string& method,
        const nlohmann::json& params) override;
    bool is_alive() const override;
    void close() override;

private:
    Config config_;
    infra::HttpClient::Ptr http_client_;
    std::atomic<int> next_id_{1};
    std::atomic<bool> closed_{false};
    std::optional<std::string> session_id_;  // MCP-Session-Id header
    std::mutex mutex_;

    // SSE 模式下的后台事件流
    std::thread sse_thread_;
    std::atomic<bool> sse_running_{false};

    core::Result<nlohmann::json> send_http_request(
        const nlohmann::json& message, int timeout_ms);
    void start_sse_stream();
    std::optional<nlohmann::json> parse_sse_event(const std::string& data);
};

// ============================================================================
// McpServerConnection — 单个 MCP Server 的连接
// ============================================================================

class McpServerConnection {
public:
    struct Config {
        std::string name;              // server 名称
        McpServerConfig server_config; // 从用户配置解析
        int connection_timeout_ms = 30000;
        int call_timeout_ms = 30000;
    };

    explicit McpServerConnection(const Config& config);
    ~McpServerConnection();

    // 连接并初始化
    core::Result<void> connect();

    // 获取连接状态
    McpConnectionState state() const { return state_; }

    // 获取 server 能力
    const McpServerCapabilities& capabilities() const { return capabilities_; }

    // 列出工具
    core::Result<std::vector<McpToolDefinition>> list_tools();

    // 调用工具
    core::Result<nlohmann::json> call_tool(
        const std::string& tool_name,
        const nlohmann::json& arguments);

    // 断开连接
    void disconnect();

    // 是否已连接且就绪
    bool is_ready() const { return state_ == McpConnectionState::Ready; }

    // server 名称
    const std::string& name() const { return config_.name; }

private:
    Config config_;
    std::unique_ptr<McpTransport> transport_;
    McpConnectionState state_ = McpConnectionState::Disconnected;
    McpServerCapabilities capabilities_;
    std::mutex mutex_;

    // 创建传输层 (根据配置自动选择 Stdio / Http)
    core::Result<std::unique_ptr<McpTransport>> create_transport();

    // MCP 协议握手
    core::Result<void> perform_handshake();
};

// ============================================================================
// NativeMcpClient — 实现 McporterClient 接口
// ============================================================================

class NativeMcpClient : public McporterClient {
public:
    explicit NativeMcpClient(const McporterConfig& config);
    ~NativeMcpClient() override;

    // ========== Daemon 管理 (在原生模式下 = 连接池管理) ==========
    core::Result<void> start_daemon() override;
    core::Result<void> stop_daemon() override;
    core::Result<bool> is_daemon_running() override;
    core::Result<void> restart_daemon() override;
    core::Result<void> ensure_daemon_started() override;

    // ========== 工具发现 ==========
    core::Result<std::vector<McpServerInfo>> list_servers() override;
    core::Result<std::vector<McpToolDefinition>> list_tools(
        const std::string& server_name, bool with_schema = true) override;
    core::Result<std::vector<McpToolDefinition>> discover_all_tools() override;

    // ========== 工具调用 ==========
    core::Result<McporterResult> call_tool(
        const std::string& server_and_tool, const core::JsonValue& args) override;
    core::Result<std::vector<McporterResult>> call_tools(
        const std::vector<McporterCallArgs>& calls) override;

    // ========== 配置管理 ==========
    core::Result<core::JsonValue> get_config() override;
    core::Result<void> add_server(const std::string& name, const std::string& command,
                                   const core::JsonValue& options = {}) override;
    core::Result<void> remove_server(const std::string& name) override;
    core::Result<void> import_config(const std::string& config_path) override;

    // ========== 健康检查 ==========
    core::Result<bool> health_check() override;
    core::Result<std::string> get_version() override;

private:
    McporterConfig config_;
    mutable std::mutex pool_mutex_;
    std::map<std::string, std::shared_ptr<McpServerConnection>> connections_;
    bool started_ = false;

    // 获取或创建连接
    core::Result<std::shared_ptr<McpServerConnection>> get_or_connect(
        const std::string& server_name);

    // 从配置加载 server 列表
    std::map<std::string, McpServerConfig> load_server_configs();

    // 解析 "server.tool" → (server, tool)
    static std::pair<std::string, std::string> split_server_tool(
        const std::string& server_and_tool);
};

// ============================================================================
// 工厂函数
// ============================================================================

/// 创建原生 MCP 客户端 (等价于 McporterClient::create)
std::shared_ptr<McporterClient> create_native_mcp_client(const McporterConfig& config);

} // namespace mcp
} // namespace hunterclaw
