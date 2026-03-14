#pragma once

#include <string>
#include <vector>

namespace hunterclaw {
namespace core {
namespace subprocess {

// 进程执行结果结构
struct ProcessResult {
    int exit_code;              // 退出码 (0 表示成功)
    std::string stdout_output;  // 标准输出
    std::string stderr_output;  // 标准错误输出
    bool timed_out = false;     // 是否超时
    bool idle_timeout = false;  // 是否因无输出超时
};

// 执行选项
struct RunOptions {
    int timeout_ms = 30000;           // 全局超时(毫秒)
    int idle_timeout_ms = 0;          // 无输出超时(毫秒, 0表示禁用)
    std::string cwd;                  // 工作目录
    bool inherit_env = true;          // 是否继承环境变量
};

// 执行命令(同步,带超时)
// 参数:
//   command: 命令数组,第一个元素是程序路径,后续是参数
//   options: 执行选项
// 返回:
//   ProcessResult 包含退出码和输出
ProcessResult run(
    const std::vector<std::string>& command,
    const RunOptions& options = {}
);

// 向后兼容的重载版本
inline ProcessResult run(
    const std::vector<std::string>& command,
    int timeout_ms
) {
    RunOptions options;
    options.timeout_ms = timeout_ms;
    return run(command, options);
}

// Popen 风格的进程句柄
struct Process {
    void* handle;  // 内部实现指针 (指向 subprocess_s)
    
    ~Process();
    
    // 读取标准输出 (非阻塞)
    std::string read_stdout(size_t max_size = 4096);
    
    // 读取标准错误 (非阻塞)
    std::string read_stderr(size_t max_size = 4096);
    
    // 写入标准输入
    bool write_stdin(const std::string& data);
    
    // 检查进程是否还在运行
    bool is_alive();
    
    // 等待进程结束并获取退出码 (阻塞)
    int wait();
    
    // 获取进程ID (如果支持)
    int pid();
};

// 启动进程 (异步,返回句柄)
Process* popen(
    const std::vector<std::string>& command,
    bool capture_stdout = true,
    bool capture_stderr = true
);

} // namespace subprocess
} // namespace core
} // namespace hunterclaw
