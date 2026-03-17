#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <memory>

namespace hunterclaw {
namespace infra {

// 单实例锁 (flock)
class InstanceLock {
public:
    // 禁止拷贝
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    
    // 允许移动
    InstanceLock(InstanceLock&&) noexcept;
    InstanceLock& operator=(InstanceLock&&) noexcept;
    
    // 释放锁
    ~InstanceLock();
    
    // 获取锁 (阻塞)
    static core::Result<InstanceLock> Acquire(const std::string& name);
    
    // 尝试获取锁 (非阻塞)
    static core::Result<InstanceLock> TryAcquire(const std::string& name);
    
    // 检查是否被锁
    static bool IsLocked(const std::string& name);
    
    // 获取锁文件路径
    static std::string GetLockPath(const std::string& name);
    
    // 获取持有锁的 PID (如果可用)
    static core::Result<uint32_t> GetLockOwner(const std::string& name);
    
    // 强制释放锁 (危险操作,仅在确认进程已死时使用)
    static core::Result<void> ForceRelease(const std::string& name);
    
private:
    InstanceLock(int fd, const std::string& lock_path);
    
    int fd_ = -1;
    std::string lock_path_;
};

}  // namespace infra
}  // namespace hunterclaw
