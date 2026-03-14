#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <thread>
#include <mutex>
#include <chrono>

namespace hunterclaw {
namespace plugins {

// ============================================================================
// Plugin Hot Reload - File Watcher + Diff Plan
// ============================================================================

enum class ReloadAction {
    HotReload,      // Can be reloaded without restart
    RestartRequired // Needs full restart
};

struct ReloadPlan {
    ReloadAction action;
    std::vector<std::string> changed_paths;
    std::string reason;
};

// File watcher (chokidar-like)
class FileWatcher {
public:
    struct WatchOptions {
        bool ignore_initial = true;
        int stability_threshold_ms = 200;  // Wait for file to stabilize
        int poll_interval_ms = 50;
        std::vector<std::string> ignored_paths;
    };
    
    FileWatcher(const WatchOptions& options = WatchOptions());
    ~FileWatcher();
    
    // Start watching a path
    void watch(const std::string& path, std::function<void(const std::string&)> on_change);
    
    // Stop watching
    void unwatch(const std::string& path);
    
    // Stop all watches
    void stop();
    
private:
    WatchOptions options_;
    std::map<std::string, int64_t> file_mtimes_;
    std::map<std::string, std::function<void(const std::string&)>> callbacks_;
    std::unique_ptr<std::thread> poll_thread_;
    bool running_;
    std::mutex mutex_;
    
    void poll_loop();
    int64_t get_file_mtime(const std::string& path);
};

// Reload plan builder
class ReloadPlanBuilder {
public:
    // Build reload plan based on changed paths
    static ReloadPlan build_plan(const std::vector<std::string>& changed_paths);
    
    // Check if path requires restart
    static bool requires_restart(const std::string& path);
    
    // Check if path supports hot reload
    static bool supports_hot_reload(const std::string& path);
    
private:
    // Paths that require restart
    static const std::vector<std::string> restart_paths_;
    
    // Paths that support hot reload
    static const std::vector<std::string> hot_reload_paths_;
};

// Config snapshot (for error recovery)
struct ConfigSnapshot {
    bool exists;
    bool valid;
    std::string content;
    std::vector<std::string> issues;
    int64_t timestamp_ms;
};

// Hot reload manager
class HotReloadManager {
public:
    struct Settings {
        int debounce_ms = 300;  // Merge changes within 300ms
        int max_retries = 3;
        int retry_delay_ms = 1000;
        bool enabled = true;
    };
    
    struct Callbacks {
        std::function<void(const ReloadPlan&, const std::string&)> on_hot_reload;
        std::function<void(const ReloadPlan&, const std::string&)> on_restart;
        std::function<ConfigSnapshot(const std::string&)> validate_config;
    };
    
    HotReloadManager(const Settings& settings, const Callbacks& callbacks);
    ~HotReloadManager();
    
    // Start watching config file
    void watch_config(const std::string& config_path);
    
    // Stop watching
    void stop();
    
    // Trigger manual reload
    core::Result<bool> reload_now(const std::string& config_path);
    
private:
    Settings settings_;
    Callbacks callbacks_;
    std::unique_ptr<FileWatcher> watcher_;
    
    std::string current_config_content_;
    int missing_config_retries_;
    
    std::mutex debounce_mutex_;
    std::chrono::steady_clock::time_point last_change_time_;
    std::unique_ptr<std::thread> debounce_thread_;
    bool debounce_scheduled_;
    
    void on_config_change(const std::string& path);
    void schedule_reload_after(int delay_ms, const std::string& path);
    void execute_reload(const std::string& path);
};

}  // namespace plugins
}  // namespace hunterclaw
