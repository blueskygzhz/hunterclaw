#include "hunterclaw/core/subprocess.h"
extern "C" {
#include "subprocess.h"  // External C library
}
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <memory>

namespace hunterclaw {
namespace core {
namespace subprocess {

namespace {
    // 辅助函数: 将 vector<string> 转换为 C 风格 char* 数组
    std::vector<char*> to_c_args(const std::vector<std::string>& command) {
        std::vector<char*> args;
        args.reserve(command.size() + 1);
        
        for (const auto& arg : command) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);  // 以 NULL 结尾
        
        return args;
    }
    
    // 读取文件流到字符串
    std::string read_stream(FILE* stream, size_t max_size = 1024 * 1024) {
        if (!stream) return "";
        
        std::string result;
        char buffer[4096];
        
        while (result.size() < max_size) {
            size_t bytes = fread(buffer, 1, sizeof(buffer), stream);
            if (bytes == 0) break;
            result.append(buffer, bytes);
        }
        
        return result;
    }
}

// ============================================================================
// ProcessResult run() 实现
// ============================================================================

ProcessResult run(
    const std::vector<std::string>& command,
    const RunOptions& options
) {
    ProcessResult result;
    result.exit_code = -1;
    
    if (command.empty()) {
        result.stderr_output = "Error: empty command";
        return result;
    }
    
    // 转换为 C 风格参数
    auto args = to_c_args(command);
    
    // 创建进程
    struct subprocess_s process;
    int proc_options = subprocess_option_enable_async;
    
    if (options.inherit_env) {
        proc_options |= subprocess_option_inherit_environment;
    }
    
    int ret = subprocess_create(
        args.data(),
        proc_options,
        &process
    );
    
    if (ret != 0) {
        result.stderr_output = "Error: failed to create process";
        return result;
    }
    
    // 等待进程完成 (带超时)
    auto start_time = std::chrono::steady_clock::now();
    auto last_output_time = start_time;
    size_t last_output_size = 0;
    
    while (true) {
        // 检查是否还活着
        if (!subprocess_alive(&process)) {
            break;
        }
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time).count();
        
        // 检查全局超时
        if (options.timeout_ms > 0 && elapsed_ms > options.timeout_ms) {
            subprocess_terminate(&process);
            subprocess_destroy(&process);
            result.stderr_output = "Error: process timeout (global)";
            result.exit_code = -2;
            result.timed_out = true;
            return result;
        }
        
        // 检查无输出超时
        if (options.idle_timeout_ms > 0) {
            // 读取当前输出大小
            FILE* stdout_file = subprocess_stdout(&process);
            FILE* stderr_file = subprocess_stderr(&process);
            
            size_t current_size = 0;
            if (stdout_file) {
                long pos = ftell(stdout_file);
                fseek(stdout_file, 0, SEEK_END);
                current_size += ftell(stdout_file);
                fseek(stdout_file, pos, SEEK_SET);
            }
            if (stderr_file) {
                long pos = ftell(stderr_file);
                fseek(stderr_file, 0, SEEK_END);
                current_size += ftell(stderr_file);
                fseek(stderr_file, pos, SEEK_SET);
            }
            
            // 如果有新输出,更新时间戳
            if (current_size > last_output_size) {
                last_output_time = now;
                last_output_size = current_size;
            }
            
            // 检查是否超过无输出超时
            auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_output_time).count();
            
            if (idle_ms > options.idle_timeout_ms) {
                subprocess_terminate(&process);
                subprocess_destroy(&process);
                result.stderr_output = "Error: process timeout (no output)";
                result.exit_code = -3;
                result.timed_out = true;
                result.idle_timeout = true;
                return result;
            }
        }
        
        // 短暂休眠避免 CPU 占用
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 读取输出
    FILE* stdout_file = subprocess_stdout(&process);
    FILE* stderr_file = subprocess_stderr(&process);
    
    result.stdout_output = read_stream(stdout_file);
    result.stderr_output = read_stream(stderr_file);
    
    // 获取退出码
    int return_code = 0;
    subprocess_join(&process, &return_code);
    result.exit_code = return_code;
    
    // 清理
    subprocess_destroy(&process);
    
    return result;
}

// ============================================================================
// Process 类实现 (异步 popen)
// ============================================================================

Process::~Process() {
    if (handle) {
        auto* proc = static_cast<struct subprocess_s*>(handle);
        subprocess_destroy(proc);
        delete proc;
        handle = nullptr;
    }
}

std::string Process::read_stdout(size_t max_size) {
    if (!handle) return "";
    
    auto* proc = static_cast<struct subprocess_s*>(handle);
    FILE* stream = subprocess_stdout(proc);
    
    return read_stream(stream, max_size);
}

std::string Process::read_stderr(size_t max_size) {
    if (!handle) return "";
    
    auto* proc = static_cast<struct subprocess_s*>(handle);
    FILE* stream = subprocess_stderr(proc);
    
    return read_stream(stream, max_size);
}

bool Process::write_stdin(const std::string& data) {
    if (!handle) return false;
    
    auto* proc = static_cast<struct subprocess_s*>(handle);
    FILE* stream = subprocess_stdin(proc);
    
    if (!stream) return false;
    
    size_t written = fwrite(data.data(), 1, data.size(), stream);
    fflush(stream);
    
    return written == data.size();
}

bool Process::is_alive() {
    if (!handle) return false;
    
    auto* proc = static_cast<struct subprocess_s*>(handle);
    return subprocess_alive(proc) != 0;
}

int Process::wait() {
    if (!handle) return -1;
    
    auto* proc = static_cast<struct subprocess_s*>(handle);
    int return_code = 0;
    subprocess_join(proc, &return_code);
    
    return return_code;
}

int Process::pid() {
    // subprocess.h 不提供 PID 访问
    return -1;
}

Process* popen(
    const std::vector<std::string>& command,
    bool capture_stdout,
    bool capture_stderr
) {
    if (command.empty()) {
        return nullptr;
    }
    
    // 转换为 C 风格参数
    auto args = to_c_args(command);
    
    // 创建进程
    auto* proc = new struct subprocess_s;
    
    int options = subprocess_option_inherit_environment 
                | subprocess_option_enable_async;
    
    int ret = subprocess_create(
        args.data(),
        options,
        proc
    );
    
    if (ret != 0) {
        delete proc;
        return nullptr;
    }
    
    // 创建Process 包装
    auto* process = new Process();
    process->handle = proc;
    
    return process;
}

Process* popen_with_env(
    const std::vector<std::string>& command,
    const std::vector<std::string>& env,
    bool /*capture_stdout*/,
    bool /*capture_stderr*/
) {
    if (command.empty()) {
        return nullptr;
    }
    
    // 转换命令为 C 风格参数
    auto args = to_c_args(command);
    
    // 转换 env 为 C 风格数组 (NULL 结尾)
    std::vector<char*> c_env;
    c_env.reserve(env.size() + 1);
    for (const auto& e : env) {
        c_env.push_back(const_cast<char*>(e.c_str()));
    }
    c_env.push_back(nullptr);
    
    // 创建进程 — 🎯 关键: 不设置 inherit_environment, 传入显式 env,
    // 子进程仅拥有净化后的环境变量, 不继承宿主全部环境 (防凭据泄露)。
    auto* proc = new struct subprocess_s;
    int options = subprocess_option_enable_async;
    
    int ret = subprocess_create_ex(
        args.data(),
        options,
        c_env.data(),
        proc
    );
    
    if (ret != 0) {
        delete proc;
        return nullptr;
    }
    
    auto* process = new Process();
    process->handle = proc;
    
    return process;
}

} // namespace subprocess
} // namespace core
} // namespace hunterclaw
