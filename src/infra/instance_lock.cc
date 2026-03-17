#include "hunterclaw/infra/instance_lock.h"
#include "hunterclaw/core/utils.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <sstream>

namespace hunterclaw {
namespace infra {

// ==================== 构造/析构 ====================

InstanceLock::InstanceLock(int fd, const std::string& lock_path)
    : fd_(fd), lock_path_(lock_path) {}

InstanceLock::InstanceLock(InstanceLock&& other) noexcept
    : fd_(other.fd_), lock_path_(std::move(other.lock_path_)) {
    other.fd_ = -1;
}

InstanceLock& InstanceLock::operator=(InstanceLock&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
        }
        fd_ = other.fd_;
        lock_path_ = std::move(other.lock_path_);
        other.fd_ = -1;
    }
    return *this;
}

InstanceLock::~InstanceLock() {
    if (fd_ >= 0) {
        // 写入 PID (用于 GetLockOwner)
        std::ofstream file(lock_path_, std::ios::trunc);
        if (file.is_open()) {
            file << "0" << std::endl;  // 清空 PID
            file.close();
        }
        
        // 释放锁
        flock(fd_, LOCK_UN);
        close(fd_);
        
        // 删除锁文件
        unlink(lock_path_.c_str());
    }
}

// ==================== 公共接口 ====================

core::Result<InstanceLock> InstanceLock::Acquire(const std::string& name) {
    std::string lock_path = GetLockPath(name);
    
    // 确保目录存在
    std::string dir = lock_path.substr(0, lock_path.find_last_of('/'));
    core::utils::ensure_directory(dir);
    
    // 打开锁文件 (创建如果不存在)
    int fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return Result<InstanceLock>::Err("Failed to open lock file: " + std::string(strerror(errno)));
    }
    
    // 尝试获取锁 (阻塞)
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return Result<InstanceLock>::Err("Failed to acquire lock: " + std::string(strerror(errno)));
    }
    
    // 写入当前 PID
    if (ftruncate(fd, 0) != 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return Result<InstanceLock>::Err("Failed to truncate lock file");
    }
    
    std::string pid_str = std::to_string(getpid()) + "\n";
    if (write(fd, pid_str.c_str(), pid_str.size()) != static_cast<ssize_t>(pid_str.size())) {
        flock(fd, LOCK_UN);
        close(fd);
        return Result<InstanceLock>::Err("Failed to write PID to lock file");
    }
    
    return Result<InstanceLock>::Ok(InstanceLock(fd, lock_path));
}

core::Result<InstanceLock> InstanceLock::TryAcquire(const std::string& name) {
    std::string lock_path = GetLockPath(name);
    
    // 确保目录存在
    std::string dir = lock_path.substr(0, lock_path.find_last_of('/'));
    core::utils::ensure_directory(dir);
    
    // 打开锁文件
    int fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return Result<InstanceLock>::Err("Failed to open lock file: " + std::string(strerror(errno)));
    }
    
    // 尝试获取锁 (非阻塞)
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        if (errno == EWOULDBLOCK) {
            return Result<InstanceLock>::Err("Lock already held by another process");
        }
        return Result<InstanceLock>::Err("Failed to acquire lock: " + std::string(strerror(errno)));
    }
    
    // 写入当前 PID
    if (ftruncate(fd, 0) != 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return Result<InstanceLock>::Err("Failed to truncate lock file");
    }
    
    std::string pid_str = std::to_string(getpid()) + "\n";
    if (write(fd, pid_str.c_str(), pid_str.size()) != static_cast<ssize_t>(pid_str.size())) {
        flock(fd, LOCK_UN);
        close(fd);
        return Result<InstanceLock>::Err("Failed to write PID to lock file");
    }
    
    return Result<InstanceLock>::Ok(InstanceLock(fd, lock_path));
}

bool InstanceLock::IsLocked(const std::string& name) {
    std::string lock_path = GetLockPath(name);
    
    int fd = open(lock_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;  // 文件不存在 = 未锁定
    }
    
    // 尝试非阻塞锁
    bool locked = (flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK);
    
    if (!locked) {
        flock(fd, LOCK_UN);
    }
    close(fd);
    
    return locked;
}

std::string InstanceLock::GetLockPath(const std::string& name) {
#ifdef __APPLE__
    return "/tmp/" + name + ".lock";
#elif defined(__linux__)
    return "/var/run/" + name + ".lock";
#elif defined(_WIN32)
    return "C:\\ProgramData\\" + name + ".lock";
#else
    return "/tmp/" + name + ".lock";
#endif
}

core::Result<uint32_t> InstanceLock::GetLockOwner(const std::string& name) {
    std::string lock_path = GetLockPath(name);
    
    std::ifstream file(lock_path);
    if (!file.is_open()) {
        return Result<uint32_t>::Err("Lock file not found");
    }
    
    uint32_t pid;
    file >> pid;
    file.close();
    
    if (pid == 0) {
        return Result<uint32_t>::Err("Invalid PID in lock file");
    }
    
    // 验证进程是否存在
    if (kill(pid, 0) != 0) {
        return Result<uint32_t>::Err("Process no longer exists");
    }
    
    return Result<uint32_t>::Ok(pid);
}

core::Result<void> InstanceLock::ForceRelease(const std::string& name) {
    std::string lock_path = GetLockPath(name);
    
    // 检查锁是否被持有
    if (!IsLocked(name)) {
        return Result<void>::Ok();  // 未锁定,无需释放
    }
    
    // 获取持有锁的进程
    auto owner_result = GetLockOwner(name);
    if (owner_result.is_ok()) {
        uint32_t pid = owner_result.unwrap();
        
        // 验证进程是否还活着
        if (kill(pid, 0) == 0) {
            return Result<void>::Err("Cannot force release: process " + std::to_string(pid) + " is still running");
        }
    }
    
    // 删除锁文件 (进程已死,安全释放)
    if (unlink(lock_path.c_str()) != 0) {
        return Result<void>::Err("Failed to remove lock file: " + std::string(strerror(errno)));
    }
    
    return Result<void>::Ok();
}

}  // namespace infra
}  // namespace hunterclaw
