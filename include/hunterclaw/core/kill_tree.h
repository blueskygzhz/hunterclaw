#pragma once

#include "hunterclaw/core/result.h"
#include <cstdint>
#include <chrono>

namespace hunterclaw {
namespace core {

// 进程树终止选项
struct KillTreeOptions {
    // 优雅关闭等待时间（SIGTERM → SIGKILL）
    std::chrono::milliseconds grace_ms{3000};  // 默认3秒
    
    // 是否使用进程组终止（Unix: -pid）
    bool use_process_group = true;
    
    // 是否强制终止（直接SIGKILL）
    bool force = false;
};

// 进程树终止器
class KillTree {
public:
    // 终止进程树（优雅关闭）
    // 参数:
    //   pid: 目标进程ID
    //   options: 终止选项
    // 返回:
    //   Result<void> 成功或错误信息
    static Result<void> Kill(int pid, const KillTreeOptions& options = {});
    
    // 强制终止进程树（立即SIGKILL/TerminateProcess）
    static Result<void> ForceKill(int pid);
    
    // 检查进程是否存在
    static bool IsAlive(int pid);
    
    // 发送信号给进程
    // 参数:
    //   pid: 目标进程ID
    //   signal: Unix信号编号（如 SIGTERM=15, SIGKILL=9）
    //           Windows下会映射为TerminateProcess
    static Result<void> SendSignal(int pid, int signal);
    
private:
    // 平台相关实现
    static Result<void> KillUnix(int pid, const KillTreeOptions& options);
    static Result<void> KillWindows(int pid, const KillTreeOptions& options);
    
    // 获取子进程列表
    static Result<std::vector<int>> GetChildProcesses(int pid);
    
    // 优雅终止单个进程
    static Result<void> GracefulKillProcess(
        int pid, 
        std::chrono::milliseconds grace_ms
    );
};

}  // namespace core
}  // namespace hunterclaw
