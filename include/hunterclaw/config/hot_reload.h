#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <functional>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace config {

using json = nlohmann::json;

// ============================================================================
// 1. 配置模式枚举
// ============================================================================

enum class ConfigFormat {
    JSON,           // 标准 JSON
    JSON5,          // JSON with comments
    YAML,           // YAML 格式
    TOML            // TOML 格式
};

// ============================================================================
// 2. 配置加载选项
// ============================================================================

struct ConfigLoadOptions {
    ConfigFormat format = ConfigFormat::JSON5;
    bool expand_env_vars = true;            // ${VAR} 环境变量替换
    bool allow_includes = true;             // includes: ["local.json"] 支持
    bool validate_schema = true;            // Zod-style 验证
    bool backup_on_write = true;            // 自动备份旧配置
    int max_backups = 10;                   // 最多保留 10 个备份
};

// ============================================================================
// 3. 配置验证器
// ============================================================================

class ConfigValidator {
public:
    struct ValidationResult {
        bool valid = true;
        std::vector<std::string> errors;
    };
    
    virtual ~ConfigValidator() = default;
    
    virtual ValidationResult validate(const json& config) = 0;
};

// ============================================================================
// 4. 配置加载器
// ============================================================================

class ConfigLoader {
public:
    explicit ConfigLoader(const ConfigLoadOptions& options = {})
        : options_(options) {}
    
    struct LoadResult {
        bool success = false;
        json config;
        std::vector<std::string> errors;
        std::string loaded_from;        // 实际加载的文件路径
    };
    
    LoadResult load(const std::string& path) {
        try {
            // 1. 读取文件
            auto content = read_file(path);
            if (!content) {
                return LoadResult{false, {}, {"File not found: " + path}, ""};
            }
            
            // 2. 解析配置
            json config = parse_config(*content, options_.format);
            
            // 3. 环境变量替换
            if (options_.expand_env_vars) {
                expand_environment_variables(config);
            }
            
            // 4. 处理 includes
            if (options_.allow_includes) {
                if (config.contains("includes")) {
                    for (const auto& include_path : config["includes"]) {
                        auto include_result = load(include_path);
                        if (include_result.success) {
                            merge_configs(config, include_result.config);
                        }
                    }
                }
            }
            
            // 5. 验证配置
            if (options_.validate_schema && validator_) {
                auto validation = validator_->validate(config);
                if (!validation.valid) {
                    return LoadResult{false, {}, validation.errors, path};
                }
            }
            
            return LoadResult{true, config, {}, path};
            
        } catch (const std::exception& e) {
            return LoadResult{false, {}, {"Parse error: " + std::string(e.what())}, path};
        }
    }
    
    void set_validator(std::shared_ptr<ConfigValidator> validator) {
        validator_ = std::move(validator);
    }

private:
    std::optional<std::string> read_file(const std::string& path) {
        // 文件读取实现 (略)
        return std::nullopt;
    }
    
    json parse_config(const std::string& content, ConfigFormat format) {
        switch (format) {
            case ConfigFormat::JSON:
            case ConfigFormat::JSON5:
                return json::parse(content, nullptr, true, true); // allow_exceptions, ignore_comments
            default:
                throw std::runtime_error("Unsupported config format");
        }
    }
    
    void expand_environment_variables(json& config) {
        // 递归替换 ${VAR} 和 ${VAR:-default}
        if (config.is_string()) {
            std::string value = config.get<std::string>();
            size_t pos = 0;
            while ((pos = value.find("${", pos)) != std::string::npos) {
                size_t end = value.find("}", pos);
                if (end == std::string::npos) break;
                
                std::string var_expr = value.substr(pos + 2, end - pos - 2);
                std::string var_name = var_expr;
                std::string default_value;
                
                // 处理默认值语法: ${VAR:-default}
                size_t sep = var_expr.find(":-");
                if (sep != std::string::npos) {
                    var_name = var_expr.substr(0, sep);
                    default_value = var_expr.substr(sep + 2);
                }
                
                const char* env_value = std::getenv(var_name.c_str());
                std::string replacement = env_value ? env_value : default_value;
                
                value.replace(pos, end - pos + 1, replacement);
                pos += replacement.length();
            }
            config = value;
        } else if (config.is_object()) {
            for (auto& [key, val] : config.items()) {
                expand_environment_variables(val);
            }
        } else if (config.is_array()) {
            for (auto& item : config) {
                expand_environment_variables(item);
            }
        }
    }
    
    void merge_configs(json& base, const json& override) {
        if (!override.is_object()) return;
        
        for (auto& [key, value] : override.items()) {
            if (base.contains(key) && base[key].is_object() && value.is_object()) {
                merge_configs(base[key], value);
            } else {
                base[key] = value;
            }
        }
    }
    
    ConfigLoadOptions options_;
    std::shared_ptr<ConfigValidator> validator_;
};

// ============================================================================
// 5. 配置备份管理器
// ============================================================================

class ConfigBackupManager {
public:
    explicit ConfigBackupManager(const std::string& config_dir, int max_backups = 10)
        : config_dir_(config_dir), max_backups_(max_backups) {}
    
    struct BackupInfo {
        std::string path;
        std::string timestamp;
        size_t size_bytes;
    };
    
    std::optional<std::string> create_backup(const std::string& config_path) {
        try {
            auto now = std::chrono::system_clock::now();
            auto timestamp = format_timestamp(now);
            
            std::string backup_path = config_path + ".backup." + timestamp;
            
            // 复制文件 (实现略)
            // std::filesystem::copy_file(config_path, backup_path);
            
            // 清理旧备份
            cleanup_old_backups(config_path);
            
            return backup_path;
            
        } catch (const std::exception& e) {
            return std::nullopt;
        }
    }
    
    std::vector<BackupInfo> list_backups(const std::string& config_path) {
        // 列出所有备份文件 (实现略)
        return {};
    }
    
    bool restore_backup(const std::string& backup_path, const std::string& target_path) {
        // 恢复备份 (实现略)
        return false;
    }

private:
    void cleanup_old_backups(const std::string& config_path) {
        auto backups = list_backups(config_path);
        if (backups.size() > static_cast<size_t>(max_backups_)) {
            // 删除最旧的备份 (实现略)
        }
    }
    
    std::string format_timestamp(std::chrono::system_clock::time_point tp) {
        auto t = std::chrono::system_clock::to_time_t(tp);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
        return buf;
    }
    
    std::string config_dir_;
    int max_backups_;
};

// ============================================================================
// 6. 配置热更新管理器
// ============================================================================

class ConfigHotReloadManager {
public:
    struct ReloadConfig {
        std::string config_path;
        std::chrono::milliseconds poll_interval{5000};  // 5秒轮询
        bool auto_reload = true;
        bool safe_rollback = true;                      // 验证失败时回滚
    };
    
    using ReloadCallback = std::function<void(const json& old_config, const json& new_config)>;
    
    explicit ConfigHotReloadManager(const ReloadConfig& config,
                                   std::shared_ptr<ConfigLoader> loader,
                                   std::shared_ptr<ConfigBackupManager> backup_mgr)
        : config_(config), loader_(loader), backup_mgr_(backup_mgr) {}
    
    struct ReloadResult {
        bool success = false;
        std::string error_message;
        json old_config;
        json new_config;
        bool rolled_back = false;
    };
    
    ReloadResult reload() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        try {
            // 1. 加载新配置
            auto load_result = loader_->load(config_.config_path);
            if (!load_result.success) {
                return ReloadResult{
                    false,
                    "Failed to load config: " + load_result.errors[0],
                    current_config_,
                    {},
                    false
                };
            }
            
            json new_config = load_result.config;
            
            // 2. 备份旧配置
            if (config_.safe_rollback) {
                auto backup_path = backup_mgr_->create_backup(config_.config_path);
                if (!backup_path) {
                    return ReloadResult{
                        false,
                        "Failed to create backup",
                        current_config_,
                        {},
                        false
                    };
                }
            }
            
            // 3. 通知回调 (可能失败)
            if (reload_callback_) {
                try {
                    reload_callback_(current_config_, new_config);
                } catch (const std::exception& e) {
                    // 验证失败,回滚
                    if (config_.safe_rollback) {
                        return ReloadResult{
                            false,
                            "Callback failed: " + std::string(e.what()),
                            current_config_,
                            new_config,
                            true
                        };
                    }
                    throw;
                }
            }
            
            // 4. 成功更新
            json old_config = current_config_;
            current_config_ = new_config;
            last_reload_time_ = std::chrono::steady_clock::now();
            
            return ReloadResult{
                true,
                "",
                old_config,
                new_config,
                false
            };
            
        } catch (const std::exception& e) {
            return ReloadResult{
                false,
                "Reload failed: " + std::string(e.what()),
                current_config_,
                {},
                false
            };
        }
    }
    
    void set_reload_callback(ReloadCallback callback) {
        reload_callback_ = std::move(callback);
    }
    
    json current_config() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_config_;
    }
    
    void start_auto_reload() {
        if (!config_.auto_reload) return;
        
        // 启动后台线程定期检查文件变化 (实现略)
        // 可使用 inotify (Linux) / FSEvents (macOS) / ReadDirectoryChangesW (Windows)
    }
    
    void stop_auto_reload() {
        // 停止后台线程 (实现略)
    }

private:
    ReloadConfig config_;
    std::shared_ptr<ConfigLoader> loader_;
    std::shared_ptr<ConfigBackupManager> backup_mgr_;
    
    mutable std::mutex mutex_;
    json current_config_;
    std::chrono::steady_clock::time_point last_reload_time_;
    ReloadCallback reload_callback_;
};

// ============================================================================
// 7. 完整配置管理器
// ============================================================================

class ConfigManager {
public:
    static ConfigManager& instance() {
        static ConfigManager manager;
        return manager;
    }
    
    struct Config {
        std::string config_path = "hunterclaw.json";
        ConfigLoadOptions load_options;
        ConfigHotReloadManager::ReloadConfig reload_config;
        std::string backup_dir = ".backups";
        int max_backups = 10;
    };
    
    void initialize(const Config& config) {
        loader_ = std::make_shared<ConfigLoader>(config.load_options);
        backup_mgr_ = std::make_shared<ConfigBackupManager>(
            config.backup_dir, config.max_backups);
        
        auto reload_config = config.reload_config;
        reload_config.config_path = config.config_path;
        
        hot_reload_mgr_ = std::make_unique<ConfigHotReloadManager>(
            reload_config, loader_, backup_mgr_);
        
        // 初始加载
        auto load_result = loader_->load(config.config_path);
        if (!load_result.success) {
            throw std::runtime_error("Failed to load config: " + load_result.errors[0]);
        }
        
        current_config_ = load_result.config;
        
        // 设置热更新回调
        hot_reload_mgr_->set_reload_callback(
            [this](const json& old_cfg, const json& new_cfg) {
                on_config_changed(old_cfg, new_cfg);
            });
        
        // 启动自动重载
        if (reload_config.auto_reload) {
            hot_reload_mgr_->start_auto_reload();
        }
    }
    
    json get_config() const {
        return current_config_;
    }
    
    template<typename T>
    T get(const std::string& json_path, const T& default_value = T{}) const {
        try {
            auto value = get_json_path(current_config_, json_path);
            if (value.is_null()) return default_value;
            return value.get<T>();
        } catch (const std::exception&) {
            return default_value;
        }
    }
    
    ConfigHotReloadManager::ReloadResult reload() {
        return hot_reload_mgr_->reload();
    }

private:
    ConfigManager() = default;
    
    void on_config_changed(const json& old_cfg, const json& new_cfg) {
        // 计算差异并触发相应的重载逻辑
        // 例如: 重启变更的 Channel, 更新速率限制配置等
    }
    
    json get_json_path(const json& obj, const std::string& path) const {
        std::vector<std::string> parts;
        size_t start = 0;
        size_t end = path.find('.');
        
        while (end != std::string::npos) {
            parts.push_back(path.substr(start, end - start));
            start = end + 1;
            end = path.find('.', start);
        }
        parts.push_back(path.substr(start));
        
        json current = obj;
        for (const auto& part : parts) {
            if (!current.contains(part)) {
                return json();
            }
            current = current[part];
        }
        return current;
    }
    
    std::shared_ptr<ConfigLoader> loader_;
    std::shared_ptr<ConfigBackupManager> backup_mgr_;
    std::unique_ptr<ConfigHotReloadManager> hot_reload_mgr_;
    json current_config_;
};

} // namespace config
} // namespace hunterclaw
