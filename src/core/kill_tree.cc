#include "hunterclaw/core/kill_tree.h"
#include <thread>
#include <cstring>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
    #include <tlhelp32.h>
    #include <psapi.h>
#else
    #include <signal.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include <errno.h>
    #include <dirent.h>
    #include <fstream>
    #include <sstream>
#endif

namespace hunterclaw {
namespace core {

// ============================================================================
// Unix实现
// ============================================================================

#ifndef _WIN32

Result<void> KillTree::KillUnix(int pid, const KillTreeOptions& options) {
    if (pid <= 0) {
        return Result<void>::Err("Invalid PID");
    }
    
    // 检查进程是否存在
    if (!IsAlive(pid)) {
        return Result<void>::Ok();  // 已经不存在，视为成功
    }
    
    // 如果强制终止，直接发送SIGKILL
    if (options.force) {
        return ForceKill(pid);
    }
    
    // 优雅关闭流程
    int target = options.use_process_group ? -pid : pid;
    
    // 1. 发送SIGTERM
    if (::kill(target, SIGTERM) == -1) {
        if (errno == ESRCH) {
            return Result<void>::Ok();  // 进程已不存在
        }
        return Result<void>::Err(
            std::string("Failed to send SIGTERM: ") + std::strerror(errno)
        );
    }
    
    // 2. 等待grace period
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < options.grace_ms) {
        if (!IsAlive(pid)) {
            return Result<void>::Ok();  // 进程已退出
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // 3. 超时后发送SIGKILL
    if (IsAlive(pid)) {
        if (::kill(target, SIGKILL) == -1) {
            if (errno == ESRCH) {
                return Result<void>::Ok();  // 进程已不存在
            }
            return Result<void>::Err(
                std::string("Failed to send SIGKILL: ") + std::strerror(errno)
            );
        }
        
        // 等待最多1秒确认终止
        for (int i = 0; i < 20; ++i) {
            if (!IsAlive(pid)) {
                return Result<void>::Ok();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    return Result<void>::Ok();
}

bool KillTree::IsAlive(int pid) {
    if (pid <= 0) return false;
    
    // 发送信号0检查进程是否存在（不会实际发送信号）
    return ::kill(pid, 0) == 0;
}

Result<void> KillTree::SendSignal(int pid, int signal) {
    if (pid <= 0) {
        return Result<void>::Err("Invalid PID");
    }
    
    if (::kill(pid, signal) == -1) {
        if (errno == ESRCH) {
            return Result<void>::Err("Process not found");
        }
        return Result<void>::Err(
            std::string("Failed to send signal: ") + std::strerror(errno)
        );
    }
    
    return Result<void>::Ok();
}

Result<std::vector<int>> KillTree::GetChildProcesses(int pid) {
    std::vector<int> children;
    
    // 读取 /proc/[pid]/task/[pid]/children (Linux)
    std::string path = "/proc/" + std::to_string(pid) + "/task/" + 
                       std::to_string(pid) + "/children";
    
    std::ifstream file(path);
    if (!file.is_open()) {
        // 回退：扫描所有进程
        DIR* dir = opendir("/proc");
        if (!dir) {
            return Result<std::vector<int>>::Err("Failed to open /proc");
        }
        
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR) {
                int child_pid = std::atoi(entry->d_name);
                if (child_pid > 0) {
                    // 读取 /proc/[child_pid]/stat 获取父进程ID
                    std::string stat_path = std::string("/proc/") + 
                                           entry->d_name + "/stat";
                    std::ifstream stat_file(stat_path);
                    if (stat_file.is_open()) {
                        std::string line;
                        std::getline(stat_file, line);
                        
                        // 格式: pid (comm) state ppid ...
                        size_t rparen = line.rfind(')');
                        if (rparen != std::string::npos) {
                            std::istringstream iss(line.substr(rparen + 1));
                            char state;
                            int ppid;
                            iss >> state >> ppid;
                            
                            if (ppid == pid) {
                                children.push_back(child_pid);
                            }
                        }
                    }
                }
            }
        }
        closedir(dir);
        
        return Result<std::vector<int>>::Ok(children);
    }
    
    // 读取children文件
    int child_pid;
    while (file >> child_pid) {
        children.push_back(child_pid);
    }
    
    return Result<std::vector<int>>::Ok(children);
}

#endif  // !_WIN32

// ============================================================================
// Windows实现
// ============================================================================

#ifdef _WIN32

Result<void> KillTree::KillWindows(int pid, const KillTreeOptions& options) {
    if (pid <= 0) {
        return Result<void>::Err("Invalid PID");
    }
    
    // 使用taskkill命令（最可靠的方式）
    std::string command = "taskkill /PID " + std::to_string(pid) + " /T";
    
    if (options.force) {
        command += " /F";  // 强制终止
    }
    
    // 执行命令
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    if (!CreateProcessA(
        nullptr,
        const_cast<char*>(command.c_str()),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    )) {
        return Result<void>::Err(
            "Failed to create taskkill process: " + 
            std::to_string(GetLastError())
        );
    }
    
    // 等待taskkill完成
    WaitForSingleObject(pi.hProcess, 5000);
    
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    if (exit_code == 0 || exit_code == 128) {
        // 0=成功, 128=进程已不存在
        return Result<void>::Ok();
    }
    
    return Result<void>::Err(
        "taskkill failed with exit code " + std::to_string(exit_code)
    );
}

bool KillTree::IsAlive(int pid) {
    if (pid <= 0) return false;
    
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == nullptr) {
        return false;  // 无法打开句柄，认为进程不存在
    }
    
    DWORD exit_code = 0;
    bool alive = false;
    
    if (GetExitCodeProcess(hProcess, &exit_code)) {
        alive = (exit_code == STILL_ACTIVE);
    }
    
    CloseHandle(hProcess);
    return alive;
}

Result<void> KillTree::SendSignal(int pid, int signal) {
    // Windows不支持Unix信号，映射为TerminateProcess
    if (pid <= 0) {
        return Result<void>::Err("Invalid PID");
    }
    
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess == nullptr) {
        return Result<void>::Err(
            "Failed to open process: " + std::to_string(GetLastError())
        );
    }
    
    BOOL success = TerminateProcess(hProcess, 1);
    CloseHandle(hProcess);
    
    if (!success) {
        return Result<void>::Err(
            "Failed to terminate process: " + std::to_string(GetLastError())
        );
    }
    
    return Result<void>::Ok();
}

Result<std::vector<int>> KillTree::GetChildProcesses(int pid) {
    std::vector<int> children;
    
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return Result<std::vector<int>>::Err(
            "Failed to create process snapshot"
        );
    }
    
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    
    if (!Process32First(snapshot, &pe32)) {
        CloseHandle(snapshot);
        return Result<std::vector<int>>::Err("Failed to enumerate processes");
    }
    
    do {
        if (pe32.th32ParentProcessID == static_cast<DWORD>(pid)) {
            children.push_back(static_cast<int>(pe32.th32ProcessID));
        }
    } while (Process32Next(snapshot, &pe32));
    
    CloseHandle(snapshot);
    return Result<std::vector<int>>::Ok(children);
}

#endif  // _WIN32

// ============================================================================
// 公共接口实现
// ============================================================================

Result<void> KillTree::Kill(int pid, const KillTreeOptions& options) {
#ifdef _WIN32
    return KillWindows(pid, options);
#else
    return KillUnix(pid, options);
#endif
}

Result<void> KillTree::ForceKill(int pid) {
    KillTreeOptions options;
    options.force = true;
    return Kill(pid, options);
}

Result<void> KillTree::GracefulKillProcess(
    int pid, 
    std::chrono::milliseconds grace_ms
) {
    KillTreeOptions options;
    options.grace_ms = grace_ms;
    options.use_process_group = false;  // 只终止单个进程
    return Kill(pid, options);
}

}  // namespace core
}  // namespace hunterclaw
