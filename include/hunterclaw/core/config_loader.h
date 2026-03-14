#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/result.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <cstdint>

namespace hunterclaw {
namespace core {

// Gateway 配置
struct GatewayConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 18789;
    bool tls = false;
    std::string token;
};

// Daemon 配置
struct DaemonConfig {
    bool auto_restart = true;
    int restart_limit = 5;
    int restart_interval = 60;  // seconds
    std::string working_directory;
};

// 安全配置
struct SecurityConfig {
    std::string exec_policy = "allowlist";  // deny | allowlist | full
    bool path_sandbox = true;
    bool symlink_protection = true;
};

// 日志配置
struct LogConfig {
    std::string level = "info";  // debug | info | warn | error
    std::string file;  // 空字符串 = stdout
    std::string rotate_size = "10MB";
    int rotate_count = 5;
    std::string format = "console";  // json | console | syslog
};

// 完整配置
struct Config {
    GatewayConfig gateway;
    DaemonConfig daemon;
    SecurityConfig security;
    LogConfig logging;
    
    // 从 JSON 解析
    static Result<Config> FromJson(const nlohmann::json& json);
    
    // 转换为 JSON
    nlohmann::json ToJson() const;
};

// 配置加载选项
struct LoadOptions {
    std::optional<uint16_t> cli_port;
    std::optional<std::string> cli_token;
    std::optional<bool> cli_tls;
    std::optional<std::string> cli_working_dir;
    
    std::string env_prefix = "HUNTERCLAW_";
    std::optional<std::string> config_file;  // 空 = ~/.hunterclaw/config.json
};

class ConfigLoader {
public:
    // 加载配置 (优先级: CLI > env > file > default)
    static Result<Config> Load(const LoadOptions& opts = {});
    
    // 配置合并 (override 优先)
    static Config Merge(const Config& base, const Config& override_cfg);
    
    // 配置验证
    static Result<void> Validate(const Config& config);
    
    // 默认配置
    static Config Default();
    
    // 从文件加载
    static Result<Config> LoadFromFile(const std::string& path);
    
    // 保存到文件
    static Result<void> SaveToFile(const Config& config, const std::string& path);
    
private:
    // 从环境变量加载
    static Config LoadFromEnv(const std::string& prefix);
    
    // 解析大小字符串 (10MB -> 10485760)
    static Result<uint64_t> ParseSize(const std::string& size_str);
};

}  // namespace core
}  // namespace hunterclaw
