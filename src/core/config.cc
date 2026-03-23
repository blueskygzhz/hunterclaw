#include "hunterclaw/core/types.h"
#include "hunterclaw/core/utils.h"
#include <fstream>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace core {

// Load configuration from file
Result<Config> load_config(const std::string& config_path) {
    // ✅ 对齐 OpenClaw: 从 JSON 文件加载配置
    Config config;
    config.workspace_dir = ".";
    config.config_dir = "~/.hunterclaw";
    config.gateway.host = "127.0.0.1";
    config.gateway.port = 18789;
    config.gateway.tls_enabled = false;
    config.gateway.auth.enabled = false;
    config.gateway.auth.mode = "none";

    if (config_path.empty()) {
        return Result<Config>::Ok(std::move(config));
    }

    // 尝试从文件加载
    std::ifstream file(config_path);
    if (!file.is_open()) {
        // 文件不存在，返回默认配置
        return Result<Config>::Ok(std::move(config));
    }

    try {
        nlohmann::json j;
        file >> j;

        // 解析 gateway 配置
        if (j.contains("gateway") && j["gateway"].is_object()) {
            const auto& gw = j["gateway"];
            if (gw.contains("host") && gw["host"].is_string())
                config.gateway.host = gw["host"].get<std::string>();
            if (gw.contains("port") && gw["port"].is_number())
                config.gateway.port = gw["port"].get<int>();
            if (gw.contains("tls") && gw["tls"].is_boolean())
                config.gateway.tls_enabled = gw["tls"].get<bool>();

            // auth 配置
            if (gw.contains("auth") && gw["auth"].is_object()) {
                const auto& auth = gw["auth"];
                if (auth.contains("enabled") && auth["enabled"].is_boolean())
                    config.gateway.auth.enabled = auth["enabled"].get<bool>();
                if (auth.contains("mode") && auth["mode"].is_string())
                    config.gateway.auth.mode = auth["mode"].get<std::string>();
                if (auth.contains("bearerToken") && auth["bearerToken"].is_string())
                    config.gateway.auth.bearer_token = auth["bearerToken"].get<std::string>();
                if (auth.contains("password") && auth["password"].is_string())
                    config.gateway.auth.password = auth["password"].get<std::string>();
            }
        }

        // 解析 workspace
        if (j.contains("workspace") && j["workspace"].is_string())
            config.workspace_dir = j["workspace"].get<std::string>();

        return Result<Config>::Ok(std::move(config));

    } catch (const std::exception& e) {
        return Result<Config>::Err(std::string("Failed to parse config: ") + e.what());
    }
}

// Save configuration to file
Result<bool> save_config(const std::string& config_path, const Config& config) {
    // ✅ 对齐 OpenClaw: 将配置写入 JSON 文件
    if (config_path.empty()) {
        return Result<bool>::Err("Config path is empty");
    }

    try {
        nlohmann::json j;
        j["gateway"]["host"] = config.gateway.host;
        j["gateway"]["port"] = config.gateway.port;
        j["gateway"]["tls"] = config.gateway.tls_enabled;
        j["gateway"]["auth"]["enabled"] = config.gateway.auth.enabled;
        j["gateway"]["auth"]["mode"] = config.gateway.auth.mode;
        if (!config.gateway.auth.bearer_token.empty())
            j["gateway"]["auth"]["bearerToken"] = config.gateway.auth.bearer_token;
        j["workspace"] = config.workspace_dir;

        std::ofstream file(config_path);
        if (!file.is_open()) {
            return Result<bool>::Err("Failed to open config file for writing: " + config_path);
        }
        file << j.dump(2);
        return Result<bool>::Ok(true);

    } catch (const std::exception& e) {
        return Result<bool>::Err(std::string("Failed to save config: ") + e.what());
    }
}

// Create default configuration
Config create_default_config() {
    Config config;
    config.gateway.host = "0.0.0.0";
    config.gateway.port = 18789;
    config.gateway.tls_enabled = false;
    
    // Auth 配置 (对齐 OpenClaw)
    config.gateway.auth.enabled = false;  // 默认不开启鉴权
    config.gateway.auth.mode = "none";    // 鉴权模式: none, bearer, device, challenge
    config.workspace_dir = ".";
    config.config_dir = "~/.hunterclaw";
    return config;
}

} // namespace core
} // namespace hunterclaw
