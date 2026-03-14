#include "hunterclaw/skills/file_watcher.h"

#ifdef __linux__

#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <regex>
#include <algorithm>
#include <spdlog/spdlog.h>

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
    
    // Glob 模式匹配简化实现
    bool glob_match(const std::string& pattern, const std::string& path) {
        // 将 glob 转换为正则表达式
        std::string regex_pattern = pattern;
        
        // 转义特殊字符
        std::vector<std::pair<std::string, std::string>> replacements = {
            {"\\", "\\\\"},
            {".", "\\."},
            {"*", ".*"},
            {"?", "."}
        };
        
        for (const auto& [from, to] : replacements) {
            size_t pos = 0;
            while ((pos = regex_pattern.find(from, pos)) != std::string::npos) {
                regex_pattern.replace(pos, from.length(), to);
                pos += to.length();
            }
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
            } catch (...) {
                // 忽略无效的正则表达式
            }
        }
        
        return false;
    }
}

// ============================================================================
// InotifyWatcher Implementation
// ============================================================================

struct InotifyWatcher::Impl {
    FileWatcherConfig config;
    EventCallback callback;
    
    int inotify_fd = -1;
    std::atomic<bool> running{false};
    std::thread watch_thread;
    
    std::mutex mutex;
    std::map<int, fs::path> watch_descriptors;  // wd -> path
    std::vector<std::string> watch_patterns;
    
    // 防抖机制
    struct PendingEvent {
        WatchEvent event;
        std::chrono::system_clock::time_point scheduled_time;
    };
    std::map<std::string, PendingEvent> pending_events;
    std::thread debounce_thread;
    std::atomic<bool> debounce_running{false};
    
    explicit Impl(const FileWatcherConfig& cfg) : config(cfg) {}
    
    ~Impl() {
        stop();
    }
    
    core::Result<bool> add_watch(const std::string& pattern) {
        std::lock_guard<std::mutex> lock(mutex);
        
        // 保存模式
        if (std::find(watch_patterns.begin(), watch_patterns.end(), pattern) 
            != watch_patterns.end()) {
            return core::Result<bool>::Ok(true);  // 已存在
        }
        
        watch_patterns.push_back(pattern);
        
        // 如果正在运行，立即添加监控
        if (running) {
            return setup_watches();
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> remove_watch(const std::string& pattern) {
        std::lock_guard<std::mutex> lock(mutex);
        
        auto it = std::find(watch_patterns.begin(), watch_patterns.end(), pattern);
        if (it != watch_patterns.end()) {
            watch_patterns.erase(it);
        }
        
        // 重新设置所有监控
        if (running) {
            return setup_watches();
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> setup_watches() {
        // 清除旧的监控
        for (const auto& [wd, path] : watch_descriptors) {
            inotify_rm_watch(inotify_fd, wd);
        }
        watch_descriptors.clear();
        
        // 解析 glob 模式并添加监控
        for (const auto& pattern : watch_patterns) {
            auto result = add_watch_from_pattern(pattern);
            if (result.is_err()) {
                log()->warn("Failed to add watch for pattern {}: {}", 
                           pattern, result.error);
            }
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> add_watch_from_pattern(const std::string& pattern) {
        // 简化实现: 监控父目录
        fs::path pattern_path(pattern);
        
        // 如果包含通配符，监控父目录
        std::string pattern_str = pattern_path.string();
        size_t star_pos = pattern_str.find('*');
        
        fs::path watch_path;
        if (star_pos != std::string::npos) {
            // 提取父目录
            fs::path parent = pattern_path.parent_path();
            while (parent.string().find('*') != std::string::npos) {
                parent = parent.parent_path();
            }
            watch_path = parent;
        } else {
            watch_path = pattern_path.parent_path();
        }
        
        if (!fs::exists(watch_path)) {
            return core::Result<bool>::Err("Path does not exist: " + watch_path.string());
        }
        
        // 添加 inotify 监控
        uint32_t mask = IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM;
        
        int wd = inotify_add_watch(inotify_fd, watch_path.string().c_str(), mask);
        if (wd < 0) {
            return core::Result<bool>::Err("Failed to add inotify watch");
        }
        
        watch_descriptors[wd] = watch_path;
        log()->debug("Added watch for: {}", watch_path.string());
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> start() {
        if (running) {
            return core::Result<bool>::Err("Already running");
        }
        
        // 初始化 inotify
        inotify_fd = inotify_init1(IN_NONBLOCK);
        if (inotify_fd < 0) {
            return core::Result<bool>::Err("Failed to initialize inotify");
        }
        
        // 设置监控
        auto result = setup_watches();
        if (result.is_err()) {
            close(inotify_fd);
            inotify_fd = -1;
            return result;
        }
        
        // 启动监控线程
        running = true;
        watch_thread = std::thread([this]() { watch_loop(); });
        
        // 启动防抖线程
        debounce_running = true;
        debounce_thread = std::thread([this]() { debounce_loop(); });
        
        log()->info("File watcher started (inotify)");
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> stop() {
        if (!running) {
            return core::Result<bool>::Ok(true);
        }
        
        running = false;
        
        if (watch_thread.joinable()) {
            watch_thread.join();
        }
        
        debounce_running = false;
        if (debounce_thread.joinable()) {
            debounce_thread.join();
        }
        
        // 清理 inotify
        if (inotify_fd >= 0) {
            for (const auto& [wd, path] : watch_descriptors) {
                inotify_rm_watch(inotify_fd, wd);
            }
            close(inotify_fd);
            inotify_fd = -1;
        }
        
        watch_descriptors.clear();
        pending_events.clear();
        
        log()->info("File watcher stopped");
        
        return core::Result<bool>::Ok(true);
    }
    
    void watch_loop() {
        constexpr size_t EVENT_BUF_LEN = 4096;
        char buffer[EVENT_BUF_LEN] __attribute__((aligned(8)));
        
        while (running) {
            struct pollfd pfd = { inotify_fd, POLLIN, 0 };
            int ret = poll(&pfd, 1, 100);  // 100ms timeout
            
            if (ret < 0) {
                if (errno == EINTR) continue;
                log()->error("poll() failed: {}", strerror(errno));
                break;
            }
            
            if (ret == 0) continue;  // Timeout
            
            ssize_t len = read(inotify_fd, buffer, EVENT_BUF_LEN);
            if (len < 0) {
                if (errno == EAGAIN) continue;
                log()->error("read() failed: {}", strerror(errno));
                break;
            }
            
            // 处理事件
            for (char* ptr = buffer; ptr < buffer + len; ) {
                struct inotify_event* event = reinterpret_cast<struct inotify_event*>(ptr);
                
                if (event->len > 0) {
                    handle_inotify_event(event);
                }
                
                ptr += sizeof(struct inotify_event) + event->len;
            }
        }
    }
    
    void handle_inotify_event(const struct inotify_event* event) {
        std::lock_guard<std::mutex> lock(mutex);
        
        // 查找监控路径
        auto it = watch_descriptors.find(event->wd);
        if (it == watch_descriptors.end()) {
            return;
        }
        
        fs::path base_path = it->second;
        fs::path full_path = base_path / event->name;
        
        // 检查是否匹配监控模式
        bool matches = false;
        for (const auto& pattern : watch_patterns) {
            if (glob_match(pattern, full_path.string())) {
                matches = true;
                break;
            }
        }
        
        if (!matches) {
            return;
        }
        
        // 检查是否应该忽略
        if (should_ignore(full_path, config.ignored_patterns)) {
            return;
        }
        
        // 确定事件类型
        WatchEventType type;
        if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
            type = WatchEventType::Add;
        } else if (event->mask & IN_MODIFY) {
            type = WatchEventType::Change;
        } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
            type = WatchEventType::Unlink;
        } else {
            return;
        }
        
        // 调度事件 (带防抖)
        schedule_event(WatchEvent{type, full_path, std::chrono::system_clock::now()});
    }
    
    void schedule_event(const WatchEvent& event) {
        std::string key = event.path.string();
        auto now = std::chrono::system_clock::now();
        auto scheduled_time = now + config.debounce_delay;
        
        pending_events[key] = PendingEvent{event, scheduled_time};
    }
    
    void debounce_loop() {
        while (debounce_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            auto now = std::chrono::system_clock::now();
            
            std::vector<WatchEvent> ready_events;
            
            {
                std::lock_guard<std::mutex> lock(mutex);
                
                auto it = pending_events.begin();
                while (it != pending_events.end()) {
                    if (it->second.scheduled_time <= now) {
                        ready_events.push_back(it->second.event);
                        it = pending_events.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            
            // 触发回调
            for (const auto& event : ready_events) {
                if (callback) {
                    callback(event);
                }
            }
        }
    }
};

InotifyWatcher::InotifyWatcher(const FileWatcherConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

InotifyWatcher::~InotifyWatcher() = default;

core::Result<bool> InotifyWatcher::add_watch(const std::string& pattern) {
    return impl_->add_watch(pattern);
}

core::Result<bool> InotifyWatcher::remove_watch(const std::string& pattern) {
    return impl_->remove_watch(pattern);
}

core::Result<bool> InotifyWatcher::start() {
    return impl_->start();
}

core::Result<bool> InotifyWatcher::stop() {
    return impl_->stop();
}

bool InotifyWatcher::is_running() const {
    return impl_->running;
}

void InotifyWatcher::set_callback(EventCallback callback) {
    impl_->callback = std::move(callback);
}

} // namespace skills
} // namespace hunterclaw

#endif // __linux__
