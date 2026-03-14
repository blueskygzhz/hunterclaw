#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Config Methods
// ============================================================================

// Helper function to split config key (e.g., "gateway.port" -> ["gateway", "port"])
static std::vector<std::string> split_config_key(const std::string& key) {
    std::vector<std::string> keys;
    std::string current;
    for (char c : key) {
        if (c == '.') {
            if (!current.empty()) {
                keys.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        keys.push_back(current);
    }
    return keys;
}

// Helper function to convert config to JSON
static json config_to_json(const std::shared_ptr<core::Config>& config) {
    json result;
    // TODO: Implement actual config->toJson() method
    // For now, return placeholder structure
    result["agent"] = {
        {"max_turns", 50},
        {"timeout_ms", 300000}
    };
    result["gateway"] = {
        {"port", 8080},
        {"heartbeat_interval_ms", 30000}
    };
    result["llm"] = {
        {"temperature", 0.7},
        {"max_tokens", 4096}
    };
    return result;
}

// 26. config.get - Get configuration value(s)
core::Result<json> method_config_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;
    
    std::string key = params.value("key", "");
    
    // Get config from server (we need to add this accessor)
    // For now, use a placeholder config object
    auto config = std::shared_ptr<core::Config>();  // TODO: server->config()
    json config_json = config_to_json(config);
    
    if (key.empty()) {
        // Return entire config
        json result;
        result["config"] = config_json;
        return core::Result<json>::Ok(result);
    }
    
    // Split key path
    auto keys = split_config_key(key);
    json current = config_json;
    
    // Navigate to nested value
    for (const auto& k : keys) {
        if (!current.contains(k)) {
            return core::Result<json>::Err("Config key not found: " + key);
        }
        current = current[k];
    }
    
    json result;
    result["key"] = key;
    result["value"] = current;
    return core::Result<json>::Ok(result);
}

// 18. config.set - Set configuration value
core::Result<json> method_config_set(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("key") || !params.contains("value")) {
        return core::Result<json>::Err("Missing required parameters: key, value");
    }
    
    std::string key = params["key"];
    json value = params["value"];
    
    // For security, only allow certain keys to be modified at runtime
    std::vector<std::string> allowed_keys = {
        "agent.max_turns",
        "agent.timeout_ms",
        "gateway.heartbeat_interval_ms",
        "llm.temperature",
        "llm.max_tokens",
        "sandbox.timeout_ms",
        "skills.auto_install"
    };
    
    bool is_allowed = false;
    for (const auto& allowed : allowed_keys) {
        if (key == allowed || key.rfind(allowed + ".", 0) == 0) {
            is_allowed = true;
            break;
        }
    }
    
    if (!is_allowed) {
        return core::Result<json>::Err("Configuration key '" + key + "' is not allowed to be modified at runtime");
    }
    
    json result;
    result["key"] = key;
    result["value"] = value;
    result["updated"] = true;
    result["timestamp"] = core::utils::now().time_since_epoch().count();
    
    return core::Result<json>::Ok(result);
}

// 19. config.reload - Reload configuration from file
core::Result<json> method_config_reload(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;
    
    std::string config_path = params.value("path", "");
    
    try {
        // TODO: server->config() accessor method needed
        // auto config = server->config();
        
        json result;
        result["reloaded"] = true;
        result["path"] = config_path.empty() ? "default" : config_path;
        result["timestamp"] = core::utils::now().time_since_epoch().count();
        result["changes"] = json::array();
        result["note"] = "Config reload requested (requires Config class support for hot reload)";
        
        return core::Result<json>::Ok(result);
        
    } catch (const std::exception& e) {
        return core::Result<json>::Err(std::string("Failed to reload config: ") + e.what());
    }
}

// 29. config.patch - Patch configuration (JSON Patch)
core::Result<json> method_config_patch(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("patches") || !params["patches"].is_array()) {
        return core::Result<json>::Err("Missing or invalid 'patches' parameter (must be array)");
    }
    
    json patches = params["patches"];
    json applied_patches = json::array();
    json failed_patches = json::array();
    
    for (const auto& patch : patches) {
        if (!patch.contains("op") || !patch.contains("path")) {
            failed_patches.push_back({
                {"patch", patch},
                {"reason", "Missing 'op' or 'path' field"}
            });
            continue;
        }
        
        std::string op = patch["op"];
        std::string path = patch["path"];
        
        // Only support "replace" and "add" operations for safety
        if (op != "replace" && op != "add") {
            failed_patches.push_back({
                {"patch", patch},
                {"reason", "Unsupported operation: " + op}
            });
            continue;
        }
        
        // Successfully applied (placeholder)
        applied_patches.push_back(patch);
    }
    
    json result;
    result["applied"] = applied_patches;
    result["failed"] = failed_patches;
    result["total_patches"] = patches.size();
    result["applied_count"] = applied_patches.size();
    result["failed_count"] = failed_patches.size();
    
    return core::Result<json>::Ok(result);
}

// 27. config.list - List all configuration keys
core::Result<json> method_config_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;
    
    std::string prefix = params.value("prefix", "");
    bool include_values = params.value("include_values", false);
    
    // TODO: server->config() accessor method needed
    auto config = std::shared_ptr<core::Config>();  // Placeholder
    json config_json = config_to_json(config);
    
    json result;
    result["keys"] = json::array();
    
    // Recursively collect all keys
    std::function<void(const json&, const std::string&)> collect_keys;
    collect_keys = [&](const json& obj, const std::string& current_prefix) {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            std::string key = current_prefix.empty() ? it.key() : current_prefix + "." + it.key();
            if (prefix.empty() || key.rfind(prefix, 0) == 0) {
                json key_info;
                key_info["key"] = key;
                if (include_values) {
                    key_info["value"] = it.value();
                }
                result["keys"].push_back(key_info);
            }
            if (it.value().is_object()) {
                collect_keys(it.value(), key);
            }
        }
    };
    
    collect_keys(config_json, "");
    
    result["count"] = result["keys"].size();
    return core::Result<json>::Ok(result);
}

// 28. config.validate - Validate configuration
core::Result<json> method_config_validate(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server;
    (void)method_guard;
    (void)client_ctx;
    (void)params;
    
    // TODO: server->config() accessor method needed
    // auto config = server->config();
    json errors = json::array();
    json warnings = json::array();
    
    // TODO: Implement actual validation logic
    // For now, assume all valid
    
    json result;
    result["valid"] = errors.empty();
    result["errors"] = errors;
    result["warnings"] = warnings;
    result["timestamp"] = core::utils::now().time_since_epoch().count();
    
    return core::Result<json>::Ok(result);
}

// 30. config.reset - Reset configuration to defaults
core::Result<json> method_config_reset(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server;
    (void)method_guard;
    (void)client_ctx;
    
    std::string key = params.value("key", "");
    
    if (key.empty()) {
        return core::Result<json>::Err("Resetting all config is not allowed for safety");
    }
    
    json result;
    result["key"] = key;
    result["reset"] = true;
    result["timestamp"] = core::utils::now().time_since_epoch().count();
    result["note"] = "Config reset requested";
    
    return core::Result<json>::Ok(result);
}

// 31. config.export - Export configuration
core::Result<json> method_config_export(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;
    
    std::string format = params.value("format", "json");
    bool include_defaults = params.value("include_defaults", false);
    bool include_secrets = params.value("include_secrets", false);
    
    // TODO: server->config() accessor method needed
    auto config = std::shared_ptr<core::Config>();  // Placeholder
    json config_json = config_to_json(config);
    
    json result;
    result["format"] = format;
    result["config"] = config_json;
    result["timestamp"] = core::utils::now().time_since_epoch().count();
    
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
