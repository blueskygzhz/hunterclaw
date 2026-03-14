#include "hunterclaw/core/config_loader.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/infra/fs_safe.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <regex>

namespace hunterclaw {
namespace core {

// ==================== Config JSON 序列化 ====================

Result<Config> Config::FromJson(const nlohmann::json& json) {
    Config config;
    
    try {
        // Gateway
        if (json.contains("gateway")) {
            auto& gw = json["gateway"];
            config.gateway.host = gw.value("host", "127.0.0.1");
            config.gateway.port = gw.value("port", 18789);
            config.gateway.tls = gw.value("tls", false);
            config.gateway.token = gw.value("token", "");
        }
        
        // Daemon
        if (json.contains("daemon")) {
            auto& dm = json["daemon"];
            config.daemon.auto_restart = dm.value("auto_restart", true);
            config.daemon.restart_limit = dm.value("restart_limit", 5);
            config.daemon.restart_interval = dm.value("restart_interval", 60);
            config.daemon.working_directory = dm.value("working_directory", "");
        }
        
        // Security
        if (json.contains("security")) {
            auto& sec = json["security"];
            config.security.exec_policy = sec.value("exec_policy", "allowlist");
            config.security.path_sandbox = sec.value("path_sandbox", true);
            config.security.symlink_protection = sec.value("symlink_protection", true);
        }
        
        // Logging
        if (json.contains("logging")) {
            auto& log = json["logging"];
            config.logging.level = log.value("level", "info");
            config.logging.file = log.value("file", "");
            config.logging.rotate_size = log.value("rotate_size", "10MB");
            config.logging.rotate_count = log.value("rotate_count", 5);
            config.logging.format = log.value("format", "console");
        }
        
        return Result<Config>::Ok(config);
        
    } catch (const std::exception& e) {
        return Result<Config>::Err("Failed to parse config JSON: " + std::string(e.what()));
    }
}

nlohmann::json Config::ToJson() const {
    return {
        {"gateway", {
            {"host", gateway.host},
            {"port", gateway.port},
            {"tls", gateway.tls},
            {"token", gateway.token}
        }},
        {"daemon", {
            {"auto_restart", daemon.auto_restart},
            {"restart_limit", daemon.restart_limit},
            {"restart_interval", daemon.restart_interval},
            {"working_directory", daemon.working_directory}
        }},
        {"security", {
            {"exec_policy", security.exec_policy},
            {"path_sandbox", security.path_sandbox},
            {"symlink_protection", security.symlink_protection}
        }},
        {"logging", {
            {"level", logging.level},
            {"file", logging.file},
            {"rotate_size", logging.rotate_size},
            {"rotate_count", logging.rotate_count},
            {"format", logging.format}
        }}
    };
}

// ==================== ConfigLoader 实现 ====================

Config ConfigLoader::Default() {
    Config config;
    config.gateway.host = "127.0.0.1";
    config.gateway.port = 18789;
    config.gateway.tls = false;
    
    config.daemon.auto_restart = true;
    config.daemon.restart_limit = 5;
    config.daemon.restart_interval = 60;
    
    config.security.exec_policy = "allowlist";
    config.security.path_sandbox = true;
    config.security.symlink_protection = true;
    
    config.logging.level = "info";
#ifdef __APPLE__
    config.logging.file = utils::expand_path("~/Library/Logs/HunterClaw/daemon.log");
#elif defined(__linux__)
    config.logging.file = "/var/log/hunterclaw/daemon.log";
#elif defined(_WIN32)
    config.logging.file = utils::expand_path("%APPDATA%\\HunterClaw\\daemon.log");
#endif
    config.logging.rotate_size = "10MB";
    config.logging.rotate_count = 5;
    config.logging.format = "console";
    
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
    if (opts.cli_token.has_value()) {
        config.gateway.token = opts.cli_token.value();
    }
    if (opts.cli_tls.has_value()) {
        config.gateway.tls = opts.cli_tls.value();
    }
    if (opts.cli_working_dir.has_value()) {
        config.daemon.working_directory = opts.cli_working_dir.value();
    }
    
    // 5. 验证配置
    auto validate_result = Validate(config);
    if (validate_result.is_err()) {
        return Result<Config>::Err(validate_result.error);
    }
    
    return Result<Config>::Ok(config);
}

Config ConfigLoader::Merge(const Config& base, const Config& override_cfg) {
    Config result = base;
    
    // Gateway (只覆盖非默认值)
    if (override_cfg.gateway.host != "127.0.0.1") result.gateway.host = override_cfg.gateway.host;
    if (override_cfg.gateway.port != 18789) result.gateway.port = override_cfg.gateway.port;
    result.gateway.tls = override_cfg.gateway.tls;
    if (!override_cfg.gateway.token.empty()) result.gateway.token = override_cfg.gateway.token;
    
    // Daemon
    result.daemon.auto_restart = override_cfg.daemon.auto_restart;
    result.daemon.restart_limit = override_cfg.daemon.restart_limit;
    result.daemon.restart_interval = override_cfg.daemon.restart_interval;
    if (!override_cfg.daemon.working_directory.empty()) {
        result.daemon.working_directory = override_cfg.daemon.working_directory;
    }
    
    // Security
    if (override_cfg.security.exec_policy != "allowlist") {
        result.security.exec_policy = override_cfg.security.exec_policy;
    }
    result.security.path_sandbox = override_cfg.security.path_sandbox;
    result.security.symlink_protection = override_cfg.security.symlink_protection;
    
    // Logging
    if (override_cfg.logging.level != "info") result.logging.level = override_cfg.logging.level;
    if (!override_cfg.logging.file.empty()) result.logging.file = override_cfg.logging.file;
    result.logging.rotate_size = override_cfg.logging.rotate_size;
    result.logging.rotate_count = override_cfg.logging.rotate_count;
    if (override_cfg.logging.format != "console") result.logging.format = override_cfg.logging.format;
    
    return result;
}

Result<void> ConfigLoader::Validate(const Config& config) {
    // 验证端口范围
    if (config.gateway.port == 0 || config.gateway.port > 65535) {
        return Result<void>::Err("Invalid port: " + std::to_string(config.gateway.port));
    }
    
    // 验证 exec_policy
    if (config.security.exec_policy != "deny" &&
        config.security.exec_policy != "allowlist" &&
        config.security.exec_policy != "full") {
        return Result<void>::Err("Invalid exec_policy: " + config.security.exec_policy);
    }
    
    // 验证日志级别
    if (config.logging.level != "debug" &&
        config.logging.level != "info" &&
        config.logging.level != "warn" &&
        config.logging.level != "error") {
        return Result<void>::Err("Invalid log level: " + config.logging.level);
    }
    
    // 验证日志格式
    if (config.logging.format != "json" &&
        config.logging.format != "console" &&
        config.logging.format != "syslog") {
        return Result<void>::Err("Invalid log format: " + config.logging.format);
    }
    
    return Result<void>::Ok();
}

Result<Config> ConfigLoader::LoadFromFile(const std::string& path) {
    auto expanded_path = utils::expand_path(path);
    
    // 安全读取文件
    auto read_result = infra::fs_safe::SafeRead(expanded_path);
    if (read_result.is_err()) {
        return Result<Config>::Err("Failed to read config file: " + read_result.error);
    }
    
    // 解析 JSON
    try {
        auto json = nlohmann::json::parse(read_result.unwrap());
        return Config::FromJson(json);
    } catch (const std::exception& e) {
        return Result<Config>::Err("Failed to parse config file: " + std::string(e.what()));
    }
}

Result<void> ConfigLoader::SaveToFile(const Config& config, const std::string& path) {
    auto expanded_path = utils::expand_path(path);
    
    // 转换为 JSON
    auto json = config.ToJson();
    std::string json_str = json.dump(2);  // 2 spaces indent
    
    // 安全写入文件
    auto write_result = infra::fs_safe::SafeWrite(expanded_path, json_str);
    if (write_result.is_err()) {
        return Result<void>::Err("Failed to write config file: " + write_result.error);
    }
    
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
    if (auto tls = get_env("TLS")) config.gateway.tls = (*tls == "true" || *tls == "1");
    if (auto token = get_env("TOKEN")) config.gateway.token = *token;
    
    // Daemon
    if (auto restart = get_env("AUTO_RESTART")) {
        config.daemon.auto_restart = (*restart == "true" || *restart == "1");
    }
    if (auto limit = get_env("RESTART_LIMIT")) config.daemon.restart_limit = std::stoi(*limit);
    if (auto interval = get_env("RESTART_INTERVAL")) config.daemon.restart_interval = std::stoi(*interval);
    if (auto workdir = get_env("WORKING_DIR")) config.daemon.working_directory = *workdir;
    
    // Security
    if (auto policy = get_env("EXEC_POLICY")) config.security.exec_policy = *policy;
    if (auto sandbox = get_env("PATH_SANDBOX")) {
        config.security.path_sandbox = (*sandbox == "true" || *sandbox == "1");
    }
    
    // Logging
    if (auto level = get_env("LOG_LEVEL")) config.logging.level = *level;
    if (auto file = get_env("LOG_FILE")) config.logging.file = *file;
    if (auto size = get_env("LOG_ROTATE_SIZE")) config.logging.rotate_size = *size;
    if (auto count = get_env("LOG_ROTATE_COUNT")) config.logging.rotate_count = std::stoi(*count);
    if (auto format = get_env("LOG_FORMAT")) config.logging.format = *format;
    
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
