// 🎯 对齐 OpenClaw: agents/mcp-transport-config.ts + mcp-stdio.ts + mcp-http.ts
// MCP 传输配置统一解析

#include "hunterclaw/mcp/mcp_transport_config.h"
#include <algorithm>

namespace hunterclaw {
namespace mcp {

static const int DEFAULT_CONNECTION_TIMEOUT_MS = 30000;

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// ============================================================================
// resolveStdioMcpServerLaunchConfig (对齐 OpenClaw agents/mcp-stdio.ts:14-41)
// ============================================================================

StdioMcpLaunchResult resolve_stdio_mcp_server_launch_config(
    const std::map<std::string, std::string>& raw) {

    StdioMcpLaunchResult result;

    auto cmd_it = raw.find("command");
    if (cmd_it == raw.end() || trim(cmd_it->second).empty()) {
        // 🎯 对齐 OpenClaw: if url present, "not a stdio server (has url)"
        auto url_it = raw.find("url");
        if (url_it != raw.end() && !trim(url_it->second).empty()) {
            result.ok = false;
            result.reason = "not a stdio server (has url)";
            return result;
        }
        result.ok = false;
        result.reason = "its command is missing";
        return result;
    }

    result.ok = true;
    result.command = cmd_it->second;

    // cwd / workingDirectory
    auto cwd_it = raw.find("cwd");
    if (cwd_it != raw.end() && !trim(cwd_it->second).empty()) {
        result.cwd = cwd_it->second;
    } else {
        auto wd_it = raw.find("workingDirectory");
        if (wd_it != raw.end() && !trim(wd_it->second).empty()) {
            result.cwd = wd_it->second;
        }
    }
    return result;
}

// ============================================================================
// resolveHttpMcpServerLaunchConfig (对齐 OpenClaw agents/mcp-http.ts:19-69)
// ============================================================================

HttpMcpLaunchResult resolve_http_mcp_server_launch_config(
    const std::map<std::string, std::string>& raw,
    McpTransportType type) {

    HttpMcpLaunchResult result;
    auto url_it = raw.find("url");
    if (url_it == raw.end() || trim(url_it->second).empty()) {
        result.ok = false;
        result.reason = "its url is missing";
        return result;
    }

    std::string url = trim(url_it->second);
    // 🎯 简化 URL 验证（完整版需要 URL parser）
    if (url.substr(0, 7) != "http://" && url.substr(0, 8) != "https://") {
        result.ok = false;
        result.reason = "only http and https URLs are supported";
        return result;
    }

    result.ok = true;
    result.transport_type = type;
    result.url = url;
    return result;
}

// ============================================================================
// resolveMcpTransportConfig (对齐 OpenClaw agents/mcp-transport-config.ts:96-144)
// ============================================================================

std::optional<ResolvedMcpTransportConfig> resolve_mcp_transport_config(
    const std::string& server_name,
    const std::map<std::string, std::string>& raw_server) {

    // 🎯 对齐 OpenClaw: 获取请求的 transport 类型
    std::string requested_transport;
    auto transport_it = raw_server.find("transport");
    if (transport_it != raw_server.end()) {
        requested_transport = to_lower(trim(transport_it->second));
    }

    // 获取 connectionTimeoutMs
    int timeout_ms = DEFAULT_CONNECTION_TIMEOUT_MS;
    auto timeout_it = raw_server.find("connectionTimeoutMs");
    if (timeout_it != raw_server.end()) {
        try { timeout_ms = std::stoi(timeout_it->second); } catch (...) {}
    }

    // 🎯 对齐 OpenClaw: 先尝试 stdio
    auto stdio_launch = resolve_stdio_mcp_server_launch_config(raw_server);
    if (stdio_launch.ok) {
        ResolvedMcpTransportConfig config;
        config.kind = ResolvedMcpTransportConfig::Kind::Stdio;
        config.transport_type = McpTransportType::Stdio;
        config.command = stdio_launch.command;
        config.args = stdio_launch.args;
        config.env = stdio_launch.env;
        config.cwd = stdio_launch.cwd;
        config.description = stdio_launch.command;
        config.connection_timeout_ms = timeout_ms;
        return config;
    }

    // 🎯 对齐 OpenClaw: 检查不支持的 transport
    if (!requested_transport.empty() &&
        requested_transport != "sse" &&
        requested_transport != "streamable-http") {
        return std::nullopt;
    }

    // 🎯 对齐 OpenClaw: 尝试 streamable-http
    if (requested_transport == "streamable-http") {
        auto http_result = resolve_http_mcp_server_launch_config(raw_server, McpTransportType::StreamableHttp);
        if (http_result.ok) {
            ResolvedMcpTransportConfig config;
            config.kind = ResolvedMcpTransportConfig::Kind::Http;
            config.transport_type = McpTransportType::StreamableHttp;
            config.url = http_result.url;
            config.headers = http_result.headers;
            config.description = http_result.url;
            config.connection_timeout_ms = timeout_ms;
            return config;
        }
    }

    // 🎯 对齐 OpenClaw: 尝试 SSE
    auto sse_result = resolve_http_mcp_server_launch_config(raw_server, McpTransportType::Sse);
    if (sse_result.ok) {
        ResolvedMcpTransportConfig config;
        config.kind = ResolvedMcpTransportConfig::Kind::Http;
        config.transport_type = McpTransportType::Sse;
        config.url = sse_result.url;
        config.headers = sse_result.headers;
        config.description = sse_result.url;
        config.connection_timeout_ms = timeout_ms;
        return config;
    }

    return std::nullopt;
}

} // namespace mcp
} // namespace hunterclaw
