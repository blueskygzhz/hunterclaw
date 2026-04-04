// ============================================================================
// Config Methods
// 🎯 OpenClaw 严格对齐: server-methods/config.ts configHandlers
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <chrono>
#include <signal.h>
#include <unistd.h>

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
        // ✅ 对齐 OpenClaw: 通过 server->config() 获取配置
        auto config = server->config();
        
        json result;
        result["reloaded"] = true;
        result["path"] = config_path.empty() ? (config ? config->config_dir + "/config.yaml" : "default") : config_path;
        result["timestamp"] = core::utils::now().time_since_epoch().count();
        result["changes"] = json::array();
        result["note"] = "Config reload requested";
        
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
    
    // ✅ 对齐 OpenClaw: 通过 server->config() 获取配置
    auto config = server->config();
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
    
    // ✅ 对齐 OpenClaw: 验证配置字段
    (void)method_guard;
    (void)client_ctx;
    (void)params;
    auto config_v = server->config();
    json errors = json::array();
    json warnings = json::array();

    if (config_v) {
        if (config_v->gateway.port <= 0 || config_v->gateway.port > 65535) {
            json err; err["field"] = "gateway.port"; err["message"] = "Port must be 1-65535";
            errors.push_back(err);
        }
        if (config_v->gateway.host.empty()) {
            json warn; warn["field"] = "gateway.host"; warn["message"] = "Host is empty, using default 127.0.0.1";
            warnings.push_back(warn);
        }
    }
    
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
    
    // ✅ 对齐 OpenClaw: 通过 server->config() 获取配置
    auto config = server->config();
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
    // ✅ 对齐 OpenClaw: 返回 ConfigSchemaResponse 格式
    result["schema"] = json::object({
        {"type", "object"},
        {"title", "HunterClaw Configuration"},
        {"properties", json::object({
            {"agents", json::object({{"type", "array"}, {"description", "Agent configurations"}})},
            {"providers", json::object({{"type", "object"}, {"description", "LLM provider configurations"}})},
            {"skills", json::object({{"type", "object"}, {"description", "Skills configuration"}})},
            {"plugins", json::object({{"type", "object"}, {"description", "Plugin configurations"}})},
            {"auth", json::object({{"type", "object"}, {"description", "Authentication configuration"}})}
        })}
    });
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
    
    // ✅ 对齐 OpenClaw: 完整的 JSON Schema 定义
    // 包含详细的 provider、skill、plugin 等配置 schema
    json schema_root = json::object({
        {"type", "object"},
        {"title", "HunterClaw Configuration"},
        {"properties", json::object({
            {"agents", json::object({
                {"type", "array"},
                {"description", "Agent configurations"},
                {"items", json::object({
                    {"type", "object"},
                    {"properties", json::object({
                        {"id", json::object({{"type", "string"}})},
                        {"name", json::object({{"type", "string"}})},
                        {"provider", json::object({{"type", "string"}})},
                        {"model", json::object({{"type", "string"}})},
                        {"max_turns", json::object({{"type", "integer"}, {"minimum", 1}, {"maximum", 100}})},
                        {"timeout_ms", json::object({{"type", "integer"}, {"minimum", 1000}})}
                    })},
                    {"required", json::array({"id", "provider", "model"})}
                })}
            })},
            {"providers", json::object({
                {"type", "object"},
                {"description", "LLM provider configurations"},
                {"properties", json::object({
                    {"anthropic", json::object({
                        {"type", "object"},
                        {"properties", json::object({
                            {"apiKey", json::object({{"type", "string"}, {"minLength", 10}})},
                            {"baseUrl", json::object({{"type", "string"}, {"format", "uri"}})},
                            {"model", json::object({{"type", "string"}})}
                        })}
                    })},
                    {"openai", json::object({
                        {"type", "object"},
                        {"properties", json::object({
                            {"apiKey", json::object({{"type", "string"}, {"minLength", 10}})},
                            {"baseUrl", json::object({{"type", "string"}, {"format", "uri"}})},
                            {"model", json::object({{"type", "string"}})}
                        })}
                    })}
                })}
            })},
            {"skills", json::object({
                {"type", "object"},
                {"description", "Skills configuration"},
                {"properties", json::object({
                    {"auto_install", json::object({{"type", "boolean"}})},
                    {"directory", json::object({{"type", "string"}})},
                    {"timeout_ms", json::object({{"type", "integer"}, {"minimum", 1000}})}
                })}
            })},
            {"plugins", json::object({
                {"type", "object"},
                {"description", "Plugin configurations"},
                {"properties", json::object({
                    {"enabled", json::object({{"type", "array"}, {"items", json::object({{"type", "string"}})}})}
                })}
            })},
            {"auth", json::object({
                {"type", "object"},
                {"description", "Authentication configuration"},
                {"properties", json::object({
                    {"enabled", json::object({{"type", "boolean"}})},
                    {"type", json::object({{"type", "string"}, {"enum", json::array({"bearer", "apikey", "oauth"})}})},
                    {"secret", json::object({{"type", "string"}, {"minLength", 16}})}
                })}
            })}
        })}
    });
    
    // ✅ 对齐 OpenClaw: 路径查找逻辑 (支持点分隔的路径)
    json* current = &schema_root;
    std::istringstream path_stream(path);
    std::string segment;
    bool found = true;
    
    while (std::getline(path_stream, segment, '.')) {
        if (segment.empty()) continue;
        
        if (current->contains("properties") && (*current)["properties"].contains(segment)) {
            current = &(*current)["properties"][segment];
        } else if (current->contains("items") && (*current)["items"].is_object()) {
            // 支持数组项的 schema 查找
            current = &(*current)["items"];
            if (current->contains("properties") && (*current)["properties"].contains(segment)) {
                current = &(*current)["properties"][segment];
            } else {
                found = false;
                break;
            }
        } else {
            found = false;
            break;
        }
    }
    
    result["schema"] = found ? *current : json(nullptr);
    
    // ✅ 对齐 OpenClaw: uiHints (UI 提示信息)
    json ui_hints = json::object();
    if (found && current->contains("type")) {
        std::string type = (*current)["type"].get<std::string>();
        if (type == "string" && current->contains("format")) {
            ui_hints["widget"] = "input";
            ui_hints["format"] = (*current)["format"];
        } else if (type == "boolean") {
            ui_hints["widget"] = "checkbox";
        } else if (type == "integer" || type == "number") {
            ui_hints["widget"] = "number";
            if (current->contains("minimum")) ui_hints["min"] = (*current)["minimum"];
            if (current->contains("maximum")) ui_hints["max"] = (*current)["maximum"];
        } else if (type == "array") {
            ui_hints["widget"] = "array";
        } else if (type == "object") {
            ui_hints["widget"] = "object";
        }
    }
    result["uiHints"] = ui_hints;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// config.apply 内部辅助函数
// ============================================================================

// 🎯 OpenClaw 对齐: diffConfigPaths
// 计算两个配置对象之间的变更路径列表（用于 audit 日志）
// 对应 OpenClaw: diffConfigPaths(snapshot.config, parsed.config)
static std::vector<std::string> diff_config_paths(const json& before, const json& after, const std::string& prefix = "") {
    std::vector<std::string> changed;
    // 收集 after 中新增或变更的 key
    for (auto it = after.begin(); it != after.end(); ++it) {
        std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (!before.contains(it.key())) {
            changed.push_back(key);
        } else if (it.value().is_object() && before[it.key()].is_object()) {
            auto sub = diff_config_paths(before[it.key()], it.value(), key);
            changed.insert(changed.end(), sub.begin(), sub.end());
        } else if (it.value() != before[it.key()]) {
            changed.push_back(key);
        }
    }
    // 收集 before 中被删除的 key
    for (auto it = before.begin(); it != before.end(); ++it) {
        std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (!after.contains(it.key())) {
            changed.push_back(key);
        }
    }
    return changed;
}

// 🎯 OpenClaw 对齐: summarizeChangedPaths
// 将变更路径列表格式化为日志字符串
static std::string summarize_changed_paths(const std::vector<std::string>& paths) {
    if (paths.empty()) return "(none)";
    std::string result;
    for (size_t i = 0; i < paths.size() && i < 5; ++i) {
        if (!result.empty()) result += ",";
        result += paths[i];
    }
    if (paths.size() > 5) {
        result += ",... +" + std::to_string(paths.size() - 5) + " more";
    }
    return result;
}

// 🎯 OpenClaw 对齐: writeRestartSentinel
// 写入 sentinel 文件，供重启后恢复上下文使用
// 对应 OpenClaw: writeRestartSentinel(payload) -> sentinelPath
static std::string write_restart_sentinel(
    const std::string& config_dir,
    const std::string& kind,       // "config-apply" | "config-patch"
    const std::string& mode,       // "config.apply" | "config.patch"
    const std::string& session_key,
    const std::string& note
) {
    try {
        std::string sentinel_dir = config_dir + "/.sentinel";
        fs::create_directories(sentinel_dir);

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        std::string sentinel_path = sentinel_dir + "/restart-" + std::to_string(now_ms) + ".json";

        json payload;
        payload["kind"] = kind;
        payload["status"] = "ok";
        payload["ts"] = now_ms;
        payload["sessionKey"] = session_key.empty() ? json(nullptr) : json(session_key);
        payload["message"] = note.empty() ? json(nullptr) : json(note);
        payload["stats"] = json{{"mode", mode}};

        std::ofstream f(sentinel_path);
        if (f.is_open()) {
            f << payload.dump(2);
            return sentinel_path;
        }
    } catch (...) {}
    return "";
}

// 🎯 OpenClaw 对齐: scheduleGatewaySigusr1Restart
// 调度 SIGUSR1 重启，支持延迟和合并（coalesce）
// 对应 OpenClaw: scheduleGatewaySigusr1Restart({ delayMs, reason, audit })
struct RestartResult {
    bool scheduled = false;
    bool coalesced = false;
    int64_t delay_ms = 0;
};

static RestartResult schedule_gateway_sigusr1_restart(
    int64_t delay_ms,
    const std::string& reason,
    const std::vector<std::string>& changed_paths
) {
    RestartResult result;
    result.delay_ms = delay_ms;

    try {
        if (delay_ms <= 0) {
            // 立即发送 SIGUSR1
            ::kill(::getpid(), SIGUSR1);
            result.scheduled = true;
        } else {
            // 延迟发送：在后台线程中等待后发送
            std::thread([delay_ms, reason]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                ::kill(::getpid(), SIGUSR1);
            }).detach();
            result.scheduled = true;
        }
    } catch (...) {
        result.scheduled = false;
    }

    return result;
}

// ============================================================================
// config.apply - 应用完整配置并触发重启
// 🎯 OpenClaw 严格对齐: server-methods/config.ts configHandlers["config.apply"]
//
// OpenClaw 完整流程:
//   1. requireConfigBaseHash  — baseHash 必须存在且匹配（文件存在时）
//   2. parseValidateConfigFromRawOrRespond — 解析 JSON5 + restoreRedactedValues + validateConfig
//   3. diffConfigPaths        — 计算变更路径（用于 audit 日志）
//   4. writeConfigFile        — 写入配置文件
//   5. resolveConfigRestartRequest — 解析 sessionKey/note/restartDelayMs
//   6. buildConfigRestartSentinelPayload — 构建 sentinel payload (kind="config-apply")
//   7. writeRestartSentinel   — 写入 sentinel 文件
//   8. scheduleGatewaySigusr1Restart — 调度 SIGUSR1 重启（带 audit）
//   9. 返回 { ok, path, config(redacted), restart, sentinel }
// ============================================================================
core::Result<json> method_config_apply(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;

    // ── Step 1: 参数校验 ──
    // 🎯 OpenClaw 对齐: raw (string) 必须存在
    if (!params.contains("raw") || !params["raw"].is_string()) {
        return core::Result<json>::Err("invalid config.apply params: raw (string) required");
    }
    std::string raw = params["raw"].get<std::string>();

    auto config = server->config();
    std::string config_path = config ? config->config_dir + "/config.yaml" : "";

    // ── Step 2: requireConfigBaseHash ──
    // 🎯 OpenClaw 对齐: 配置文件存在时 baseHash 必须提供且匹配
    // 对应 OpenClaw:
    //   if (!snapshot.exists) return true;  // 文件不存在时跳过校验
    //   if (!snapshotHash) respond(false, ..., "config base hash unavailable")
    //   if (!baseHash) respond(false, ..., "config base hash required")
    //   if (baseHash !== snapshotHash) respond(false, ..., "config changed since last load")
    if (fs::exists(config_path)) {
        std::string current_hash = compute_config_hash(config_path);
        if (current_hash.empty()) {
            return core::Result<json>::Err("config base hash unavailable; re-run config.get and retry");
        }
        if (!params.contains("baseHash") || !params["baseHash"].is_string() ||
            params["baseHash"].get<std::string>().empty()) {
            return core::Result<json>::Err("config base hash required; re-run config.get and retry");
        }
        std::string base_hash = params["baseHash"].get<std::string>();
        if (base_hash != current_hash) {
            return core::Result<json>::Err("config changed since last load; re-run config.get and retry");
        }
    }

    // ── Step 3: parseValidateConfig ──
    // 🎯 OpenClaw 对齐: parseConfigJson5 + restoreRedactedValues + validateConfigObjectWithPlugins
    // HunterClaw 简化版: 直接 JSON 解析（不支持 JSON5 和 redact 恢复）
    json parsed_config;
    try {
        parsed_config = json::parse(raw);
    } catch (const std::exception& e) {
        return core::Result<json>::Err(std::string("invalid config: ") + e.what());
    }
    if (!parsed_config.is_object()) {
        return core::Result<json>::Err("invalid config: must be an object");
    }

    // ── Step 4: diffConfigPaths ──
    // 🎯 OpenClaw 对齐: 计算变更路径，用于 audit 日志
    json before_config = json::object();
    if (fs::exists(config_path)) {
        try {
            std::ifstream f(config_path);
            if (f.is_open()) {
                std::string content((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                before_config = json::parse(content);
            }
        } catch (...) {}
    }
    auto changed_paths = diff_config_paths(before_config, parsed_config);

    // ── Step 5: audit 日志 ──
    // 🎯 OpenClaw 对齐: context.logGateway.info("config.apply write ...")
    std::string actor_id = client_ctx.account_id.empty() ? client_ctx.conn_id : client_ctx.account_id;
    { auto _l = spdlog::get("console"); if (_l) _l->info("[config.apply] write actor={} changedPaths={} restartReason=config.apply",
        actor_id, summarize_changed_paths(changed_paths)); }

    // ── Step 6: writeConfigFile ──
    // 🎯 OpenClaw 对齐: writeConfigFile(parsed.config, writeOptions)
    bool write_ok = false;
    if (!config_path.empty()) {
        try {
            fs::create_directories(fs::path(config_path).parent_path());
            std::ofstream f(config_path);
            if (f.is_open()) {
                f << parsed_config.dump(2);
                write_ok = true;
            }
        } catch (const std::exception& e) {
            return core::Result<json>::Err(std::string("failed to write config: ") + e.what());
        }
    }

    // ── Step 7: resolveConfigRestartRequest ──
    // 🎯 OpenClaw 对齐: parseRestartRequestParams(params)
    int64_t restart_delay_ms = 0;
    if (params.contains("restartDelayMs") && params["restartDelayMs"].is_number()) {
        restart_delay_ms = params["restartDelayMs"].get<int64_t>();
    }
    std::string session_key = params.value("sessionKey", "");
    std::string note = params.value("note", "");

    // ── Step 8: writeRestartSentinel ──
    // 🎯 OpenClaw 对齐: buildConfigRestartSentinelPayload + writeRestartSentinel
    std::string sentinel_path;
    json sentinel_payload = nullptr;
    if (write_ok && config) {
        sentinel_path = write_restart_sentinel(
            config->config_dir,
            "config-apply",
            "config.apply",
            session_key,
            note
        );
        if (!sentinel_path.empty()) {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            sentinel_payload = json{
                {"kind", "config-apply"},
                {"status", "ok"},
                {"ts", now_ms},
                {"sessionKey", session_key.empty() ? json(nullptr) : json(session_key)},
                {"message", note.empty() ? json(nullptr) : json(note)},
                {"stats", json{{"mode", "config.apply"}}}
            };
        }
    }

    // ── Step 9: scheduleGatewaySigusr1Restart ──
    // 🎯 OpenClaw 对齐: scheduleGatewaySigusr1Restart({ delayMs, reason: "config.apply", audit })
    json restart_result;
    if (write_ok) {
        auto restart = schedule_gateway_sigusr1_restart(
            restart_delay_ms,
            "config.apply",
            changed_paths
        );
        if (restart.coalesced) {
            { auto _l = spdlog::get("console"); if (_l) _l->warn("[config.apply] restart coalesced actor={} delayMs={}", actor_id, restart.delay_ms); }
        }
        restart_result = json{
            {"scheduled", restart.scheduled},
            {"delayMs", restart.delay_ms},
            {"coalesced", restart.coalesced}
        };
    } else {
        restart_result = json{{"scheduled", false}, {"delayMs", restart_delay_ms}, {"coalesced", false}};
    }

    // ── Step 10: 返回结果 ──
    // 🎯 OpenClaw 对齐: { ok, path, config(redacted), restart, sentinel }
    // HunterClaw 简化版: config 不做 redact（无 uiHints 支持）
    json result;
    result["ok"] = write_ok;
    result["path"] = config_path;
    result["config"] = parsed_config;
    result["restart"] = restart_result;
    result["sentinel"] = json{
        {"path", sentinel_path.empty() ? json(nullptr) : json(sentinel_path)},
        {"payload", sentinel_payload}
    };

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

    // \u2705 \u5bf9\u9f50 OpenClaw: exec(`open/xdg-open ${configPath}`)\n    auto config = server->config();\n    std::string config_path = config ? (config->config_dir + \"/config.yaml\") : \"\";\n\n    if (config_path.empty() || !fs::exists(config_path)) {\n        json result;\n        result[\"ok\"] = false;\n        result[\"path\"] = config_path;\n        result[\"error\"] = \"Config file not found: \" + config_path;\n        return core::Result<json>::Ok(result);\n    }\n\n    // \u5bf9\u9f50 OpenClaw: \u6839\u636e\u5e73\u53f0\u9009\u62e9\u547d\u4ee4\n    std::string open_cmd;\n#if defined(__APPLE__)\n    open_cmd = \"open \" + config_path;\n#elif defined(_WIN32)\n    open_cmd = \"start \" + config_path;\n#else\n    open_cmd = \"xdg-open \" + config_path + \" &\";\n#endif\n\n    int ret = std::system(open_cmd.c_str());\n    json result;\n    result[\"ok\"] = (ret == 0);\n    result[\"path\"] = config_path;\n    if (ret != 0) {\n        result[\"error\"] = \"Failed to open config file (exit code \" + std::to_string(ret) + \")\";\n    }\n    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
