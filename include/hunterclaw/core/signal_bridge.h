#pragma once

#include "hunterclaw/core/result.h"
#include <functional>
#include <memory>

namespace hunterclaw {
namespace core {

// 信号桥接选项
struct SignalBridgeOptions {
    // 是否转发SIGINT/SIGTERM（默认true）
    bool forward_termination_signals = true;
    
    // 是否转发SIGHUP/SIGUSR1/SIGUSR2（默认false）
    bool forward_user_signals = false;
    
    // 自定义信号处理器（在转发前调用）
    std::function<void(int signal)> on_signal_before_forward;
    
    // 自定义信号处理器（在转发后调用）
    std::function<void(int signal)> on_signal_after_forward;
};

// 信号桥接句柄
class SignalBridgeHandle {
public:
    virtual ~SignalBridgeHandle() = default;
    
    // 分离桥接（停止信号转发）
    virtual void detach() = 0;
    
    // 是否已分离
    virtual bool is_detached() const = 0;
};

// 信号桥接器
class SignalBridge {
public:
    // 附加信号桥接（父进程信号 → 子进程）
    // 参数:
    //   child_pid: 子进程ID
    //   options: 桥接选项
    // 返回:
    //   SignalBridgeHandle 用于控制桥接生命周期
    // 
    // 用法:
    //   auto handle = SignalBridge::Attach(child_pid);
    //   // 子进程运行...
    //   handle->detach();  // 或自动析构时分离
    static std::unique_ptr<SignalBridgeHandle> Attach(
        int child_pid,
        const SignalBridgeOptions& options = {}
    );
    
    // 附加桥接（使用句柄，Windows支持）
#ifdef _WIN32
    static std::unique_ptr<SignalBridgeHandle> AttachHandle(
        void* child_process_handle,
        const SignalBridgeOptions& options = {}
    );
#endif
    
    // 发送信号到进程
    // 参数:
    //   pid: 目标进程ID
    //   signal: 信号编号（Unix）或事件（Windows）
    static Result<void> SendSignal(int pid, int signal);
    
    // 注册全局信号处理器
    // 参数:
    //   signal: 信号编号（如 SIGTERM=15）
    //   handler: 处理器函数
    static Result<void> RegisterHandler(
        int signal, 
        std::function<void(int)> handler
    );
    
    // 取消注册全局信号处理器
    static Result<void> UnregisterHandler(int signal);
    
private:
    // 内部实现类（Unix）
    class UnixSignalBridge;
    
    // 内部实现类（Windows）
    class WindowsSignalBridge;
};

}  // namespace core
}  // namespace hunterclaw
