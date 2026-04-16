#pragma once

// 🎯 对齐 OpenClaw: agents/pi-bundle-mcp-types.ts (SessionMcpRuntime, McpToolCatalog, etc.)
// 🎯 对齐 OpenClaw: agents/pi-bundle-mcp-names.ts (sanitizeServerName, buildSafeToolName)

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// McpServerCatalog (对齐 OpenClaw line 10-14)
// ============================================================================

struct McpServerCatalog {
    std::string server_name;
    std::string launch_summary;
    int tool_count = 0;
};

// ============================================================================
// McpCatalogTool (对齐 OpenClaw line 16-24)
// ============================================================================

struct McpCatalogTool {
    std::string server_name;
    std::string safe_server_name;
    std::string tool_name;
    std::string title;
    std::string description;
    nlohmann::json input_schema;
    std::string fallback_description;
};

// ============================================================================
// McpToolCatalog (对齐 OpenClaw line 26-31)
// ============================================================================

struct McpToolCatalog {
    int version = 1;
    int64_t generated_at = 0;
    std::map<std::string, McpServerCatalog> servers;
    std::vector<McpCatalogTool> tools;
};

// ============================================================================
// SessionMcpRuntime (对齐 OpenClaw line 33-44)
// ============================================================================

struct CallToolResult {
    nlohmann::json content;        // Array of {type, text}
    nlohmann::json structured_content;
    bool is_error = false;
};

class SessionMcpRuntime {
public:
    virtual ~SessionMcpRuntime() = default;

    std::string session_id;
    std::string session_key;
    std::string workspace_dir;
    std::string config_fingerprint;
    int64_t created_at = 0;
    int64_t last_used_at = 0;

    // 🎯 对齐 OpenClaw: getCatalog()
    virtual McpToolCatalog get_catalog() = 0;

    // 🎯 对齐 OpenClaw: markUsed()
    virtual void mark_used() = 0;

    // 🎯 对齐 OpenClaw: callTool(serverName, toolName, input)
    virtual CallToolResult call_tool(
        const std::string& server_name,
        const std::string& tool_name,
        const nlohmann::json& input) = 0;

    // 🎯 对齐 OpenClaw: dispose()
    virtual void dispose() = 0;
};

// ============================================================================
// SessionMcpRuntimeManager (对齐 OpenClaw line 46-58)
// ============================================================================

class SessionMcpRuntimeManager {
public:
    virtual ~SessionMcpRuntimeManager() = default;

    // 🎯 对齐 OpenClaw: getOrCreate(params)
    virtual std::shared_ptr<SessionMcpRuntime> get_or_create(
        const std::string& session_id,
        const std::string& session_key,
        const std::string& workspace_dir) = 0;

    // 🎯 对齐 OpenClaw: bindSessionKey(sessionKey, sessionId)
    virtual void bind_session_key(const std::string& session_key, const std::string& session_id) = 0;

    // 🎯 对齐 OpenClaw: resolveSessionId(sessionKey)
    virtual std::string resolve_session_id(const std::string& session_key) = 0;

    // 🎯 对齐 OpenClaw: disposeSession(sessionId)
    virtual void dispose_session(const std::string& session_id) = 0;

    // 🎯 对齐 OpenClaw: disposeAll()
    virtual void dispose_all() = 0;

    // 🎯 对齐 OpenClaw: listSessionIds()
    virtual std::vector<std::string> list_session_ids() = 0;
};

// ============================================================================
// BundleMcpToolRuntime (对齐 OpenClaw line 5-8)
// ============================================================================

struct BundleMcpAgentTool {
    std::string name;
    std::string label;
    std::string description;
    nlohmann::json parameters;
    std::function<CallToolResult(const std::string&, const nlohmann::json&)> execute;
};

struct BundleMcpToolRuntime {
    std::vector<BundleMcpAgentTool> tools;
    std::function<void()> dispose;
};

// ============================================================================
// Tool Name Utilities (对齐 OpenClaw pi-bundle-mcp-names.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: TOOL_NAME_SEPARATOR = "__"
inline const std::string TOOL_NAME_SEPARATOR = "__";

// 🎯 对齐 OpenClaw: sanitizeServerName(raw, usedNames)
std::string sanitize_server_name(const std::string& raw, std::set<std::string>& used_names);

// 🎯 对齐 OpenClaw: sanitizeToolName(raw)
std::string sanitize_tool_name(const std::string& raw);

// 🎯 对齐 OpenClaw: normalizeReservedToolNames(names?)
std::set<std::string> normalize_reserved_tool_names(const std::vector<std::string>& names = {});

// 🎯 对齐 OpenClaw: buildSafeToolName({serverName, toolName, reservedNames})
std::string build_safe_tool_name(
    const std::string& server_name,
    const std::string& tool_name,
    std::set<std::string>& reserved_names);

} // namespace mcp
} // namespace hunterclaw
