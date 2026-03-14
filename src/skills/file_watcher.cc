#include "hunterclaw/skills/file_watcher.h"
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <regex>
#include <algorithm>

namespace hunterclaw {
namespace skills {

namespace {
    auto log() {
        auto logger = spdlog::get("file_watcher");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("file_watcher");
        }
        return logger;
    }
    
    // Glob 模式匹配
    bool glob_match(const std::string& pattern, const std::string& path) {
        std::string regex_pattern = pattern;
        
        // 简单的 glob 转正则表达式
        size_t pos = 0;
        while ((pos = regex_pattern.find("*", pos)) != std::string::npos) {
            regex_pattern.replace(pos, 1, ".*");
            pos += 2;
        }
        
        try {
            std::regex re(regex_pattern);
            return std::regex_match(path, re);
        } catch (...) {
            return false;
        }
    }
    
    // 检查路径是否应该被忽略
    bool should_ignore(const fs::path& path, const std::vector<std::string>& patterns) {
        std::string path_str = path.string();
        
        for (const auto& pattern : patterns) {
            try {
                std::regex re(pattern);
                if (std::regex_search(path_str, re)) {
                    return true;
                }
            } catch (...) {}
        }
        
        return false;
    }
    
    // 获取文件修改时间
    std::optional<fs::file_time_type> get_mtime(const fs::path& path) {
        try {
            if (fs::exists(path) && fs::is_regular_file(path)) {
                return fs::last_write_time(path);
            }
        } catch (...) {}
        return std::nullopt;
    }
    
    // 递归查找匹配文件
    std::vector<fs::path> find_matching_files(
        const fs::path& base_path,
        const std::string& pattern,
        const std::vector<std::string>& ignored_patterns
    ) {
        std::vector<fs::path> results;
        
        if (!fs::exists(base_path)) {
            return results;
        }
        
        try {
            for (const auto& entry : fs::recursive_directory_iterator(
                base_path,
                fs::directory_options::skip_permission_denied
            )) {
                if (!entry.is_regular_file()) continue;
                
                const auto& path = entry.path();
                
                // 检查是否应该忽略
                if (should_ignore(path, ignored_patterns)) {
                    continue;
                }
                
                // 检查是否匹配模式
                if (glob_match(pattern, path.string())) {
                    results.push_back(path);
                }
            }
        } catch (const std::exception& e) {
            log()->warn("Error scanning {}: {}", base_path.string(), e.what());
        }
        
        return results;
    }
}

// ============================================================================
// PollingWatcher Implementation (跨平台降级方案)
// ============================================================================

struct PollingWatcher::Impl {
    FileWatcherConfig config;
    EventCallback callback;
    
    std::atomic<bool> running{false};
    std::thread poll_thread;
    
    std::mutex mutex;
    std::vector<std::string> watch_patterns;
    std::map<std::string, fs::file_time_type> file_mtimes;  // path -> mtime
    
    explicit Impl(const FileWatcherConfig& cfg) : config(cfg) {}
    
    ~Impl() {
        stop();
    }
    
    core::Result<bool> add_watch(const std::string& pattern) {
        std::lock_guard<std::mutex> lock(mutex);
        
        if (std::find(watch_patterns.begin(), watch_patterns.end(), pattern) 
            != watch_patterns.end()) {
            return core::Result<bool>::Ok(true);
        }
        
        watch_patterns.push_back(pattern);
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> remove_watch(const std::string& pattern) {
        std::lock_guard<std::mutex> lock(mutex);
        
        auto it = std::find(watch_patterns.begin(), watch_patterns.end(), pattern);
        if (it != watch_patterns.end()) {
            watch_patterns.erase(it);
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> start() {
        if (running) {
            return core::Result<bool>::Err("Already running");
        }
        
        // 初始化文件状态
        scan_and_initialize();
        
        running = true;
        poll_thread = std::thread([this]() { poll_loop(); });
        
        log()->info("File watcher started (polling mode)");
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> stop() {
        if (!running) {
            return core::Result<bool>::Ok(true);
        }
        
        running = false;
        
        if (poll_thread.joinable()) {
            poll_thread.join();
        }
        
        file_mtimes.clear();
        
        log()->info("File watcher stopped");
        
        return core::Result<bool>::Ok(true);
    }
    
    void scan_and_initialize() {
        std::lock_guard<std::mutex> lock(mutex);
        
        for (const auto& pattern : watch_patterns) {
            // 解析模式获取基础路径
            fs::path pattern_path(pattern);
            std::string pattern_str = pattern_path.string();
            
            size_t star_pos = pattern_str.find('*');
            if (star_pos == std::string::npos) {
                // 精确路径
                auto mtime = get_mtime(pattern_path);
                if (mtime) {
                    file_mtimes[pattern_str] = *mtime;
                }
            } else {
                // Glob 模式
                fs::path base_path = pattern_path.parent_path();
                while (base_path.string().find('*') != std::string::npos) {
                    base_path = base_path.parent_path();
                }
                
                auto files = find_matching_files(base_path, pattern, config.ignored_patterns);
                for (const auto& file : files) {
                    auto mtime = get_mtime(file);
                    if (mtime) {
                        file_mtimes[file.string()] = *mtime;
                    }
                }
            }
        }
        
        log()->debug("Initialized tracking for {} files", file_mtimes.size());
    }
    
    void poll_loop() {
        while (running) {
            std::this_thread::sleep_for(config.poll_interval);
            
            check_changes();
        }
    }
    
    void check_changes() {
        std::vector<WatchEvent> events;
        
        {
            std::lock_guard<std::mutex> lock(mutex);
            
            // 临时存储当前扫描结果
            std::map<std::string, fs::file_time_type> current_files;
            
            // 扫描所有监控模式
            for (const auto& pattern : watch_patterns) {
                fs::path pattern_path(pattern);
                std::string pattern_str = pattern_path.string();
                
                size_t star_pos = pattern_str.find('*');
                if (star_pos == std::string::npos) {
                    // 精确路径
                    auto mtime = get_mtime(pattern_path);
                    if (mtime) {
                        current_files[pattern_str] = *mtime;
                    }
                } else {
                    // Glob 模式
                    fs::path base_path = pattern_path.parent_path();
                    while (base_path.string().find('*') != std::string::npos) {
                        base_path = base_path.parent_path();
                    }
                    
                    auto files = find_matching_files(base_path, pattern, config.ignored_patterns);
                    for (const auto& file : files) {
                        auto mtime = get_mtime(file);
                        if (mtime) {
                            current_files[file.string()] = *mtime;
                        }
                    }
                }
            }
            
            // 检测变更
            // 1. 新增或修改的文件
            for (const auto& [path, mtime] : current_files) {
                auto it = file_mtimes.find(path);
                
                if (it == file_mtimes.end()) {
                    // 新增文件
                    if (!config.ignore_initial || !file_mtimes.empty()) {
                        events.push_back(WatchEvent{
                            WatchEventType::Add,
                            fs::path(path),
                            std::chrono::system_clock::now()
                        });
                    }
                } else if (it->second != mtime) {
                    // 修改文件
                    events.push_back(WatchEvent{
                        WatchEventType::Change,
                        fs::path(path),
                        std::chrono::system_clock::now()
                    });
                }
            }
            
            // 2. 删除的文件
            for (const auto& [path, mtime] : file_mtimes) {
                if (current_files.find(path) == current_files.end()) {
                    events.push_back(WatchEvent{
                        WatchEventType::Unlink,
                        fs::path(path),
                        std::chrono::system_clock::now()
                    });
                }
            }
            
            // 更新状态
            file_mtimes = std::move(current_files);
        }
        
        // 触发回调
        for (const auto& event : events) {
            if (callback) {
                callback(event);
            }
        }
    }
};

PollingWatcher::PollingWatcher(const FileWatcherConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

PollingWatcher::~PollingWatcher() = default;

core::Result<bool> PollingWatcher::add_watch(const std::string& pattern) {
    return impl_->add_watch(pattern);
}

core::Result<bool> PollingWatcher::remove_watch(const std::string& pattern) {
    return impl_->remove_watch(pattern);
}

core::Result<bool> PollingWatcher::start() {
    return impl_->start();
}

core::Result<bool> PollingWatcher::stop() {
    return impl_->stop();
}

bool PollingWatcher::is_running() const {
    return impl_->running;
}

void PollingWatcher::set_callback(EventCallback callback) {
    impl_->callback = std::move(callback);
}

// ============================================================================
// Factory Method
// ============================================================================

std::unique_ptr<FileWatcher> FileWatcher::create(const FileWatcherConfig& config) {
#ifdef __linux__
    log()->info("Creating file watcher (inotify)");
    return std::make_unique<InotifyWatcher>(config);
#elif defined(__APPLE__)
    log()->info("Creating file watcher (FSEvents)");
    return std::make_unique<FSEventsWatcher>(config);
#else
    log()->info("Creating file watcher (polling fallback)");
    return std::make_unique<PollingWatcher>(config);
#endif
}

} // namespace skills
} // namespace hunterclaw
