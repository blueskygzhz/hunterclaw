#include "hunterclaw/memory/qmd_process.h"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <thread>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Output Management
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

OutputWithCap append_output_with_cap(
    const std::string& current,
    const std::string& chunk,
    size_t max_chars
) {
    std::string appended = current + chunk;
    if (appended.length() <= max_chars) {
        return {appended, false};
    }
    // Keep tail to avoid losing important info
    return {appended.substr(appended.length() - max_chars), true};
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Spawn Invocation Resolution
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CliSpawnInvocation resolve_cli_spawn_invocation(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::map<std::string, std::string>& env,
    const std::string& package_name
) {
    CliSpawnInvocation invocation;
    
    // Simple resolution: use command directly on Unix
    // Full Windows handling would require plugin-sdk/windows-spawn logic
    invocation.command = command;
    invocation.argv = args;
    invocation.shell = false;
    invocation.windows_hide = false;
    
    return invocation;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Command Execution
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

CliCommandResult run_cli_command(
    const std::string& command_summary,
    const CliSpawnInvocation& invocation,
    const std::map<std::string, std::string>& env,
    const std::string& cwd,
    int timeout_ms,
    size_t max_output_chars,
    bool discard_stdout
) {
    // Create pipes for stdout/stderr
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        throw std::runtime_error("Failed to create pipes");
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("Failed to fork process");
    }
    
    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        
        // Change directory
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            _exit(127);
        }
        
        // Set environment
        for (const auto& [key, value] : env) {
            setenv(key.c_str(), value.c_str(), 1);
        }
        
        // Prepare argv for execvp
        std::vector<char*> c_argv;
        c_argv.push_back(const_cast<char*>(invocation.command.c_str()));
        for (const auto& arg : invocation.argv) {
            c_argv.push_back(const_cast<char*>(arg.c_str()));
        }
        c_argv.push_back(nullptr);
        
        execvp(invocation.command.c_str(), c_argv.data());
        _exit(127);  // execvp failed
    }
    
    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    
    set_nonblocking(stdout_pipe[0]);
    set_nonblocking(stderr_pipe[0]);
    
    std::string stdout_text;
    std::string stderr_text;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    
    auto start_time = std::chrono::steady_clock::now();
    
    pollfd fds[2];
    fds[0].fd = stdout_pipe[0];
    fds[0].events = POLLIN;
    fds[1].fd = stderr_pipe[0];
    fds[1].events = POLLIN;
    
    bool done = false;
    while (!done) {
        // Check timeout
        if (timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time
            ).count();
            if (elapsed >= timeout_ms) {
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
                close(stdout_pipe[0]);
                close(stderr_pipe[0]);
                throw std::runtime_error(
                    command_summary + " timed out after " + 
                    std::to_string(timeout_ms) + "ms"
                );
            }
        }
        
        int poll_timeout = timeout_ms > 0 ? 100 : -1;
        int ret = poll(fds, 2, poll_timeout);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        // Read stdout
        if (!discard_stdout && (fds[0].revents & POLLIN)) {
            char buffer[4096];
            ssize_t n = read(stdout_pipe[0], buffer, sizeof(buffer));
            if (n > 0) {
                auto result = append_output_with_cap(
                    stdout_text, 
                    std::string(buffer, n), 
                    max_output_chars
                );
                stdout_text = result.text;
                stdout_truncated = stdout_truncated || result.truncated;
            }
        }
        
        // Read stderr
        if (fds[1].revents & POLLIN) {
            char buffer[4096];
            ssize_t n = read(stderr_pipe[0], buffer, sizeof(buffer));
            if (n > 0) {
                auto result = append_output_with_cap(
                    stderr_text, 
                    std::string(buffer, n), 
                    max_output_chars
                );
                stderr_text = result.text;
                stderr_truncated = stderr_truncated || result.truncated;
            }
        }
        
        // Check if process finished
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            done = true;
        }
    }
    
    // Read any remaining data
    for (int i = 0; i < 10; ++i) {
        char buffer[4096];
        
        if (!discard_stdout) {
            ssize_t n = read(stdout_pipe[0], buffer, sizeof(buffer));
            if (n > 0) {
                auto result = append_output_with_cap(
                    stdout_text, 
                    std::string(buffer, n), 
                    max_output_chars
                );
                stdout_text = result.text;
                stdout_truncated = stdout_truncated || result.truncated;
            }
        }
        
        ssize_t n = read(stderr_pipe[0], buffer, sizeof(buffer));
        if (n > 0) {
            auto result = append_output_with_cap(
                stderr_text, 
                std::string(buffer, n), 
                max_output_chars
            );
            stderr_text = result.text;
            stderr_truncated = stderr_truncated || result.truncated;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    
    // Check truncation
    if (!discard_stdout && (stdout_truncated || stderr_truncated)) {
        throw std::runtime_error(
            command_summary + " produced too much output (limit " + 
            std::to_string(max_output_chars) + " chars)"
        );
    }
    
    // Get exit status
    int status;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    
    if (exit_code != 0) {
        std::string error_msg = command_summary + " failed (code " + 
                                std::to_string(exit_code) + "): " + 
                                (!stderr_text.empty() ? stderr_text : stdout_text);
        throw std::runtime_error(error_msg);
    }
    
    return {stdout_text, stderr_text, exit_code};
}

}  // namespace memory
}  // namespace hunterclaw
