#pragma once

// 🎯 对齐 OpenClaw: agents/mcp-transport-config.ts (统一传输配置解析)
// 🎯 对齐 OpenClaw: agents/mcp-stdio.ts (stdio 传输)
// 🎯 对齐 OpenClaw: agents/mcp-http.ts (HTTP SSE / streamable-http 传输)

#include <string>
#include <vector>
#include <map>
#include <optional>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// Transport Type (对齐 OpenClaw line 13)
// ============================================================================

enum class McpTransportType {
    Stdio,            // 🎯 "stdio"
    Sse,              // 🎯 "sse"
    StreamableHttp,   // 🎯 "streamable-http"
};

// ============================================================================
// Resolved Transport Config (对齐 OpenClaw line 15-38)
// ============================================================================

struct ResolvedMcpTransportConfig {
    enum class Kind { Stdio, Http };

    Kind kind;
    McpTransportType transport_type;
    std::string description;
    int connection_timeout_ms = 30000;   // 🎯 DEFAULT_CONNECTION_TIMEOUT_MS = 30_000

    // Stdio fields (对齐 OpenClaw line 20-27)
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string cwd;

    // HTTP fields (对齐 OpenClaw line 29-34)
    std::string url;
    std::map<std::string, std::string> headers;
};

// ============================================================================
// Stdio Launch Config (对齐 OpenClaw agents/mcp-stdio.ts:3-8)
// ============================================================================

struct StdioMcpLaunchResult {
    bool ok = false;
    std::string reason;
    // Filled when ok=true
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string cwd;
};

// 🎯 对齐 OpenClaw: resolveStdioMcpServerLaunchConfig(raw)
StdioMcpLaunchResult resolve_stdio_mcp_server_launch_config(
    const std::map<std::string, std::string>& raw);

// ============================================================================
// HTTP Launch Config (对齐 OpenClaw agents/mcp-http.ts:9-13)
// ============================================================================

struct HttpMcpLaunchResult {
    bool ok = false;
    std::string reason;
    McpTransportType transport_type = McpTransportType::Sse;
    std::string url;
    std::map<std::string, std::string> headers;
};

// 🎯 对齐 OpenClaw: resolveHttpMcpServerLaunchConfig(raw, options?)
HttpMcpLaunchResult resolve_http_mcp_server_launch_config(
    const std::map<std::string, std::string>& raw,
    McpTransportType type = McpTransportType::Sse);

// ============================================================================
// Unified Transport Config Resolver (对齐 OpenClaw line 96-144)
// ============================================================================

// 🎯 对齐 OpenClaw: resolveMcpTransportConfig(serverName, rawServer)
std::optional<ResolvedMcpTransportConfig> resolve_mcp_transport_config(
    const std::string& server_name,
    const std::map<std::string, std::string>& raw_server);

} // namespace mcp
} // namespace hunterclaw
