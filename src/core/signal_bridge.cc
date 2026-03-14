#include "hunterclaw/core/signal_bridge.h"
#include "hunterclaw/core/kill_tree.h"
#include <map>
#include <mutex>
#include <atomic>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <signal.h>
    #include <unistd.h>
    #include <cstring>
#endif

namespace hunterclaw {
namespace core {

// ============================================================================
// Unix实现
// ============================================================================

#ifndef _WIN32

class SignalBridge::UnixSignalBridge : public SignalBridgeHandle {
public:
    UnixSignalBridge(int child_pid, const SignalBridgeOptions& options)
        : child_pid_(child_pid), options_(options), detached_(false) {
        
        // 注册信号处理器
        if (options_.forward_termination_signals) {
            RegisterSignal(SIGINT);
            RegisterSignal(SIGTERM);
        }
        
        if (options_.forward_user_signals) {
            RegisterSignal(SIGHUP);
            RegisterSignal(SIGUSR1);
            RegisterSignal(SIGUSR2);
        }
    }
    
    ~UnixSignalBridge() override {
        detach();
    }
    
    void detach() override {
        if (detached_.exchange(true)) {
            return;  // 已分离
        }
        
        // 取消注册信号处理器
        std::lock_guard<std::mutex> lock(bridges_mutex_);
        bridges_.erase(this);
        
        // 如果没有其他桥接，恢复默认信号处理
        if (bridges_.empty()) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            signal(SIGUSR1, SIG_DFL);
            signal(SIGUSR2, SIG_DFL);
        }
    }
    
    bool is_detached() const override {
        return detached_.load();
    }
    
private:
    void RegisterSignal(int sig) {
        std::lock_guard<std::mutex> lock(bridges_mutex_);
        
        // 添加到桥接列表
        bridges_[this] = child_pid_;
        
        // 设置信号处理器（首次）
        if (bridges_.size() == 1) {
            struct sigaction sa;
            std::memset(&sa, 0, sizeof(sa));
            sa.sa_handler = &GlobalSignalHandler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESTART;
            
            sigaction(sig, &sa, nullptr);
        }
    }
    
    static void GlobalSignalHandler(int sig) {
        std::lock_guard<std::mutex> lock(bridges_mutex_);
        
        // 转发信号到所有子进程
        for (const auto& pair : bridges_) {
            UnixSignalBridge* bridge = pair.first;
            int child_pid = pair.second;
            
            if (bridge->detached_.load()) {
                continue;
            }
            
            // 调用前置回调
            if (bridge->options_.on_signal_before_forward) {
                bridge->options_.on_signal_before_forward(sig);
            }
            
            // 转发信号
            KillTree::SendSignal(child_pid, sig);
            
            // 调用后置回调
            if (bridge->options_.on_signal_after_forward) {
                bridge->options_.on_signal_after_forward(sig);
            }
        }
    }
    
    int child_pid_;
    SignalBridgeOptions options_;
    std::atomic<bool> detached_;
    
    // 全局桥接列表（用于信号处理器）
    static std::mutex bridges_mutex_;
    static std::map<UnixSignalBridge*, int> bridges_;
};

std::mutex SignalBridge::UnixSignalBridge::bridges_mutex_;
std::map<SignalBridge::UnixSignalBridge*, int> 
    SignalBridge::UnixSignalBridge::bridges_;

#endif  // !_WIN32

// ============================================================================
// Windows实现
// ============================================================================

#ifdef _WIN32

class SignalBridge::WindowsSignalBridge : public SignalBridgeHandle {
public:
    WindowsSignalBridge(int child_pid, const SignalBridgeOptions& options)
        : child_pid_(child_pid), 
          child_handle_(nullptr),
          options_(options), 
          detached_(false) {
        
        // 打开子进程句柄
        child_handle_ = OpenProcess(
            PROCESS_TERMINATE | SYNCHRONIZE, 
            FALSE, 
            child_pid
        );
        
        // 注册控制台事件处理器
        if (options_.forward_termination_signals) {
            RegisterHandler();
        }
    }
    
    ~WindowsSignalBridge() override {
        detach();
        
        if (child_handle_) {
            CloseHandle(child_handle_);
        }
    }
    
    void detach() override {
        if (detached_.exchange(true)) {
            return;
        }
        
        // 取消注册处理器
        std::lock_guard<std::mutex> lock(bridges_mutex_);
        bridges_.erase(this);
        
        // 如果没有其他桥接，移除控制台处理器
        if (bridges_.empty()) {
            SetConsoleCtrlHandler(&GlobalCtrlHandler, FALSE);
        }
    }
    
    bool is_detached() const override {
        return detached_.load();
    }
    
private:
    void RegisterHandler() {
        std::lock_guard<std::mutex> lock(bridges_mutex_);
        
        // 添加到桥接列表
        bridges_[this] = child_handle_;
        
        // 设置控制台处理器（首次）
        if (bridges_.size() == 1) {
            SetConsoleCtrlHandler(&GlobalCtrlHandler, TRUE);
        }
    }
    
    static BOOL WINAPI GlobalCtrlHandler(DWORD ctrl_type) {
        std::lock_guard<std::mutex> lock(bridges_mutex_);
        
        // 映射控制台事件到信号
        int signal = 0;
        switch (ctrl_type) {
            case CTRL_C_EVENT:
                signal = 2;  // SIGINT
                break;
            case CTRL_BREAK_EVENT:
                signal = 15;  // SIGTERM
                break;
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                signal = 15;  // SIGTERM
                break;
            default:
                return FALSE;
        }
        
        // 转发到所有子进程
        for (const auto& pair : bridges_) {
            WindowsSignalBridge* bridge = pair.first;
            HANDLE child_handle = pair.second;
            
            if (bridge->detached_.load() || !child_handle) {
                continue;
            }
            
            // 调用前置回调
            if (bridge->options_.on_signal_before_forward) {
                bridge->options_.on_signal_before_forward(signal);
            }
            
            // 终止进程（Windows没有信号机制）
            TerminateProcess(child_handle, 1);
            
            // 调用后置回调
            if (bridge->options_.on_signal_after_forward) {
                bridge->options_.on_signal_after_forward(signal);
            }
        }
        
        return TRUE;  // 已处理
    }
    
    int child_pid_;
    HANDLE child_handle_;
    SignalBridgeOptions options_;
    std::atomic<bool> detached_;
    
    // 全局桥接列表
    static std::mutex bridges_mutex_;
    static std::map<WindowsSignalBridge*, HANDLE> bridges_;
};

std::mutex SignalBridge::WindowsSignalBridge::bridges_mutex_;
std::map<SignalBridge::WindowsSignalBridge*, HANDLE> 
    SignalBridge::WindowsSignalBridge::bridges_;

#endif  // _WIN32

// ============================================================================
// 公共接口实现
// ============================================================================

std::unique_ptr<SignalBridgeHandle> SignalBridge::Attach(
    int child_pid,
    const SignalBridgeOptions& options
) {
#ifdef _WIN32
    return std::make_unique<WindowsSignalBridge>(child_pid, options);
#else
    return std::make_unique<UnixSignalBridge>(child_pid, options);
#endif
}

#ifdef _WIN32
std::unique_ptr<SignalBridgeHandle> SignalBridge::AttachHandle(
    void* child_process_handle,
    const SignalBridgeOptions& options
) {
    // 获取进程ID
    DWORD pid = GetProcessId(static_cast<HANDLE>(child_process_handle));
    return std::make_unique<WindowsSignalBridge>(pid, options);
}
#endif

Result<void> SignalBridge::SendSignal(int pid, int signal) {
    return KillTree::SendSignal(pid, signal);
}

// 全局信号处理器注册（简化版本）
static std::mutex global_handlers_mutex;
static std::map<int, std::function<void(int)>> global_handlers;

Result<void> SignalBridge::RegisterHandler(
    int signal, 
    std::function<void(int)> handler
) {
    std::lock_guard<std::mutex> lock(global_handlers_mutex);
    global_handlers[signal] = handler;
    
#ifndef _WIN32
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int sig) {
        std::lock_guard<std::mutex> lock(global_handlers_mutex);
        auto it = global_handlers.find(sig);
        if (it != global_handlers.end()) {
            it->second(sig);
        }
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(signal, &sa, nullptr) == -1) {
        return Result<void>::Err(
            std::string("Failed to register signal handler: ") + 
            std::strerror(errno)
        );
    }
#endif
    
    return Result<void>::Ok();
}

Result<void> SignalBridge::UnregisterHandler(int signal) {
    std::lock_guard<std::mutex> lock(global_handlers_mutex);
    global_handlers.erase(signal);
    
#ifndef _WIN32
    ::signal(signal, SIG_DFL);
#endif
    
    return Result<void>::Ok();
}

}  // namespace core
}  // namespace hunterclaw
