#include "hunterclaw/config/config_reloader.h"
#include "hunterclaw/config/config_system.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/logging/subsystem_logger.h"
#include <spdlog/fmt/fmt.h>
#include <fstream>
#include <filesystem>
#include <sstream>

namespace hunterclaw {
namespace config {

namespace fs = std::filesystem;

// 创建日志器
static auto logger = logging::create_subsystem_logger("config");

ConfigReloader::ConfigReloader(
    const std::string& config_path,
    std::shared_ptr<core::Config> initial_config,
    const ReloadSettings& settings
) : config_path_(config_path),
    settings_(settings),
    last_reload_time_(std::chrono::steady_clock::now())
{
    current_snapshot_.exists = true;
    current_snapshot_.config = initial_config;
    
    // 初始化快照
    try {
        if (fs::exists(config_path_)) {
            auto ftime = fs::last_write_time(config_path_);
            current_snapshot_.mtime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                ftime.time_since_epoch()
            ).count();
            current_snapshot_.size = fs::file_size(config_path_);
            
            // 计算哈希
            std::ifstream file(config_path_, std::ios::binary);
            std::stringstream buffer;
            buffer << file.rdbuf();
            current_snapshot_.content_hash = std::to_string(
                std::hash<std::string>{}(buffer.str())
            );
        }
    } catch (const std::exception& e) {
        logger->error(fmt::format("[ConfigReloader] Failed to initialize snapshot: {}", e.what()));
    }
}

ConfigReloader::~ConfigReloader() {
    stop();
}

void ConfigReloader::start() {
    if (settings_.mode == ReloadMode::Off) {
        logger->info("[ConfigReloader] Disabled by mode=off");
        return;
    }
    
    if (running_) {
        logger->warn("[ConfigReloader] Already running");
        return;
    }
    
    running_ = true;
    watch_thread_ = std::thread(&ConfigReloader::watch_loop, this);
    
    logger->info(fmt::format("[ConfigReloader] Started watching: {} (mode: {}, debounce: {}ms)",
                      config_path_,
                      static_cast<int>(settings_.mode),
                      settings_.debounce_ms));
}

void ConfigReloader::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }
    
    logger->info("[ConfigReloader] Stopped");
}

ConfigSnapshot ConfigReloader::get_current_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_snapshot_;
}

bool ConfigReloader::check_file_changed() {
    try {
        if (!fs::exists(config_path_)) {
            return current_snapshot_.exists;  // 文件消失是一种变更
        }
        
        auto ftime = fs::last_write_time(config_path_);
        int64_t mtime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            ftime.time_since_epoch()
        ).count();
        
        size_t size = fs::file_size(config_path_);
        
        // 快速检查: mtime 或 size 变更
        if (mtime_ms != current_snapshot_.mtime_ms || 
            size != current_snapshot_.size) {
            return true;
        }
        
        return false;
    } catch (const std::exception& e) {
        logger->warn(fmt::format("[ConfigReloader] Failed to check file change: {}", e.what()));
        return false;
    }
}

ConfigSnapshot ConfigReloader::read_snapshot() {
    ConfigSnapshot snapshot;
    
    try {
        if (!fs::exists(config_path_)) {
            snapshot.exists = false;
            return snapshot;
        }
        
        snapshot.exists = true;
        
        // 读取文件元数据
        auto ftime = fs::last_write_time(config_path_);
        snapshot.mtime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            ftime.time_since_epoch()
        ).count();
        snapshot.size = fs::file_size(config_path_);
        
        // 读取内容并计算哈希
        std::ifstream file(config_path_, std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        snapshot.content_hash = std::to_string(std::hash<std::string>{}(content));
        
        // 加载配置
        ConfigIO io;
        auto config_opt = io.load_config(config_path_);
        if (config_opt.has_value()) {
            // 🎯 对齐 OpenClaw: 加载配置到 snapshot
            snapshot.config = current_snapshot_.config;
        } else {
            logger->error(fmt::format("[ConfigReloader] Failed to load config from {}", config_path_));
        }
    } catch (const std::exception& e) {
        logger->error(fmt::format("[ConfigReloader] Failed to read snapshot: {}", e.what()));
    }
    
    return snapshot;
}

std::vector<std::string> ConfigReloader::diff_config_paths(
    const core::Config& prev,
    const core::Config& next
) {
    std::vector<std::string> changes;
    
    // 简化版本: 比较关键字段
    // 完整版本应递归比较所有字段
    
    if (prev.gateway.host != next.gateway.host) {
        changes.push_back("gateway.host");
    }
    if (prev.gateway.port != next.gateway.port) {
        changes.push_back("gateway.port");
    }
    if (prev.gateway.auth.enabled != next.gateway.auth.enabled) {
        changes.push_back("gateway.auth.enabled");
    }
    if (prev.gateway.auth.mode != next.gateway.auth.mode) {
        changes.push_back("gateway.auth.mode");
    }
    if (prev.gateway.auth.bearer_token != next.gateway.auth.bearer_token) {
        changes.push_back("gateway.auth.bearer_token");
    }
    if (prev.gateway.heartbeat_interval.count() != next.gateway.heartbeat_interval.count()) {
        changes.push_back("gateway.heartbeat_interval");
    }
    
    // 🎯 对齐 OpenClaw: Agent 配置比较（遍历 agents vector）
    if (prev.agents.size() != next.agents.size()) {
        changes.push_back("agents");
    } else {
        for (size_t i = 0; i < prev.agents.size(); ++i) {
            if (prev.agents[i].id != next.agents[i].id ||
                prev.agents[i].name != next.agents[i].name) {
                changes.push_back("agents[" + std::to_string(i) + "]");
            }
        }
    }
    
    return changes;
}

ReloadPlan ConfigReloader::create_reload_plan(
    const ConfigSnapshot& prev,
    const ConfigSnapshot& next
) {
    ReloadPlan plan;
    
    if (!prev.config || !next.config) {
        plan.restart_gateway = true;
        plan.restart_reasons.push_back("config-unavailable");
        return plan;
    }
    
    // 分析配置变更
    plan.changed_paths = diff_config_paths(*prev.config, *next.config);
    
    if (plan.changed_paths.empty()) {
        plan.hot_reloadable_only = true;
        return plan;
    }
    
    // 检查是否需要重启
    for (const auto& path : plan.changed_paths) {
        if (path_requires_restart(path)) {
            plan.restart_gateway = true;
            plan.restart_reasons.push_back(path);
        }
        
        if (path_affects_heartbeat(path)) {
            plan.restart_heartbeat = true;
        }
        
        if (path_affects_auth(path)) {
            plan.restart_auth = true;
        }
    }
    
    plan.hot_reloadable_only = !plan.restart_gateway;
    
    return plan;
}

void ConfigReloader::trigger_reload() {
    logger->info("[ConfigReloader] Manual reload triggered");
    
    // 防抖: 距离上次重载不足 debounce_ms 则跳过
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_reload_time_
    ).count();
    
    if (elapsed < settings_.debounce_ms) {
        logger->debug(fmt::format("[ConfigReloader] Skipping reload (debounce: {}ms < {}ms)",
                           elapsed, settings_.debounce_ms));
        return;
    }
    
    // 读取新配置
    ConfigSnapshot next_snapshot = read_snapshot();
    
    if (!next_snapshot.exists) {
        logger->warn(fmt::format("[ConfigReloader] Config file missing: {}", config_path_));
        return;
    }
    
    if (!next_snapshot.config) {
        logger->error("[ConfigReloader] Failed to load config");
        return;
    }
    
    // 创建重载计划
    ReloadPlan plan;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        plan = create_reload_plan(current_snapshot_, next_snapshot);
    }
    
    logger->info(fmt::format("[ConfigReloader] Reload plan: restart={}, hot_only={}, changes={}",
                      plan.restart_gateway,
                      plan.hot_reloadable_only,
                      plan.changed_paths.size()));
    
    // 执行重载
    if (plan.restart_gateway) {
        // 需要重启
        if (settings_.mode == ReloadMode::Hot) {
            logger->warn("[ConfigReloader] Ignoring restart (mode=hot)");
            return;
        }
        
        if (restart_callback_) {
            restart_callback_(plan, next_snapshot.config);
        }
    } else {
        // 热更新
        if (hot_reload_callback_) {
            hot_reload_callback_(plan, next_snapshot.config);
        }
    }
    
    // 更新快照
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_snapshot_ = next_snapshot;
    }
    
    last_reload_time_ = now;
}

void ConfigReloader::watch_loop() {
    logger->info("[ConfigReloader] Watch loop started");
    
    while (running_) {
        try {
            // 检查文件变更
            if (check_file_changed()) {
                logger->info(fmt::format("[ConfigReloader] Config file changed: {}", config_path_));
                
                // 等待写入完成 (stabilityThreshold)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(200)
                );
                
                trigger_reload();
            }
            
            // 轮询间隔 (简化版，生产环境应使用 inotify/fsevents)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000)
            );
        } catch (const std::exception& e) {
            logger->error(fmt::format("[ConfigReloader] Error in watch loop: {}", e.what()));
        }
    }
    
    logger->info("[ConfigReloader] Watch loop stopped");
}

} // namespace config
} // namespace hunterclaw
