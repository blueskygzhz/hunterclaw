#pragma once

// 🎯 对齐 OpenClaw: gateway/mcp-http.ts (MCP HTTP Loopback Server)
// 🎯 对齐 OpenClaw: gateway/mcp-http.protocol.ts (JSON-RPC protocol)
// 🎯 对齐 OpenClaw: gateway/mcp-http.handlers.ts (initialize/tools/list/tools/call)
// 🎯 对齐 OpenClaw: gateway/mcp-http.loopback-runtime.ts (global runtime state)
// 🎯 对齐 OpenClaw: gateway/mcp-http.schema.ts (tool schema builder)
// 🎯 对齐 OpenClaw: gateway/mcp-http.request.ts (request validation)
// 🎯 对齐 OpenClaw: gateway/mcp-http.runtime.ts (McpLoopbackToolCache)

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// MCP Protocol Constants (对齐 OpenClaw gateway/mcp-http.protocol.ts)
// ============================================================================

inline const std::string MCP_LOOPBACK_SERVER_NAME = "openclaw";
inline const std::string MCP_LOOPBACK_SERVER_VERSION = "0.1.0";
inline const std::vector<std::string> MCP_LOOPBACK_SUPPORTED_PROTOCOL_VERSIONS = {
    "2025-03-26", "2024-11-05"
};

// ============================================================================
// JSON-RPC types (对齐 OpenClaw gateway/mcp-http.protocol.ts:5-12)
// ============================================================================

struct JsonRpcRequest {
    std::string jsonrpc = "2.0";
    std::optional<std::string> id;
    std::string method;
    nlohmann::json params;
};

nlohmann::json json_rpc_result(const std::optional<std::string>& id, const nlohmann::json& result);
nlohmann::json json_rpc_error(const std::optional<std::string>& id, int code, const std::string& message);

// ============================================================================
// McpLoopbackTool (对齐 OpenClaw gateway/mcp-http.schema.ts:4)
// ============================================================================

struct McpLoopbackTool {
    std::string name;
    std::string description;
    nlohmann::json parameters;  // JSON Schema
    std::function<nlohmann::json(const std::string& tool_call_id, const nlohmann::json& args)> execute;
};

// ============================================================================
// McpToolSchemaEntry (对齐 OpenClaw gateway/mcp-http.schema.ts:6-10)
// ============================================================================

struct McpToolSchemaEntry {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

// 🎯 对齐 OpenClaw: buildMcpToolSchema(tools)
std::vector<McpToolSchemaEntry> build_mcp_tool_schema(const std::vector<McpLoopbackTool>& tools);

// ============================================================================
// JSON-RPC Handler (对齐 OpenClaw gateway/mcp-http.handlers.ts:34-89)
// ============================================================================

// 🎯 对齐 OpenClaw: handleMcpJsonRpc({message, tools, toolSchema})
std::optional<nlohmann::json> handle_mcp_json_rpc(
    const JsonRpcRequest& message,
    const std::vector<McpLoopbackTool>& tools,
    const std::vector<McpToolSchemaEntry>& tool_schema);

// ============================================================================
// McpLoopbackRuntime (对齐 OpenClaw gateway/mcp-http.loopback-runtime.ts:1-4)
// ============================================================================

struct McpLoopbackRuntime {
    int port = 0;
    std::string token;
};

// 🎯 对齐 OpenClaw: getActiveMcpLoopbackRuntime()
std::optional<McpLoopbackRuntime> get_active_mcp_loopback_runtime();

// 🎯 对齐 OpenClaw: setActiveMcpLoopbackRuntime(runtime)
void set_active_mcp_loopback_runtime(const McpLoopbackRuntime& runtime);

// 🎯 对齐 OpenClaw: clearActiveMcpLoopbackRuntime(token)
void clear_active_mcp_loopback_runtime(const std::string& token);

// 🎯 对齐 OpenClaw: createMcpLoopbackServerConfig(port)
nlohmann::json create_mcp_loopback_server_config(int port);

// ============================================================================
// MCP Loopback Server (对齐 OpenClaw gateway/mcp-http.ts:34-157)
// ============================================================================

struct McpLoopbackServer {
    int port = 0;
    std::function<void()> close;
};

// 🎯 对齐 OpenClaw: startMcpLoopbackServer(port?)
core::Result<McpLoopbackServer> start_mcp_loopback_server(int port = 0);

// 🎯 对齐 OpenClaw: ensureMcpLoopbackServer(port?)
core::Result<McpLoopbackServer> ensure_mcp_loopback_server(int port = 0);

// 🎯 对齐 OpenClaw: closeMcpLoopbackServer()
void close_mcp_loopback_server();

} // namespace mcp
} // namespace hunterclaw
