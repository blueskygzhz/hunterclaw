#pragma once

#include <string>
#include <optional>
#include <map>
#include <functional>
#include <chrono>

namespace hunterclaw {
namespace core {

// 运行模式
enum class SpawnMode {
    Child,  // 标准子进程
    Pty     // 伪终端（交互式shell）
};

// 运行状态
enum class RunState {
    Starting,  // 启动中
    Running,   // 运行中
    Exiting,   // 退出中
    Exited     // 已退出
};

// 终止原因
enum class TerminationReason {
    ManualCancel,      // 手动取消
    OverallTimeout,    // 全局超时
    NoOutputTimeout,   // 无输出超时
    SpawnError,        // 启动错误
    Signal,            // 信号终止
    Exit               // 正常退出
};

// 获取终止原因字符串
inline const char* GetTerminationReasonName(TerminationReason reason) {
    switch (reason) {
        case TerminationReason::ManualCancel:    return "manual-cancel";
        case TerminationReason::OverallTimeout:  return "overall-timeout";
        case TerminationReason::NoOutputTimeout: return "no-output-timeout";
        case TerminationReason::SpawnError:      return "spawn-error";
        case TerminationReason::Signal:          return "signal";
        case TerminationReason::Exit:            return "exit";
        default:                                  return "unknown";
    }
}

// Spawn输入参数
struct SpawnInput {
    // 运行模式
    SpawnMode mode = SpawnMode::Child;
    
    // 会话和后端标识
    std::string session_id;
    std::string backend_id;
    
    // 命令和参数
    std::vector<std::string> argv;
    
    // 超时配置
    uint32_t timeout_ms = 0;           // 全局超时（0=无限）
    uint32_t no_output_timeout_ms = 0; // 无输出超时（0=禁用）
    
    // 作用域管理（多租户隔离）
    std::optional<std::string> scope_key;
    
    // 工作目录和环境变量
    std::optional<std::string> cwd;
    std::map<std::string, std::string> env;
    
    // 输出回调（流式处理）
    std::function<void(const std::string&)> on_stdout;
    std::function<void(const std::string&)> on_stderr;
    
    // 是否捕获输出（如果false，只调用回调不保存）
    bool capture_output = true;
    
    // stdin模式
    enum class StdinMode {
        Inherit,    // 继承父进程stdin
        PipeOpen,   // 打开管道（可写入）
        PipeClosed  // 管道关闭
    } stdin_mode = StdinMode::PipeClosed;
};

// 运行退出结果
struct RunExit {
    int exit_code = -1;
    std::optional<std::string> signal;
    std::string stdout_output;
    std::string stderr_output;
    TerminationReason reason = TerminationReason::Exit;
    uint64_t duration_ms = 0;
};

// 运行记录（内部状态追踪）
struct RunRecord {
    std::string run_id;
    std::string session_id;
    std::string backend_id;
    std::optional<std::string> scope_key;
    
    std::optional<int> pid;
    RunState state = RunState::Starting;
    
    uint64_t started_at_ms = 0;
    uint64_t last_output_at_ms = 0;  // 用于无输出超时检测
    std::optional<uint64_t> exited_at_ms;
    
    std::optional<TerminationReason> termination_reason;
    std::optional<int> exit_code;
    std::optional<std::string> signal;
    
    std::string stdout_buffer;
    std::string stderr_buffer;
};

}  // namespace core
}  // namespace hunterclaw
