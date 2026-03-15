#pragma once

#include "hunterclaw/memory/types.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

// 文件监听抽象 (对齐 chokidar)

namespace hunterclaw {
namespace memory {

// File change event types
enum class FileChangeType {
    ADDED,
    MODIFIED,
    REMOVED
};

struct FileChangeEvent {
    std::string path;
    FileChangeType type;
};

using FileChangeCallback = std::function<void(const FileChangeEvent&)>;

// ============================================================================
// FileWatcher - Abstract file system watcher
// ============================================================================

class FileWatcher {
public:
    virtual ~FileWatcher() = default;

    // Start watching paths
    virtual Result<void> start(
        const std::vector<std::string>& paths,
        FileChangeCallback callback
    ) = 0;

    // Stop watching
    virtual Result<void> stop() = 0;

    // Check if watcher is active
    virtual bool is_active() const = 0;
};

// ============================================================================
// Platform-specific implementations
// ============================================================================

#ifdef __linux__
// Linux: inotify
class InotifyFileWatcher : public FileWatcher {
public:
    InotifyFileWatcher();
    ~InotifyFileWatcher() override;

    Result<void> start(
        const std::vector<std::string>& paths,
        FileChangeCallback callback
    ) override;

    Result<void> stop() override;
    bool is_active() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

#ifdef __APPLE__
// macOS: FSEvents
class FSEventsFileWatcher : public FileWatcher {
public:
    FSEventsFileWatcher();
    ~FSEventsFileWatcher() override;

    Result<void> start(
        const std::vector<std::string>& paths,
        FileChangeCallback callback
    ) override;

    Result<void> stop() override;
    bool is_active() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

#ifdef _WIN32
// Windows: FindFirstChangeNotification
class Win32FileWatcher : public FileWatcher {
public:
    Win32FileWatcher();
    ~Win32FileWatcher() override;

    Result<void> start(
        const std::vector<std::string>& paths,
        FileChangeCallback callback
    ) override;

    Result<void> stop() override;
    bool is_active() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

// ============================================================================
// Factory function
// ============================================================================

// Create platform-appropriate file watcher
std::unique_ptr<FileWatcher> create_file_watcher();

}  // namespace memory
}  // namespace hunterclaw
