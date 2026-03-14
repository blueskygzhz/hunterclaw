#pragma once

#include "hunterclaw/core/types.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

namespace hunterclaw {
namespace skills {

namespace fs = std::filesystem;

// ============================================================================
// File Watcher Event Types
// ============================================================================

enum class WatchEventType {
    Add,      // 新增文件
    Change,   // 修改文件
    Unlink    // 删除文件
};

struct WatchEvent {
    WatchEventType type;
    fs::path path;
    std::chrono::system_clock::time_point timestamp;
};

// ============================================================================
// File Watcher Configuration
// ============================================================================

struct FileWatcherConfig {
    // 防抖延迟 (毫秒)
    std::chrono::milliseconds debounce_delay{250};
    
    // 文件稳定性阈值 (等待文件写入完成)
    std::chrono::milliseconds stability_threshold{250};
    
    // 轮询间隔 (仅用于降级模式)
    std::chrono::milliseconds poll_interval{100};
    
    // 忽略的路径模式 (正则表达式)
    std::vector<std::string> ignored_patterns = {
        R"((^|[\\/])\.git([\\/]|$))",
        R"((^|[\\/])node_modules([\\/]|$))",
        R"((^|[\\/])dist([\\/]|$))",
        R"((^|[\\/])build([\\/]|$))",
        R"((^|[\\/])\.venv([\\/]|$))",
        R"((^|[\\/])__pycache__([\\/]|$))",
        R"((^|[\\/])\.pytest_cache([\\/]|$))",
        R"((^|[\\/])\.mypy_cache([\\/]|$))",
        R"((^|[\\/])\.tox([\\/]|$))",
        R"((^|[\\/])\.cache([\\/]|$))",
        R"((^|[\\/])\.DS_Store$)",
        R"((^|[\\/])Thumbs\.db$)",
        R"((^|[\\/])\.tmp([\\/]|$))",
        R"((^|[\\/])\.temp([\\/]|$))",
    };
    
    // 只监控初始文件 (不监控后续添加的文件)
    bool ignore_initial = true;
};

// ============================================================================
// File Watcher Interface
// ============================================================================

class FileWatcher {
public:
    using EventCallback = std::function<void(const WatchEvent&)>;
    
    virtual ~FileWatcher() = default;
    
    // 添加监控目标 (支持 glob 模式)
    // 例如: "/path/to/skills/*/SKILL.md"
    virtual core::Result<bool> add_watch(const std::string& pattern) = 0;
    
    // 移除监控目标
    virtual core::Result<bool> remove_watch(const std::string& pattern) = 0;
    
    // 启动监控
    virtual core::Result<bool> start() = 0;
    
    // 停止监控
    virtual core::Result<bool> stop() = 0;
    
    // 检查是否正在运行
    virtual bool is_running() const = 0;
    
    // 设置事件回调
    virtual void set_callback(EventCallback callback) = 0;
    
    // 创建平台相关的实现
    static std::unique_ptr<FileWatcher> create(const FileWatcherConfig& config = {});
};

// ============================================================================
// Platform-specific Implementations
// ============================================================================

#ifdef __linux__
// Linux: inotify-based watcher
class InotifyWatcher : public FileWatcher {
public:
    explicit InotifyWatcher(const FileWatcherConfig& config);
    ~InotifyWatcher() override;
    
    core::Result<bool> add_watch(const std::string& pattern) override;
    core::Result<bool> remove_watch(const std::string& pattern) override;
    core::Result<bool> start() override;
    core::Result<bool> stop() override;
    bool is_running() const override;
    void set_callback(EventCallback callback) override;
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

#ifdef __APPLE__
// macOS: FSEvents-based watcher
class FSEventsWatcher : public FileWatcher {
public:
    explicit FSEventsWatcher(const FileWatcherConfig& config);
    ~FSEventsWatcher() override;
    
    core::Result<bool> add_watch(const std::string& pattern) override;
    core::Result<bool> remove_watch(const std::string& pattern) override;
    core::Result<bool> start() override;
    core::Result<bool> stop() override;
    bool is_running() const override;
    void set_callback(EventCallback callback) override;
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

// Fallback: Polling-based watcher (works on all platforms)
class PollingWatcher : public FileWatcher {
public:
    explicit PollingWatcher(const FileWatcherConfig& config);
    ~PollingWatcher() override;
    
    core::Result<bool> add_watch(const std::string& pattern) override;
    core::Result<bool> remove_watch(const std::string& pattern) override;
    core::Result<bool> start() override;
    core::Result<bool> stop() override;
    bool is_running() const override;
    void set_callback(EventCallback callback) override;
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace skills
} // namespace hunterclaw
