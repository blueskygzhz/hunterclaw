// 🎯 对齐 OpenClaw: gateway/mcp-http.ts + mcp-http.handlers.ts + mcp-http.protocol.ts
//                    + mcp-http.loopback-runtime.ts + mcp-http.schema.ts
// MCP HTTP Loopback Server — JSON-RPC over HTTP

#include "hunterclaw/mcp/mcp_http_loopback.h"
#include <algorithm>
#include <mutex>
#include <chrono>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// JSON-RPC helpers (对齐 OpenClaw gateway/mcp-http.protocol.ts:14-20)
// ============================================================================

nlohmann::json json_rpc_result(const std::optional<std::string>& id, const nlohmann::json& result) {
    nlohmann::json j;
    j["jsonrpc"] = "2.0";
    j["id"] = id.value_or("");
    j["result"] = result;
    return j;
}

nlohmann::json json_rpc_error(const std::optional<std::string>& id, int code, const std::string& message) {
    nlohmann::json j;
    j["jsonrpc"] = "2.0";
    j["id"] = id.value_or("");
    j["error"] = {{"code", code}, {"message", message}};
    return j;
}

// ============================================================================
// buildMcpToolSchema (对齐 OpenClaw gateway/mcp-http.schema.ts:62-83)
// ============================================================================

static nlohmann::json flatten_union_schema(nlohmann::json raw) {
    // 🎯 对齐 OpenClaw: flattenUnionSchema(raw)
    auto variants = raw.contains("anyOf") ? raw["anyOf"] :
                   raw.contains("oneOf") ? raw["oneOf"] : nlohmann::json();
    if (!variants.is_array() || variants.empty()) return raw;

    nlohmann::json merged_props = nlohmann::json::object();
    for (const auto& variant : variants) {
        if (variant.contains("properties") && variant["properties"].is_object()) {
            for (auto& [key, schema] : variant["properties"].items()) {
                if (!merged_props.contains(key)) {
                    merged_props[key] = schema;
                }
            }
        }
    }

    nlohmann::json result = raw;
    result.erase("anyOf");
    result.erase("oneOf");
    result["type"] = "object";
    result["properties"] = merged_props;
    return result;
}

std::vector<McpToolSchemaEntry> build_mcp_tool_schema(const std::vector<McpLoopbackTool>& tools) {
    std::vector<McpToolSchemaEntry> result;
    for (const auto& tool : tools) {
        nlohmann::json raw = tool.parameters.is_object() ? nlohmann::json(tool.parameters) : nlohmann::json::object();
        if (raw.contains("anyOf") || raw.contains("oneOf")) {
            raw = flatten_union_schema(raw);
        }
        if (!raw.contains("type") || raw["type"] != "object") {
            raw["type"] = "object";
            if (!raw.contains("properties")) raw["properties"] = nlohmann::json::object();
        }
        result.push_back({tool.name, tool.description, raw});
    }
    return result;
}

// ============================================================================
// handleMcpJsonRpc (对齐 OpenClaw gateway/mcp-http.handlers.ts:34-89)
// ============================================================================

std::optional<nlohmann::json> handle_mcp_json_rpc(
    const JsonRpcRequest& message,
    const std::vector<McpLoopbackTool>& tools,
    const std::vector<McpToolSchemaEntry>& tool_schema) {

    const auto& method = message.method;

    // 🎯 对齐 OpenClaw: switch(method)
    if (method == "initialize") {
        std::string client_version = message.params.value("protocolVersion", "");
        std::string negotiated = MCP_LOOPBACK_SUPPORTED_PROTOCOL_VERSIONS[0];
        for (const auto& v : MCP_LOOPBACK_SUPPORTED_PROTOCOL_VERSIONS) {
            if (v == client_version) { negotiated = v; break; }
        }
        return json_rpc_result(message.id, {
            {"protocolVersion", negotiated},
            {"capabilities", {{"tools", nlohmann::json::object()}}},
            {"serverInfo", {{"name", MCP_LOOPBACK_SERVER_NAME}, {"version", MCP_LOOPBACK_SERVER_VERSION}}}
        });
    }

    if (method == "notifications/initialized" || method == "notifications/cancelled") {
        return std::nullopt;  // No response
    }

    if (method == "tools/list") {
        nlohmann::json tools_json = nlohmann::json::array();
        for (const auto& t : tool_schema) {
            tools_json.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}});
        }
        return json_rpc_result(message.id, {{"tools", tools_json}});
    }

    if (method == "tools/call") {
        std::string tool_name = message.params.value("name", "");
        nlohmann::json tool_args = message.params.value("arguments", nlohmann::json::object());

        auto it = std::find_if(tools.begin(), tools.end(),
            [&](const McpLoopbackTool& t) { return t.name == tool_name; });

        if (it == tools.end()) {
            return json_rpc_result(message.id, {
                {"content", {{{"type", "text"}, {"text", "Tool not available: " + tool_name}}}},
                {"isError", true}
            });
        }

        try {
            std::string tool_call_id = "mcp-" + std::to_string(
                std::chrono::system_clock::now().time_since_epoch().count());
            auto result = it->execute(tool_call_id, tool_args);
            return json_rpc_result(message.id, {
                {"content", result.is_array() ? result : nlohmann::json::array({{{"type", "text"}, {"text", result.dump()}}})},
                {"isError", false}
            });
        } catch (const std::exception& e) {
            return json_rpc_result(message.id, {
                {"content", {{{"type", "text"}, {"text", std::string("Tool error: ") + e.what()}}}},
                {"isError", true}
            });
        }
    }

    return json_rpc_error(message.id, -32601, "Method not found: " + method);
}

// ============================================================================
// Global Loopback Runtime State (对齐 OpenClaw gateway/mcp-http.loopback-runtime.ts)
// ============================================================================

static std::mutex g_loopback_mutex;
static std::optional<McpLoopbackRuntime> g_active_runtime;

std::optional<McpLoopbackRuntime> get_active_mcp_loopback_runtime() {
    std::lock_guard<std::mutex> lock(g_loopback_mutex);
    return g_active_runtime;
}

void set_active_mcp_loopback_runtime(const McpLoopbackRuntime& runtime) {
    std::lock_guard<std::mutex> lock(g_loopback_mutex);
    g_active_runtime = runtime;
}

void clear_active_mcp_loopback_runtime(const std::string& token) {
    std::lock_guard<std::mutex> lock(g_loopback_mutex);
    if (g_active_runtime && g_active_runtime->token == token) {
        g_active_runtime = std::nullopt;
    }
}

// 🎯 对齐 OpenClaw: createMcpLoopbackServerConfig(port)
nlohmann::json create_mcp_loopback_server_config(int port) {
    return {
        {"mcpServers", {
            {"openclaw", {
                {"type", "http"},
                {"url", "http://127.0.0.1:" + std::to_string(port) + "/mcp"},
                {"headers", {
                    {"Authorization", "Bearer ${OPENCLAW_MCP_TOKEN}"},
                    {"x-session-key", "${OPENCLAW_MCP_SESSION_KEY}"},
                    {"x-openclaw-agent-id", "${OPENCLAW_MCP_AGENT_ID}"},
                    {"x-openclaw-account-id", "${OPENCLAW_MCP_ACCOUNT_ID}"},
                    {"x-openclaw-message-channel", "${OPENCLAW_MCP_MESSAGE_CHANNEL}"},
                    {"x-openclaw-sender-is-owner", "${OPENCLAW_MCP_SENDER_IS_OWNER}"},
                }}
            }}
        }}
    };
}

// ============================================================================
// MCP Loopback Server (对齐 OpenClaw gateway/mcp-http.ts:34-157)
// NOTE: 完整的 HTTP 服务器实现需要依赖 httplib 或 boost.asio
// 此处提供结构性实现，实际 HTTP 监听在 gateway 层集成
// ============================================================================

static std::optional<McpLoopbackServer> g_active_server;

core::Result<McpLoopbackServer> start_mcp_loopback_server(int port) {
    // 🎯 对齐 OpenClaw: 在 127.0.0.1 上启动 HTTP 服务器
    // 完整实现使用 third_party/httplib.h
    McpLoopbackServer server;
    server.port = (port == 0) ? 19876 : port;  // 默认端口
    server.close = []() {
        // 清理回调
    };

    // 注册全局运行时
    McpLoopbackRuntime runtime;
    runtime.port = server.port;
    runtime.token = "loopback-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    set_active_mcp_loopback_runtime(runtime);

    g_active_server = server;
    return core::Result<McpLoopbackServer>::Ok(server);
}

core::Result<McpLoopbackServer> ensure_mcp_loopback_server(int port) {
    if (g_active_server) {
        return core::Result<McpLoopbackServer>::Ok(*g_active_server);
    }
    return start_mcp_loopback_server(port);
}

void close_mcp_loopback_server() {
    if (g_active_server) {
        if (g_active_server->close) g_active_server->close();
        g_active_server = std::nullopt;
    }
}

} // namespace mcp
} // namespace hunterclaw
