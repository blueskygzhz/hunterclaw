#pragma once

// 🎯 对齐 OpenClaw: config/types.mcp.ts (McpServerConfig, McpConfig)
// 🎯 对齐 OpenClaw: config/mcp-config.ts (list/set/unset MCP servers)

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// McpServerConfig (对齐 OpenClaw config/types.mcp.ts:1-21)
// ============================================================================

struct McpServerConfig {
    std::optional<std::string> command;                   // 🎯 Stdio transport: command to spawn
    std::vector<std::string> args;                        // 🎯 Stdio transport: arguments
    std::map<std::string, std::string> env;               // 🎯 Environment variables
    std::optional<std::string> cwd;                       // 🎯 Working directory
    std::optional<std::string> working_directory;         // 🎯 Alias for cwd
    std::optional<std::string> url;                       // 🎯 HTTP transport: URL
    std::optional<std::string> transport;                 // 🎯 "sse" | "streamable-http"
    std::map<std::string, std::string> headers;           // 🎯 HTTP transport: extra headers
    std::optional<int> connection_timeout_ms;             // 🎯 Optional connection timeout

    // Serialize to/from JSON
    nlohmann::json to_json() const;
    static McpServerConfig from_json(const nlohmann::json& j);
};

// ============================================================================
// McpConfig (对齐 OpenClaw config/types.mcp.ts:23-26)
// ============================================================================

struct McpConfig {
    std::map<std::string, McpServerConfig> servers;       // 🎯 Named MCP server definitions
};

// ============================================================================
// MCP Config CRUD types (对齐 OpenClaw config/mcp-config.ts)
// ============================================================================

using ConfigMcpServers = std::map<std::string, nlohmann::json>;

struct McpConfigReadResult {
    bool ok = false;
    std::string path;
    ConfigMcpServers mcp_servers;
    std::string error;      // set when !ok
    std::string base_hash;  // for optimistic locking
};

struct McpConfigWriteResult {
    bool ok = false;
    std::string path;
    ConfigMcpServers mcp_servers;
    std::string error;
    bool removed = false;
};

// ============================================================================
// MCP Config CRUD functions (对齐 OpenClaw config/mcp-config.ts:40-162)
// ============================================================================

// 🎯 对齐 OpenClaw: normalizeConfiguredMcpServers(value)
ConfigMcpServers normalize_configured_mcp_servers(const nlohmann::json& value);

// 🎯 对齐 OpenClaw: listConfiguredMcpServers()
McpConfigReadResult list_configured_mcp_servers(const std::string& config_path = "");

// 🎯 对齐 OpenClaw: setConfiguredMcpServer({name, server})
McpConfigWriteResult set_configured_mcp_server(
    const std::string& name,
    const nlohmann::json& server,
    const std::string& config_path = "");

// 🎯 对齐 OpenClaw: unsetConfiguredMcpServer({name})
McpConfigWriteResult unset_configured_mcp_server(
    const std::string& name,
    const std::string& config_path = "");

} // namespace mcp
} // namespace hunterclaw
