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
    // ★ 与 create_default_config 一致：data 始终是 object，不留 null。
    //   下面有两条提前 return（路径为空 / 文件打不开），它们也要带上这个初始化。
    config.data = nlohmann::json::object();

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

        // ★ 保留原始 JSON。
        //
        // types.h:481-483 已声明 `data` 的用途是「让 channel 插件访问任意
        // 配置键（如 cfg.data["channels"]["telegram"]["token"]）」，
        // 但此前【从未赋值】—— 所有依赖 config.data 的子系统都在读一个空
        // JSON，表现为「配置写了但功能不生效，且日志里一个字都没有」。
        //
        // ⚠ 必须在解析强类型字段【之前】赋值：下面的解析可能 throw
        //   （字段类型不符），而 data 至少应保留已成功读到的部分。
        //   这里 j 已完整解析成功，故放在紧随其后即可。
        config.data = j;

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
        // ★ 以原始 JSON 为底，再覆盖强类型字段。
        //
        // ⚠ 此前是从空 json 重建，会【静默丢弃】所有未被强类型化的配置节点
        //   （channels / plugins / remoteA2a / mcpServers ...）。
        //   任何一次 save_config 都会把用户手写的配置抹掉大半 ——
        //   而调用方通常只是想改一个端口。
        nlohmann::json j = config.data.is_object() ? config.data : nlohmann::json::object();

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
    // ★ data 初始化为空 object 而非 null。
    //   留 null 会让所有 `config.data.contains(...)` 的调用点
    //   在 nlohmann 的 null 上做 contains —— 虽然返回 false 不抛异常，
    //   但 `config.data["x"]` 形式的写入会把 null 隐式转成 object，
    //   行为取决于访问顺序。统一成 object 消除这种不确定性。
    config.data = nlohmann::json::object();
    return config;
}

} // namespace core
} // namespace hunterclaw
