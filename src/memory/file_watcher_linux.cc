#ifdef __linux__

#include "hunterclaw/memory/file_watcher.h"
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#include <map>
#include <thread>
#include <atomic>
#include <cstring>

// Linux inotify implementation (对齐 chokidar)

namespace hunterclaw {
namespace memory {

class InotifyFileWatcher::Impl {
public:
    Impl() : fd_(-1), active_(false) {}

    ~Impl() {
        stop();
    }

    Result<void> start(
        const std::vector<std::string>& paths,
        FileChangeCallback callback
    ) {
        if (active_) {
            return Result<void>::error("Watcher already active");
        }

        // Initialize inotify
        fd_ = inotify_init1(IN_NONBLOCK);
        if (fd_ == -1) {
            return Result<void>::error("Failed to init inotify: " + 
                std::string(strerror(errno)));
        }

        // Add watches
        uint32_t mask = IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM;
        for (const auto& path : paths) {
            int wd = inotify_add_watch(fd_, path.c_str(), mask);
            if (wd == -1) {
                // Log but continue
                continue;
            }
            watch_descriptors_[wd] = path;
        }

        if (watch_descriptors_.empty()) {
            close(fd_);
            fd_ = -1;
            return Result<void>::error("No valid paths to watch");
        }

        // Start watch thread
        callback_ = callback;
        active_ = true;
        watch_thread_ = std::thread([this]() { watch_loop(); });

        return Result<void>::ok();
    }

    Result<void> stop() {
        if (!active_) {
            return Result<void>::ok();
        }

        active_ = false;

        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }

        // Remove all watches
        for (const auto& pair : watch_descriptors_) {
            inotify_rm_watch(fd_, pair.first);
        }
        watch_descriptors_.clear();

        if (fd_ != -1) {
            close(fd_);
            fd_ = -1;
        }

        return Result<void>::ok();
    }

    bool is_active() const {
        return active_;
    }

private:
    void watch_loop() {
        constexpr size_t BUF_LEN = 4096;
        char buffer[BUF_LEN] __attribute__((aligned(__alignof__(struct inotify_event))));

        while (active_) {
            // Poll for events (with timeout)
            struct pollfd pfd;
            pfd.fd = fd_;
            pfd.events = POLLIN;

            int ret = poll(&pfd, 1, 1000);  // 1s timeout
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if (ret == 0) {
                continue;  // Timeout
            }

            // Read events
            ssize_t len = read(fd_, buffer, BUF_LEN);
            if (len == -1 && errno != EAGAIN) {
                break;
            }

            if (len <= 0) {
                continue;
            }

            // Process events
            const struct inotify_event* event;
            for (char* ptr = buffer; ptr < buffer + len;
                 ptr += sizeof(struct inotify_event) + event->len) {
                event = reinterpret_cast<const struct inotify_event*>(ptr);

                if (event->len == 0) {
                    continue;
                }

                auto wd_it = watch_descriptors_.find(event->wd);
                if (wd_it == watch_descriptors_.end()) {
                    continue;
                }

                std::string full_path = wd_it->second + "/" + event->name;

                FileChangeEvent change;
                change.path = full_path;

                if (event->mask & IN_CREATE) {
                    change.type = FileChangeType::ADDED;
                } else if (event->mask & (IN_MODIFY | IN_MOVED_TO)) {
                    change.type = FileChangeType::MODIFIED;
                } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    change.type = FileChangeType::REMOVED;
                } else {
                    continue;
                }

                if (callback_) {
                    callback_(change);
                }
            }
        }
    }

    int fd_;
    std::atomic<bool> active_;
    std::map<int, std::string> watch_descriptors_;
    std::thread watch_thread_;
    FileChangeCallback callback_;
};

InotifyFileWatcher::InotifyFileWatcher() 
    : impl_(std::make_unique<Impl>()) {}

InotifyFileWatcher::~InotifyFileWatcher() = default;

Result<void> InotifyFileWatcher::start(
    const std::vector<std::string>& paths,
    FileChangeCallback callback
) {
    return impl_->start(paths, callback);
}

Result<void> InotifyFileWatcher::stop() {
    return impl_->stop();
}

bool InotifyFileWatcher::is_active() const {
    return impl_->is_active();
}

// Factory function
std::unique_ptr<FileWatcher> create_file_watcher() {
    return std::make_unique<InotifyFileWatcher>();
}

}  // namespace memory
}  // namespace hunterclaw

#endif  // __linux__
