#include "hunterclaw/core/config_loader.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/infra/fs_safe.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <regex>

namespace hunterclaw {
namespace core {

// ==================== Helper Functions ====================
// These are legacy helper functions for config serialization
// Current config loading is handled by ConfigSystem

static Result<Config> ParseConfigJson(const nlohmann::json& json) {
    Config config;
    
    try {
        // Gateway
        if (json.contains("gateway")) {
            auto& gw = json["gateway"];
            config.gateway.host = gw.value("host", "127.0.0.1");
            config.gateway.port = gw.value("port", 18789);
            config.gateway.tls_enabled = gw.value("tls", false);
        }
        
        // Other fields (daemon, security, logging) are not in current Config struct
        // These are now handled by ConfigSystem
        
        return Result<Config>::Ok(config);
        
    } catch (const std::exception& e) {
        return Result<Config>::Err("Failed to parse config JSON: " + std::string(e.what()));
    }
}

static nlohmann::json ConfigToJson(const Config& config) {
    return {
        {"gateway", {
            {"host", config.gateway.host},
            {"port", config.gateway.port},
            {"tls", config.gateway.tls_enabled}
        }}
        // Other fields (daemon, security, logging) are not in current Config struct
    };
}

// ==================== ConfigLoader 实现 ====================

// Note: This is a legacy function. Default config is now handled by ConfigSystem
Config ConfigLoader::Default() {
    Config config;
    config.gateway.host = "127.0.0.1";
    config.gateway.port = 18789;
    config.gateway.tls_enabled = false;
    
    // Other fields (daemon, security, logging) are not in current Config struct
    // These are now handled by ConfigSystem
    
    return config;
}

Result<Config> ConfigLoader::Load(const LoadOptions& opts) {
    // 1. 加载默认配置
    Config config = Default();
    
    // 2. 从文件加载 (如果存在)
    std::string config_path = opts.config_file.value_or(
        utils::expand_path("~/.hunterclaw/config.json")
    );
    
    if (utils::file_exists(config_path)) {
        auto file_result = LoadFromFile(config_path);
        if (file_result.is_ok()) {
            config = Merge(config, file_result.unwrap());
        }
    }
    
    // 3. 从环境变量加载
    Config env_config = LoadFromEnv(opts.env_prefix);
    config = Merge(config, env_config);
    
    // 4. 从 CLI 选项加载 (最高优先级)
    if (opts.cli_port.has_value()) {
        config.gateway.port = opts.cli_port.value();
    }
    // cli_token, cli_tls, cli_working_dir are not in current Config struct
    
    // 5. 验证配置
    auto validate_result = Validate(config);
    if (validate_result.is_err()) {
        return Result<Config>::Err(validate_result.error);
    }
    
    return Result<Config>::Ok(config);
}

// Note: This is a legacy function. Config merging is now handled by ConfigSystem
Config ConfigLoader::Merge(const Config& base, const Config& override_cfg) {
    Config result = base;
    
    // Gateway (只覆盖非默认值)
    if (override_cfg.gateway.host != "127.0.0.1") result.gateway.host = override_cfg.gateway.host;
    if (override_cfg.gateway.port != 18789) result.gateway.port = override_cfg.gateway.port;
    result.gateway.tls_enabled = override_cfg.gateway.tls_enabled;
    
    // Other fields (daemon, security, logging) are not in current Config struct
    
    return result;
}

// Note: This is a legacy function. Config validation is now handled by ConfigSystem
Result<void> ConfigLoader::Validate(const Config& config) {
    // 验证端口范围
    if (config.gateway.port == 0 || config.gateway.port > 65535) {
        return Result<void>::Err("Invalid port: " + std::to_string(config.gateway.port));
    }
    
    // Other validations (daemon, security, logging) are not in current Config struct
    
    return Result<void>::Ok();
}

Result<Config> ConfigLoader::LoadFromFile(const std::string& path) {
    auto expanded_path = utils::expand_path(path);
    
    // 读取文件
    std::ifstream file(expanded_path);
    if (!file.is_open()) {
        return Result<Config>::Err("Failed to open config file: " + expanded_path);
    }
    
    // 解析 JSON
    try {
        nlohmann::json json;
        file >> json;
        return ParseConfigJson(json);
    } catch (const std::exception& e) {
        return Result<Config>::Err("Failed to parse config file: " + std::string(e.what()));
    }
}

Result<void> ConfigLoader::SaveToFile(const Config& config, const std::string& path) {
    auto expanded_path = utils::expand_path(path);
    
    // 转换为 JSON
    auto json = ConfigToJson(config);
    std::string json_str = json.dump(2);  // 2 spaces indent
    
    // 写入文件
    std::ofstream file(expanded_path);
    if (!file.is_open()) {
        return Result<void>::Err("Failed to open file for writing: " + expanded_path);
    }
    
    file << json_str;
    return Result<void>::Ok();
}

Config ConfigLoader::LoadFromEnv(const std::string& prefix) {
    Config config;
    
    // Helper: 读取环境变量
    auto get_env = [&](const std::string& name) -> std::optional<std::string> {
        const char* value = std::getenv((prefix + name).c_str());
        if (value) return std::string(value);
        return std::nullopt;
    };
    
    // Gateway
    if (auto host = get_env("HOST")) config.gateway.host = *host;
    if (auto port = get_env("PORT")) config.gateway.port = std::stoi(*port);
    if (auto tls = get_env("TLS")) config.gateway.tls_enabled = (*tls == "true" || *tls == "1");
    
    // Other fields (daemon, security, logging) are not in current Config struct
    
    return config;
}

Result<uint64_t> ConfigLoader::ParseSize(const std::string& size_str) {
    std::regex size_regex(R"(^(\d+(?:\.\d+)?)\s*(KB|MB|GB|TB|B)?$)", std::regex::icase);
    std::smatch match;
    
    if (!std::regex_match(size_str, match, size_regex)) {
        return Result<uint64_t>::Err("Invalid size format: " + size_str);
    }
    
    double value = std::stod(match[1].str());
    std::string unit = match[2].str();
    
    // 转换为小写
    std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);
    
    uint64_t multiplier = 1;
    if (unit == "kb") multiplier = 1024;
    else if (unit == "mb") multiplier = 1024 * 1024;
    else if (unit == "gb") multiplier = 1024 * 1024 * 1024;
    else if (unit == "tb") multiplier = 1024ULL * 1024 * 1024 * 1024;
    
    return Result<uint64_t>::Ok(static_cast<uint64_t>(value * multiplier));
}

}  // namespace core
}  // namespace hunterclaw
