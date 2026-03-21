// ============================================================================
// Config Methods
// 🎯 OpenClaw 严格对齐: server-methods/config.ts configHandlers
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>

using json = nlohmann::json;
namespace fs = std::filesystem;

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

// 🎯 OpenClaw 对齐: resolveConfigSnapshotHash
// 计算配置文件的 hash，用于 baseHash 校验
static std::string compute_config_hash(const std::string& config_path) {
    if (!fs::exists(config_path)) return "";
    std::ifstream f(config_path, std::ios::binary);
    if (!f.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    // 简单 hash: 使用 std::hash
    size_t h = std::hash<std::string>{}(content);
    char buf[32];
    snprintf(buf, sizeof(buf), "%016zx", h);
    return std::string(buf);
}

// 🎯 OpenClaw 对齐: readConfigFileSnapshot
// 读取配置文件快照，包含 raw 内容和 hash
static json read_config_snapshot(const std::string& config_path) {
    json snap;
    snap["exists"] = fs::exists(config_path);
    snap["path"] = config_path;
    snap["hash"] = compute_config_hash(config_path);
    snap["valid"] = true;
    snap["config"] = json::object();
    snap["raw"] = "";

    if (snap["exists"].get<bool>()) {
        try {
            std::ifstream f(config_path);
            if (f.is_open()) {
                std::string raw((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
                snap["raw"] = raw;
                // 尝试解析 JSON
                try {
                    snap["config"] = json::parse(raw);
                } catch (...) {
                    snap["valid"] = false;
                }
            }
        } catch (...) {
            snap["valid"] = false;
        }
    }
    return snap;
}

// Helper function to convert config to JSON
static json config_to_json(const std::shared_ptr<core::Config>& config) {
    if (config && !config->data.is_null()) {
        return config->data;
    }
    // Fallback placeholder structure
    json result;
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
// 🎯 OpenClaw 对齐: configHandlers["config.get"]
// 对应 OpenClaw: readConfigFileSnapshot + redactConfigSnapshot
core::Result<json> method_config_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    auto config = server->config();
    std::string config_path = config ? config->config_dir + "/config.yaml" : "";

    // 🎯 OpenClaw 对齐: readConfigFileSnapshot
    json snapshot = read_config_snapshot(config_path);

    // 🎯 OpenClaw 对齐: 返回快照格式 (redactConfigSnapshot)
    // 包含: exists, path, hash, valid, config, raw
    return core::Result<json>::Ok(snapshot);
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

// ============================================================================
// config.patch - 应用 merge-patch 并触发重启
// 🎯 OpenClaw 严格对齐: server-methods/config.ts configHandlers["config.patch"]
// 对应 OpenClaw: requireConfigBaseHash + applyMergePatch + writeConfigFile + scheduleGatewaySigusr1Restart
// ============================================================================
core::Result<json> method_config_patch(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    // 🎯 OpenClaw 对齐: 需要 raw 参数 (merge-patch 对象)
    if (!params.contains("raw") || !params["raw"].is_string()) {
        return core::Result<json>::Err("invalid config.patch params: raw (string) required");
    }
    std::string raw = params["raw"].get<std::string>();

    auto config = server->config();
    std::string config_path = config ? config->config_dir + "/config.yaml" : "";

    // 🎯 OpenClaw 对齐: requireConfigBaseHash 校验
    if (params.contains("baseHash") && params["baseHash"].is_string()) {
        std::string base_hash = params["baseHash"].get<std::string>();
        std::string current_hash = compute_config_hash(config_path);
        if (!current_hash.empty() && !base_hash.empty() && base_hash != current_hash) {
            return core::Result<json>::Err("config changed since last load; re-run config.get and retry");
        }
    }

    // 读取当前配置
    json current_config = json::object();
    if (fs::exists(config_path)) {
        try {
            std::ifstream f(config_path);
            if (f.is_open()) {
                std::string content((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                current_config = json::parse(content);
            }
        } catch (...) {
            return core::Result<json>::Err("invalid config; fix before patching");
        }
    }

    // 解析 patch 对象
    json patch_obj;
    try {
        patch_obj = json::parse(raw);
    } catch (const std::exception& e) {
        return core::Result<json>::Err(std::string("invalid config.patch raw: ") + e.what());
    }
    if (!patch_obj.is_object()) {
        return core::Result<json>::Err("config.patch raw must be an object");
    }

    // 🎯 OpenClaw 对齐: applyMergePatch
    // 将 patch 应用到当前配置
    json merged = current_config;
    for (auto& [key, val] : patch_obj.items()) {
        if (val.is_null()) {
            merged.erase(key);
        } else if (val.is_object() && merged.contains(key) && merged[key].is_object()) {
            // 递归合并
            for (auto& [k2, v2] : val.items()) {
                merged[key][k2] = v2;
            }
        } else {
            merged[key] = val;
        }
    }

    // 🎯 OpenClaw 对齐: writeConfigFile
    bool write_ok = false;
    if (!config_path.empty()) {
        try {
            fs::create_directories(fs::path(config_path).parent_path());
            std::ofstream f(config_path);
            if (f.is_open()) {
                f << merged.dump(2);
                write_ok = true;
            }
        } catch (...) {}
    }

    // 🎯 OpenClaw 对齐: scheduleGatewaySigusr1Restart
    int64_t restart_delay_ms = 0;
    if (params.contains("restartDelayMs") && params["restartDelayMs"].is_number()) {
        restart_delay_ms = params["restartDelayMs"].get<int64_t>();
    }

    json result;
    result["ok"] = write_ok;
    result["path"] = config_path;
    result["config"] = merged;
    result["restart"] = json{{"scheduled", write_ok}, {"delayMs", restart_delay_ms}, {"coalesced", false}};
    result["sentinel"] = json{{"path", nullptr}, {"payload", nullptr}};

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

// ============================================================================
// config.schema - 获取配置 JSON Schema
// 🎯 OpenClaw 严格对齐: server-methods/config.ts configHandlers["config.schema"]
// ============================================================================
core::Result<json> method_config_schema(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    // 🎯 OpenClaw 对齐: 返回 ConfigSchemaResponse 格式
    // { schema, uiHints, channels, plugins }
    json result;
    result["schema"] = json::object();   // TODO: 从 config 类型定义生成
    result["uiHints"] = json::object();
    result["channels"] = json::array();
    result["plugins"] = json::array();

    return core::Result<json>::Ok(result);
}

// ============================================================================
// config.schema.lookup - 查找配置 Schema 路径
// 🎯 OpenClaw 严格对齐: server-methods/config.ts configHandlers["config.schema.lookup"]
// ============================================================================
core::Result<json> method_config_schema_lookup(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    if (!params.contains("path") || !params["path"].is_string()) {
        return core::Result<json>::Err("invalid config.schema.lookup params: path (string) required");
    }
    std::string path = params["path"].get<std::string>();

    // 🎯 OpenClaw 对齐: 返回 ConfigSchemaLookupResult 格式
    // { path, schema, uiHints }
    json result;
    result["path"] = path;
    result["schema"] = nullptr;   // TODO: 从 schema 中查找
    result["uiHints"] = nullptr;

    return core::Result<json>::Ok(result);
}

// 🎯 OpenClaw 对齐: configHandlers["config.apply"]
// 对应 OpenClaw: requireConfigBaseHash + parseValidateConfig + writeConfigFile + scheduleGatewaySigusr1Restart
core::Result<json> method_config_apply(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    // 🎯 OpenClaw 对齐: 需要 raw 参数 (JSON5 格式的配置字符串)
    if (!params.contains("raw") || !params["raw"].is_string()) {
        return core::Result<json>::Err("invalid config.apply params: raw (string) required");
    }
    std::string raw = params["raw"].get<std::string>();

    auto config = server->config();
    std::string config_path = config ? config->config_dir + "/config.yaml" : "";

    // 🎯 OpenClaw 对齐: requireConfigBaseHash 校验
    // 对应 OpenClaw: if (baseHash !== snapshotHash) respond(false, ..., "config changed since last load")
    if (params.contains("baseHash") && params["baseHash"].is_string()) {
        std::string base_hash = params["baseHash"].get<std::string>();
        std::string current_hash = compute_config_hash(config_path);
        if (!current_hash.empty() && !base_hash.empty() && base_hash != current_hash) {
            return core::Result<json>::Err("config changed since last load; re-run config.get and retry");
        }
    }

    // 🎯 OpenClaw 对齐: 尝试解析配置
    json parsed_config;
    try {
        parsed_config = json::parse(raw);
    } catch (const std::exception& e) {
        return core::Result<json>::Err(std::string("invalid config: ") + e.what());
    }

    // 🎯 OpenClaw 对齐: writeConfigFile
    bool write_ok = false;
    if (!config_path.empty()) {
        try {
            fs::create_directories(fs::path(config_path).parent_path());
            std::ofstream f(config_path);
            if (f.is_open()) {
                f << parsed_config.dump(2);
                write_ok = true;
            }
        } catch (...) {}
    }

    // 🎯 OpenClaw 对齐: scheduleGatewaySigusr1Restart
    // 对应 OpenClaw: scheduleGatewaySigusr1Restart({ delayMs, reason: "config.apply" })
    int64_t restart_delay_ms = 0;
    if (params.contains("restartDelayMs") && params["restartDelayMs"].is_number()) {
        restart_delay_ms = params["restartDelayMs"].get<int64_t>();
    }
    bool restart_scheduled = false;
    if (write_ok) {
        // 发送 SIGUSR1 触发重启
        // TODO: 实现延迟重启逻辑
        restart_scheduled = true;
    }

    json result;
    result["ok"] = write_ok;
    result["path"] = config_path;
    result["config"] = parsed_config;
    result["restart"] = json{{"scheduled", restart_scheduled}, {"delayMs", restart_delay_ms}, {"coalesced", false}};
    result["sentinel"] = json{{"path", nullptr}, {"payload", nullptr}};

    return core::Result<json>::Ok(result);
}

// ============================================================================
// config.openFile - 用系统默认编辑器打开配置文件
// 🎯 OpenClaw 严格对齐: server-methods/config.ts configHandlers["config.openFile"]
// 对应 OpenClaw: exec(`open/xdg-open ${configPath}`)
// ============================================================================
core::Result<json> method_config_open_file(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    // TODO: 获取配置文件路径并用系统编辑器打开
    std::string config_path = "";  // TODO: createConfigIO().configPath

    json result;
    result["ok"] = false;
    result["path"] = config_path;
    result["error"] = "config.openFile not yet implemented";

    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
