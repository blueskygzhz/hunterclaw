// 🎯 对齐 OpenClaw: config/mcp-config.ts + config/types.mcp.ts
// MCP 服务器配置 CRUD 实现

#include "hunterclaw/mcp/mcp_config.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace mcp {

// ============================================================================
// McpServerConfig JSON serialization
// ============================================================================

nlohmann::json McpServerConfig::to_json() const {
    nlohmann::json j;
    if (command) j["command"] = *command;
    if (!args.empty()) j["args"] = args;
    if (!env.empty()) j["env"] = env;
    if (cwd) j["cwd"] = *cwd;
    if (working_directory) j["workingDirectory"] = *working_directory;
    if (url) j["url"] = *url;
    if (transport) j["transport"] = *transport;
    if (!headers.empty()) j["headers"] = headers;
    if (connection_timeout_ms) j["connectionTimeoutMs"] = *connection_timeout_ms;
    // 🎯 对齐 OpenClaw toolFilter (types.mcp.ts:11-20)
    if (tool_filter) {
        nlohmann::json tf = nlohmann::json::object();
        if (tool_filter->has_include) tf["include"] = tool_filter->include;
        if (tool_filter->has_exclude) tf["exclude"] = tool_filter->exclude;
        if (!tf.empty()) j["toolFilter"] = tf;
    }
    return j;
}

McpServerConfig McpServerConfig::from_json(const nlohmann::json& j) {
    McpServerConfig cfg;
    if (j.contains("command") && j["command"].is_string()) cfg.command = j["command"];
    if (j.contains("args") && j["args"].is_array()) {
        for (const auto& a : j["args"]) {
            if (a.is_string()) cfg.args.push_back(a.get<std::string>());
        }
    }
    if (j.contains("env") && j["env"].is_object()) {
        for (auto& [k, v] : j["env"].items()) {
            if (v.is_string()) cfg.env[k] = v.get<std::string>();
            else if (v.is_number()) cfg.env[k] = std::to_string(v.get<double>());
            else if (v.is_boolean()) cfg.env[k] = v.get<bool>() ? "true" : "false";
        }
    }
    if (j.contains("cwd") && j["cwd"].is_string()) cfg.cwd = j["cwd"];
    if (j.contains("workingDirectory") && j["workingDirectory"].is_string()) cfg.working_directory = j["workingDirectory"];
    if (j.contains("url") && j["url"].is_string()) cfg.url = j["url"];
    if (j.contains("transport") && j["transport"].is_string()) cfg.transport = j["transport"];
    if (j.contains("headers") && j["headers"].is_object()) {
        for (auto& [k, v] : j["headers"].items()) {
            if (v.is_string()) cfg.headers[k] = v.get<std::string>();
        }
    }
    if (j.contains("connectionTimeoutMs") && j["connectionTimeoutMs"].is_number()) {
        cfg.connection_timeout_ms = j["connectionTimeoutMs"].get<int>();
    }
    // 🎯 对齐 OpenClaw toolFilter (types.mcp.ts:11-20)
    if (j.contains("toolFilter") && j["toolFilter"].is_object()) {
        const auto& tf = j["toolFilter"];
        McpServerToolFilterConfig filter;
        if (tf.contains("include") && tf["include"].is_array()) {
            filter.has_include = true;
            for (const auto& p : tf["include"]) {
                if (p.is_string()) filter.include.push_back(p.get<std::string>());
            }
        }
        if (tf.contains("exclude") && tf["exclude"].is_array()) {
            filter.has_exclude = true;
            for (const auto& p : tf["exclude"]) {
                if (p.is_string()) filter.exclude.push_back(p.get<std::string>());
            }
        }
        cfg.tool_filter = std::move(filter);
    }
    return cfg;
}

// ============================================================================
// normalizeConfiguredMcpServers (对齐 OpenClaw config/mcp-config.ts:29-38)
// ============================================================================

ConfigMcpServers normalize_configured_mcp_servers(const nlohmann::json& value) {
    ConfigMcpServers result;
    if (!value.is_object()) return result;
    for (auto& [name, server] : value.items()) {
        if (server.is_object()) {
            result[name] = server;
        }
    }
    return result;
}

// ============================================================================
// Helper: read/write config file
// ============================================================================

static std::string resolve_config_path(const std::string& config_path) {
    if (!config_path.empty()) return config_path;
    // 🎯 对齐 OpenClaw: 默认使用 ~/.hunterclaw/config.yaml 或 config.json
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    if (home) return std::string(home) + "/.hunterclaw/config.json";
    return "config.json";
}

static nlohmann::json read_config_json(const std::string& path) {
    try {
        if (!fs::exists(path)) return nlohmann::json::object();
        std::ifstream f(path);
        return nlohmann::json::parse(f);
    } catch (...) {
        return nlohmann::json::object();
    }
}

static bool write_config_json(const std::string& path, const nlohmann::json& config) {
    try {
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path);
        f << config.dump(2) << "\n";
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// listConfiguredMcpServers (对齐 OpenClaw config/mcp-config.ts:40-57)
// ============================================================================

McpConfigReadResult list_configured_mcp_servers(const std::string& config_path) {
    std::string path = resolve_config_path(config_path);
    auto config = read_config_json(path);

    McpConfigReadResult result;
    result.path = path;

    if (config.is_null()) {
        result.ok = false;
        result.error = "Config file is invalid; fix it before using MCP config commands.";
        return result;
    }

    result.ok = true;
    // 🎯 对齐 OpenClaw: normalizeConfiguredMcpServers(sourceConfig.mcp?.servers)
    if (config.contains("mcp") && config["mcp"].is_object() &&
        config["mcp"].contains("servers")) {
        result.mcp_servers = normalize_configured_mcp_servers(config["mcp"]["servers"]);
    }
    return result;
}

// ============================================================================
// setConfiguredMcpServer (对齐 OpenClaw config/mcp-config.ts:59-103)
// ============================================================================

McpConfigWriteResult set_configured_mcp_server(
    const std::string& name,
    const nlohmann::json& server,
    const std::string& config_path) {

    std::string trimmed = name;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();

    McpConfigWriteResult result;
    if (trimmed.empty()) {
        result.ok = false;
        result.error = "MCP server name is required.";
        return result;
    }
    if (!server.is_object()) {
        result.ok = false;
        result.error = "MCP server config must be a JSON object.";
        return result;
    }

    std::string path = resolve_config_path(config_path);
    auto config = read_config_json(path);

    // 🎯 对齐 OpenClaw: const servers = normalizeConfiguredMcpServers(next.mcp?.servers)
    ConfigMcpServers servers;
    if (config.contains("mcp") && config["mcp"].is_object() && config["mcp"].contains("servers")) {
        servers = normalize_configured_mcp_servers(config["mcp"]["servers"]);
    }
    servers[trimmed] = server;

    // Update config
    if (!config.contains("mcp")) config["mcp"] = nlohmann::json::object();
    config["mcp"]["servers"] = nlohmann::json(servers);

    if (!write_config_json(path, config)) {
        result.ok = false;
        result.path = path;
        result.error = "Failed to write config file.";
        return result;
    }

    result.ok = true;
    result.path = path;
    result.mcp_servers = servers;
    return result;
}

// ============================================================================
// unsetConfiguredMcpServer (对齐 OpenClaw config/mcp-config.ts:105-162)
// ============================================================================

McpConfigWriteResult unset_configured_mcp_server(
    const std::string& name,
    const std::string& config_path) {

    std::string trimmed = name;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();

    McpConfigWriteResult result;
    if (trimmed.empty()) {
        result.ok = false;
        result.error = "MCP server name is required.";
        return result;
    }

    std::string path = resolve_config_path(config_path);
    auto config = read_config_json(path);

    ConfigMcpServers servers;
    if (config.contains("mcp") && config["mcp"].is_object() && config["mcp"].contains("servers")) {
        servers = normalize_configured_mcp_servers(config["mcp"]["servers"]);
    }

    if (servers.find(trimmed) == servers.end()) {
        result.ok = true;
        result.path = path;
        result.mcp_servers = servers;
        result.removed = false;
        return result;
    }

    // 🎯 对齐 OpenClaw: delete servers[name]
    servers.erase(trimmed);

    if (servers.empty()) {
        // 🎯 对齐 OpenClaw: if (Object.keys(servers).length === 0) delete next.mcp.servers
        if (config.contains("mcp") && config["mcp"].is_object()) {
            config["mcp"].erase("servers");
            if (config["mcp"].empty()) config.erase("mcp");
        }
    } else {
        config["mcp"]["servers"] = nlohmann::json(servers);
    }

    if (!write_config_json(path, config)) {
        result.ok = false;
        result.path = path;
        result.error = "Failed to write config file.";
        return result;
    }

    result.ok = true;
    result.path = path;
    result.mcp_servers = servers;
    result.removed = true;
    return result;
}

} // namespace mcp
} // namespace hunterclaw
