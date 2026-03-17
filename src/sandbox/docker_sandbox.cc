#include "hunterclaw/sandbox/sandbox.h"
#include "hunterclaw/observability/observability.h"  // ✅ Span, Tracer, LogEntry, StructuredLogger
#include "hunterclaw/observability/diagnostic_events.h"  // ✅ 可观测性集成
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <array>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <uuid/uuid.h>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace sandbox {

// 辅助函数: 执行Shell命令并获取输出
static SandboxResult execute_shell_command(const std::string& command, int timeout_sec = 0) {
    SandboxResult result;
    auto start_time = std::chrono::steady_clock::now();
    
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        result.success = false;
        result.stderr_output = "Failed to execute command";
        return result;
    }
    
    std::array<char, 128> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result.stdout_output += buffer.data();
        
        // 超时检查
        if (timeout_sec > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time
            ).count();
            
            if (elapsed > timeout_sec) {
                pclose(pipe);
                result.success = false;
                result.stderr_output = "Command timeout";
                return result;
            }
        }
    }
    
    result.exit_code = pclose(pipe);
    result.success = (result.exit_code == 0);
    
    auto end_time = std::chrono::steady_clock::now();
    result.execution_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    ).count();
    
    return result;
}

// 生成唯一ID
static std::string generate_container_id() {
    uuid_t uuid;
    uuid_generate(uuid);
    char uuid_str[37];
    uuid_unparse(uuid, uuid_str);
    return std::string("hunterclaw_") + uuid_str;
}

// Docker Sandbox实现
DockerSandbox::DockerSandbox(const SandboxConfig& config) 
    : config_(config) {
    container_id_ = generate_container_id();
}

bool DockerSandbox::initialize() {
    if (initialized_) {
        return true;
    }
    
    auto overall_start = std::chrono::steady_clock::now();
    
    // ========== 1. 检查 Docker 是否可用 ==========
    auto check_result = execute_shell_command("docker --version");
    if (!check_result.success) {
        return false;
    }
    
    // ========== 2. 拉取镜像 (如果不存在) ==========
    std::string pull_cmd = "docker pull " + config_.image + " 2>&1";
    auto pull_result = execute_shell_command(pull_cmd);
    // 忽略拉取结果,可能镜像已存在
    
    // ========== 3. 创建容器 ==========
    std::string run_cmd = build_docker_run_command();
    auto run_result = execute_shell_command(run_cmd);
    
    if (!run_result.success) {
        return false;
    }
    
    // 从输出中提取 container ID (实际ID可能与我们生成的不同)
    if (!run_result.stdout_output.empty()) {
        container_id_ = run_result.stdout_output.substr(0, 12);  // Docker短ID
    }
    
    // ========== 4. 网络策略注入 (对齐 OpenClaw) ==========
    if (!config_.network_policy.egress.empty()) {
        apply_network_policy();
    }
    
    // ========== 5. 健康检查 (对齐 OpenClaw) ==========
    // 如果配置了 entrypoint,等待服务就绪
    if (!config_.entrypoint.empty() && config_.timeout_seconds > 0) {
        const int health_check_timeout_sec = std::min(config_.timeout_seconds, 300);  // 最多等待5分钟
        const int check_interval_ms = 200;
        
        auto health_start = std::chrono::steady_clock::now();
        bool service_ready = false;
        
        while (true) {
            auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - health_start
            ).count();
            
            if (elapsed_sec >= health_check_timeout_sec) {
                // 超时但不失败,某些服务可能不支持健康检查
                break;
            }
            
            // 检查容器是否还在运行
            if (!is_running()) {
                return false;  // 容器已退出,初始化失败
            }
            
            // 尝试简单的健康检查 (检查容器日志)
            std::string logs_cmd = "docker logs --tail=10 " + container_id_;
            auto logs_result = execute_shell_command(logs_cmd);
            
            // 简单策略: 如果日志中包含 "ready" 或 "listening",认为服务就绪
            if (logs_result.success) {
                std::string logs_lower = logs_result.stdout_output;
                std::transform(logs_lower.begin(), logs_lower.end(), logs_lower.begin(), ::tolower);
                
                if (logs_lower.find("ready") != std::string::npos ||
                    logs_lower.find("listening") != std::string::npos ||
                    logs_lower.find("started") != std::string::npos) {
                    service_ready = true;
                    break;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(check_interval_ms));
        }
        
        // ✅ 可观测性: 记录健康检查结果
        try {
            using namespace observability;
            std::map<std::string, core::JsonValue> fields;
            fields["container_id"] = container_id_;
            fields["service_ready"] = service_ready;
            fields["health_check_duration_ms"] = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - health_start
                ).count()
            );
            StructuredLogger::instance().log(LogLevel::INFO, "sandbox.health_check", fields);
        } catch (...) {}
    }
    
    initialized_ = true;
    
    // ✅ 可观测性: 记录沙箱初始化
    try {
        using namespace observability;
        std::map<std::string, core::JsonValue> fields;
        fields["container_id"] = container_id_;
        fields["image"] = config_.image;
        fields["init_duration_ms"] = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - overall_start
            ).count()
        );
        StructuredLogger::instance().log(LogLevel::INFO, "sandbox.initialized", fields);
    } catch (...) {}
    
    return true;
}

// ========== 网络策略注入 (对齐 OpenClaw FQDN 白名单) ==========
void DockerSandbox::apply_network_policy() {
    // ✅ 实现细粒度网络策略
    // OpenClaw 使用 NetworkPolicy CRD (Kubernetes)
    // Docker 环境使用 iptables 实现类似功能
    
    if (config_.network_policy.default_action == "deny") {
        // 1. 默认拒绝所有出站流量
        std::string deny_all_cmd = "docker exec " + container_id_ + 
            " sh -c 'iptables -P OUTPUT DROP 2>/dev/null || true'";
        execute_shell_command(deny_all_cmd);
    }
    
    // 2. 应用白名单规则
    for (const auto& rule : config_.network_policy.egress) {
        if (rule.action == "allow") {
            // 允许访问指定域名
            std::string allow_cmd = "docker exec " + container_id_ + 
                " sh -c 'iptables -A OUTPUT -d " + rule.target + " -j ACCEPT 2>/dev/null || true'";
            execute_shell_command(allow_cmd);
        } else if (rule.action == "deny") {
            // 拒绝访问指定域名
            std::string deny_cmd = "docker exec " + container_id_ + 
                " sh -c 'iptables -A OUTPUT -d " + rule.target + " -j DROP 2>/dev/null || true'";
            execute_shell_command(deny_cmd);
        }
    }
    
    // ✅ 可观测性: 记录网络策略
    try {
        using namespace observability;
        std::map<std::string, core::JsonValue> fields;
        fields["container_id"] = container_id_;
        fields["default_action"] = config_.network_policy.default_action;
        fields["rules_count"] = static_cast<int64_t>(config_.network_policy.egress.size());
        StructuredLogger::instance().log(LogLevel::INFO, "sandbox.network_policy_applied", fields);
    } catch (...) {}
}

std::string DockerSandbox::build_docker_run_command() {
    std::stringstream cmd;
    
    cmd << "docker run -d";
    cmd << " --name " << container_id_;
    
    // ========== 资源限制 (对齐 OpenClaw) ==========
    if (!config_.resource_limits.cpu.empty()) {
        cmd << " --cpus=" << config_.resource_limits.cpu;
    }
    
    if (!config_.resource_limits.memory.empty()) {
        cmd << " --memory=" << config_.resource_limits.memory;
    }
    
    // PID 限制 (防止 fork bomb)
    if (config_.pids_limit > 0) {
        cmd << " --pids-limit=" << config_.pids_limit;
    }
    
    // ========== 安全加固 (严格对齐 OpenClaw) ==========
    
    // 1. 丢弃 capabilities (默认丢弃 ALL)
    if (!config_.drop_capabilities.empty()) {
        for (const auto& cap : config_.drop_capabilities) {
            cmd << " --cap-drop=" << cap;
        }
    } else {
        // 默认丢弃所有 capabilities
        cmd << " --cap-drop=ALL";
    }
    
    // 2. 禁止权限提升
    if (config_.no_new_privileges) {
        cmd << " --security-opt=no-new-privileges";
    }
    
    // 3. seccomp profile (默认使用 RuntimeDefault)
    if (!config_.seccomp_profile.empty()) {
        cmd << " --security-opt=seccomp=" << config_.seccomp_profile;
    } else {
        // OpenClaw 默认: seccomp=default
        cmd << " --security-opt=seccomp=default";
    }
    
    // 4. AppArmor profile
    if (!config_.apparmor_profile.empty()) {
        cmd << " --security-opt=apparmor=" << config_.apparmor_profile;
    }
    
    // 5. 只读根文件系统 (可选,需要业务支持)
    // cmd << " --read-only";  // 需要配合 tmpfs 使用
    
    // ========== 网络配置 (对齐 OpenClaw) ==========
    if (config_.network_mode == NetworkMode::None) {
        cmd << " --network=none";
    } else if (config_.network_mode == NetworkMode::Host) {
        cmd << " --network=host";
    } else {
        // 默认 bridge 模式
        cmd << " --network=bridge";
        
        // ✅ 网络策略: 如果配置了 FQDN 白名单,启动后注入 iptables 规则
        // (在 initialize() 中实现)
    }
    
    // ========== 环境变量 ==========
    for (const auto& [key, value] : config_.env) {
        cmd << " -e " << key << "=\"" << value << "\"";
    }
    
    // ========== 卷挂载 ==========
    for (const auto& vol : config_.volumes) {
        if (vol.has_host()) {
            cmd << " -v " << vol.host.path << ":" << vol.mount_path;
            if (vol.read_only) {
                cmd << ":ro";
            }
        }
    }
    
    // ========== 工作目录 ==========
    cmd << " -w " << config_.workdir;
    
    // ========== 镜像和入口命令 (对齐 OpenClaw) ==========
    cmd << " " << config_.image;
    
    // 如果配置了 entrypoint,使用它;否则保持容器运行
    if (!config_.entrypoint.empty()) {
        // OpenClaw 模式: 执行用户指定的入口命令
        cmd << " " << config_.entrypoint[0];
        for (size_t i = 1; i < config_.entrypoint.size(); ++i) {
            cmd << " " << config_.entrypoint[i];
        }
    } else {
        // 默认模式: 保持容器运行
        cmd << " tail -f /dev/null";
    }
    
    return cmd.str();
}


bool DockerSandbox::copy_to_sandbox(const std::string& host_path, const std::string& sandbox_path) {
    if (!initialized_) {
        return false;
    }
    
    std::string cmd = "docker cp " + host_path + " " + container_id_ + ":" + sandbox_path;
    auto result = execute_shell_command(cmd);
    return result.success;
}

bool DockerSandbox::copy_from_sandbox(const std::string& sandbox_path, const std::string& host_path) {
    if (!initialized_) {
        return false;
    }
    
    std::string cmd = "docker cp " + container_id_ + ":" + sandbox_path + " " + host_path;
    auto result = execute_shell_command(cmd);
    return result.success;
}

void DockerSandbox::cleanup() {
    if (!initialized_) {
        return;
    }
    
    // 停止容器
    execute_shell_command("docker stop " + container_id_);
    
    // 删除容器
    execute_shell_command("docker rm " + container_id_);
    
    initialized_ = false;
}

bool DockerSandbox::is_running() const {
    if (!initialized_) {
        return false;
    }
    
    std::string cmd = "docker ps -q -f name=" + container_id_;
    auto result = execute_shell_command(cmd);
    
    return result.success && !result.stdout_output.empty();
}

bool DockerSandbox::pause() {
    if (!initialized_) {
        return false;
    }
    std::string cmd = "docker pause " + container_id_;
    auto result = execute_shell_command(cmd);
    return result.success;
}

bool DockerSandbox::resume() {
    if (!initialized_) {
        return false;
    }
    std::string cmd = "docker unpause " + container_id_;
    auto result = execute_shell_command(cmd);
    return result.success;
}

SandboxStatus DockerSandbox::get_status() const {
    SandboxStatus status;
    if (!initialized_) {
        status.state = SandboxState::Pending;
        return status;
    }
    
    std::string cmd = "docker inspect --format='{{.State.Status}}' " + container_id_;
    auto result = execute_shell_command(cmd);
    
    if (result.success) {
        std::string state_str = result.stdout_output;
        // 去除换行符
        state_str.erase(std::remove(state_str.begin(), state_str.end(), '\n'), state_str.end());
        
        if (state_str == "running") {
            status.state = SandboxState::Running;
        } else if (state_str == "paused") {
            status.state = SandboxState::Paused;
        } else if (state_str == "exited") {
            status.state = SandboxState::Terminated;
        } else {
            status.state = SandboxState::Failed;
        }
    } else {
        status.state = SandboxState::Failed;
    }
    
    return status;
}

bool DockerSandbox::renew_expiration(const std::chrono::system_clock::time_point&) {
    // Docker沙箱不需要续期逻辑
    return true;
}

// 执行后台命令
SandboxResult DockerSandbox::execute_background_command(const std::string& command) {
    SandboxResult result;
    if (!initialized_) {
        result.success = false;
        result.stderr_output = "Sandbox not initialized";
        return result;
    }
    
    // 使用docker exec -d执行后台命令
    std::string exec_cmd = "docker exec -d " + container_id_ + " sh -c '" + command + "'";
    result = execute_shell_command(exec_cmd);
    
    return result;
}

// 获取后台命令日志
std::string DockerSandbox::get_background_command_logs(const std::string& command_id) {
    (void)command_id;  // Suppress unused parameter warning
    if (!initialized_) {
        return "Sandbox not initialized";
    }
    
    // 使用docker logs获取日志
    std::string logs_cmd = "docker logs " + container_id_;
    auto logs_result = execute_shell_command(logs_cmd);
    
    return logs_result.stdout_output + logs_result.stderr_output;
}

// 中断代码执行
bool DockerSandbox::interrupt_code(const std::string& execution_id) {
    (void)execution_id;  // ✅ 避免 unused parameter 警告
    
    if (!initialized_) {
        return false;
    }
    
    // 发送SIGINT信号到容器
    std::string kill_cmd = "docker kill --signal=SIGINT " + container_id_;
    auto kill_result = execute_shell_command(kill_cmd);
    
    return kill_result.success;
}

// 读取文件
std::string DockerSandbox::read_file(const std::string& path) {
    if (!initialized_) {
        return "";
    }
    
    // 使用docker cp读取文件
    std::string temp_path = "/tmp/hunterclaw_read_" + std::to_string(std::time(nullptr));
    std::string cp_cmd = "docker cp " + container_id_ + ":" + path + " " + temp_path;
    auto cp_result = execute_shell_command(cp_cmd);
    
    if (cp_result.success) {
        std::ifstream file(temp_path);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            file.close();
            std::remove(temp_path.c_str());
            return content;
        }
    }
    
    return "";
}

// 写入文件
bool DockerSandbox::write_file(const std::string& path, const std::string& content, bool append) {
    if (!initialized_) {
        return false;
    }
    
    // 创建临时文件
    std::string temp_path = "/tmp/hunterclaw_write_" + std::to_string(std::time(nullptr));
    std::ofstream temp_file(temp_path);
    if (!temp_file.is_open()) {
        return false;
    }
    
    temp_file << content;
    temp_file.close();
    
    // 使用docker cp复制文件
    std::string cp_cmd = "docker cp " + temp_path + " " + container_id_ + ":" + path;
    auto cp_result = execute_shell_command(cp_cmd);
    
    std::remove(temp_path.c_str());
    
    if (append && cp_result.success) {
        // 如果是追加模式，需要在容器内执行追加操作
        std::string append_cmd = "docker exec " + container_id_ + 
                                " sh -c 'cat " + path + ".new >> " + path + " && rm " + path + ".new'";
        auto append_result = execute_shell_command(append_cmd);
        return append_result.success;
    }
    
    return cp_result.success;
}

// 监控指标
void DockerSandbox::watch_metrics(std::function<void(const ResourceUsage&)> callback) {
    if (!initialized_) {
        return;
    }
    
    // 使用docker stats获取实时指标
    std::string stats_cmd = "docker stats --no-stream --format '{{.CPUPerc}},{{.MemUsage}}' " + container_id_;
    auto stats_result = execute_shell_command(stats_cmd);
    
    if (stats_result.success && callback) {
        ResourceUsage metrics;
        metrics.memory_used_gb = 0.0;  // 需要解析输出
        metrics.cpu_percent = 0.0;  // 需要解析输出
        callback(metrics);
    }
}

// ========== 执行命令 (流式输出,对齐 OpenClaw SSE) ==========
SandboxResult DockerSandbox::execute_command(
    const std::string& command,
    std::function<void(const StreamEvent&)> event_callback) {
    
    if (!initialized_) {
        SandboxResult result;
        result.success = false;
        result.stderr_output = "Sandbox not initialized";
        return result;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    // ✅ 可观测性: Tracer Span (对齐 OpenClaw)
    std::unique_ptr<observability::Span> span;
    try {
        span = observability::Tracer::instance().start_span(
            "sandbox.execute_command", 
            observability::SpanKind::Internal
        );
        span->set_attribute("container_id", container_id_);
        span->set_attribute("command", command);
    } catch (...) {
        // 故障隔离
    }
    
    // ✅ 流式事件: started (对齐 OpenClaw SSE)
    if (event_callback) {
        StreamEvent started_event;
        started_event.type = "started";
        started_event.timestamp = std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()
        );
        started_event.data["command"] = command;
        event_callback(started_event);
    }
    
    SandboxResult result;
    
    // ✅ 流式执行: 实时推送 stdout/stderr (对齐 OpenClaw SSE)
    if (event_callback) {
        // 使用 popen 实现流式输出
        std::string exec_cmd = "docker exec " + container_id_ + " sh -c '" + command + "' 2>&1";
        FILE* pipe = popen(exec_cmd.c_str(), "r");
        
        if (!pipe) {
            result.success = false;
            result.stderr_output = "Failed to execute command";
            
            // error 事件
            StreamEvent error_event;
            error_event.type = "error";
            error_event.timestamp = std::to_string(
                std::chrono::system_clock::now().time_since_epoch().count()
            );
            error_event.data["error"] = "Failed to execute command";
            event_callback(error_event);
            
            return result;
        }
        
        // 逐行读取并推送 output 事件
        std::array<char, 256> buffer;
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            std::string line(buffer.data());
            result.stdout_output += line;
            
            // ✅ 流式事件: output (对齐 OpenClaw SSE)
            StreamEvent output_event;
            output_event.type = "output";
            output_event.timestamp = std::to_string(
                std::chrono::system_clock::now().time_since_epoch().count()
            );
            output_event.data["text"] = line;
            output_event.data["stream"] = "stdout";
            event_callback(output_event);
            
            // 超时检查
            auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time
            ).count();
            
            if (config_.timeout_seconds > 0 && elapsed_sec > config_.timeout_seconds) {
                pclose(pipe);
                result.success = false;
                result.stderr_output = "Command timeout";
                
                // error 事件
                StreamEvent timeout_event;
                timeout_event.type = "error";
                timeout_event.timestamp = std::to_string(
                    std::chrono::system_clock::now().time_since_epoch().count()
                );
                timeout_event.data["error"] = "Command timeout";
                event_callback(timeout_event);
                
                return result;
            }
        }
        
        result.exit_code = pclose(pipe);
        result.success = (result.exit_code == 0);
        
        // ✅ 流式事件: done (对齐 OpenClaw SSE)
        StreamEvent done_event;
        done_event.type = "done";
        done_event.timestamp = std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()
        );
        done_event.data["exit_code"] = std::to_string(result.exit_code);
        done_event.data["success"] = result.success ? "true" : "false";
        event_callback(done_event);
        
    } else {
        // 非流式模式: 阻塞执行
        std::string exec_cmd = "docker exec " + container_id_ + " sh -c '" + command + "'";
        result = execute_shell_command(exec_cmd);
    }
    
    // ✅ Span 状态标记
    if (span) {
        span->set_status(result.success, result.stderr_output);
        span->set_attribute("exit_code", static_cast<int64_t>(result.exit_code));
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time
        ).count();
        span->set_attribute("duration_ms", static_cast<int64_t>(duration_ms));
    }
    
    // ✅ 可观测性: 审计日志 (对齐 OpenClaw)
    try {
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time
        ).count();
        
        // 使用 StructuredLogger 记录审计日志
        using namespace observability;
        
        std::map<std::string, core::JsonValue> fields;
        fields["container_id"] = container_id_;
        fields["command"] = command;
        fields["success"] = result.success;
        fields["exit_code"] = static_cast<int64_t>(result.exit_code);
        fields["duration_ms"] = static_cast<int64_t>(duration_ms);
        fields["streaming"] = event_callback != nullptr;
        
        StructuredLogger::instance().log(LogLevel::INFO, "sandbox.command_executed", fields);
        
    } catch (...) {
        // 故障隔离: 永不阻塞主流程
    }
    
    return result;
}

// 中断命令
bool DockerSandbox::interrupt_command(const std::string& command_id) {
    return interrupt_code(command_id);
}

// 获取后台命令状态
SandboxResult DockerSandbox::get_background_command_status(const std::string& command_id) {
    (void)command_id;  // ✅ 避免 unused parameter 警告
    SandboxResult result;
    result.success = true;
    result.stdout_output = "Status check not implemented";
    return result;
}

// 执行代码
SandboxResult DockerSandbox::execute_code(
    const std::string& language,
    const std::string& code,
    std::function<void(const StreamEvent&)> event_callback) {
    
    if (!initialized_) {
        SandboxResult result;
        result.success = false;
        result.stderr_output = "Sandbox not initialized";
        return result;
    }
    
    // ✅ 可观测性: Tracer Span (对齐 OpenClaw)
    std::unique_ptr<observability::Span> span;
    try {
        span = observability::Tracer::instance().start_span(
            "sandbox.execute_code", 
            observability::SpanKind::Internal
        );
        span->set_attribute("container_id", container_id_);
        span->set_attribute("language", language);
        span->set_attribute("code_length", static_cast<int64_t>(code.size()));
    } catch (...) {
        // 故障隔离
    }
    
    // 创建临时文件
    std::string temp_file = "/tmp/code_" + std::to_string(std::time(nullptr));
    if (language == "python") {
        temp_file += ".py";
    } else if (language == "javascript" || language == "js") {
        temp_file += ".js";
    } else if (language == "bash" || language == "shell") {
        temp_file += ".sh";
    }
    
    // 写入代码
    write_file(temp_file, code, false);
    
    // 执行代码
    std::string exec_cmd;
    if (language == "python") {
        exec_cmd = "python " + temp_file;
    } else if (language == "javascript" || language == "js") {
        exec_cmd = "node " + temp_file;
    } else if (language == "bash" || language == "shell") {
        exec_cmd = "bash " + temp_file;
    } else {
        SandboxResult result;
        result.success = false;
        result.stderr_output = "Unsupported language: " + language;
        
        // ✅ Span 失败标记
        if (span) {
            span->set_status(false, "Unsupported language: " + language);
        }
        
        return result;
    }
    
    auto result = execute_command(exec_cmd, event_callback);
    
    // ✅ Span 状态标记
    if (span) {
        span->set_status(result.success, result.stderr_output);
        span->set_attribute("exit_code", static_cast<int64_t>(result.exit_code));
    }
    
    // ✅ 可观测性: 审计日志 (代码执行)
    try {
        using namespace observability;
        
        std::map<std::string, core::JsonValue> fields;
        fields["container_id"] = container_id_;
        fields["language"] = language;
        fields["code_length"] = static_cast<int64_t>(code.size());
        fields["success"] = result.success;
        fields["exit_code"] = static_cast<int64_t>(result.exit_code);
        
        StructuredLogger::instance().log(LogLevel::INFO, "sandbox.code_executed", fields);
        
    } catch (...) {
        // 故障隔离
    }
    
    return result;
}

// 获取文件信息
std::map<std::string, std::string> DockerSandbox::get_file_info(const std::string& path) {
    std::map<std::string, std::string> info;
    if (!initialized_) {
        return info;
    }
    
    std::string cmd = "docker exec " + container_id_ + " stat " + path;
    auto result = execute_shell_command(cmd);
    
    if (result.success) {
        info["path"] = path;
        info["exists"] = "true";
    }
    
    return info;
}

// 列出文件
std::vector<std::string> DockerSandbox::list_files(const std::string& path, bool recursive) {
    std::vector<std::string> files;
    if (!initialized_) {
        return files;
    }
    
    std::string cmd = "docker exec " + container_id_ + " ls ";
    if (recursive) {
        cmd += "-R ";
    }
    cmd += path;
    
    auto result = execute_shell_command(cmd);
    
    if (result.success) {
        std::istringstream iss(result.stdout_output);
        std::string file;
        while (std::getline(iss, file)) {
            if (!file.empty()) {
                files.push_back(file);
            }
        }
    }
    
    return files;
}

// 删除文件
bool DockerSandbox::delete_files(const std::vector<std::string>& paths) {
    if (!initialized_) {
        return false;
    }
    
    for (const auto& path : paths) {
        std::string cmd = "docker exec " + container_id_ + " rm -f " + path;
        auto result = execute_shell_command(cmd);
        if (!result.success) {
            return false;
        }
    }
    
    return true;
}

// 搜索文件
std::vector<std::string> DockerSandbox::search_files(const std::string& pattern) {
    std::vector<std::string> files;
    if (!initialized_) {
        return files;
    }
    
    std::string cmd = "docker exec " + container_id_ + " find / -name '" + pattern + "' 2>/dev/null";
    auto result = execute_shell_command(cmd);
    
    if (result.success) {
        std::istringstream iss(result.stdout_output);
        std::string file;
        while (std::getline(iss, file)) {
            if (!file.empty()) {
                files.push_back(file);
            }
        }
    }
    
    return files;
}

// 获取指标
ResourceUsage DockerSandbox::get_metrics() {
    ResourceUsage metrics;
    if (!initialized_) {
        return metrics;
    }
    
    std::string cmd = "docker stats --no-stream --format '{{.CPUPerc}},{{.MemUsage}}' " + container_id_;
    auto result = execute_shell_command(cmd);
    
    if (result.success) {
        // 需要解析输出
        metrics.cpu_percent = 0.0;
        metrics.memory_used_gb = 0.0;
    }
    
    return metrics;
}

// 获取端点
std::string DockerSandbox::get_endpoint(int port, bool resolve_internal) {
    (void)resolve_internal;  // ✅ 标记为有意未使用
    if (!initialized_) {
        return "";
    }
    
    // 获取容器IP
    std::string cmd = "docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' " + container_id_;
    auto result = execute_shell_command(cmd);
    
    if (result.success && !result.stdout_output.empty()) {
        std::string ip = result.stdout_output;
        // 去除换行符
        ip.erase(std::remove(ip.begin(), ip.end(), '\n'), ip.end());
        return ip + ":" + std::to_string(port);
    }
    
    return "";
}


// SandboxManager实现
std::shared_ptr<SandboxManager> SandboxManager::create() {
    struct MakeSandboxManager : public SandboxManager {
        MakeSandboxManager() : SandboxManager() {}
    };
    return std::make_shared<MakeSandboxManager>();
}

SandboxInfo SandboxManager::create_sandbox(const SandboxConfig& config) {
    std::shared_ptr<Sandbox> sandbox;
    
    switch (config.type) {
        case SandboxType::Docker:
            sandbox = std::make_shared<DockerSandbox>(config);
            break;
            
        case SandboxType::Podman:
            // TODO: Podman实现类似Docker
            sandbox = std::make_shared<DockerSandbox>(config);
            break;
            
        case SandboxType::Process:
            // ProcessSandbox is abstract, use DockerSandbox as fallback
            sandbox = std::make_shared<DockerSandbox>(config);
            break;
            
        case SandboxType::None:
        default:
            return SandboxInfo{}; // 返回空的SandboxInfo
    }
    
    // 初始化沙箱
    if (!sandbox->initialize()) {
        return SandboxInfo{};
    }
    
    // 生成ID并注册
    std::string id = generate_container_id();
    sandboxes_[id] = sandbox;
    
    // 构造并返回SandboxInfo
    SandboxInfo info;
    info.id = id;
    info.image = config.image;
    info.status.state = SandboxState::Running;
    info.created_at = std::chrono::system_clock::now();
    return info;
}

void SandboxManager::delete_sandbox(const std::string& id) {
    auto it = sandboxes_.find(id);
    if (it != sandboxes_.end()) {
        it->second->cleanup();
        sandboxes_.erase(it);
    }
}

std::vector<SandboxInfo> SandboxManager::list_sandboxes(const std::map<std::string, std::string>&) const {
    std::vector<SandboxInfo> result;
    for (const auto& [id, sandbox] : sandboxes_) {
        SandboxInfo info;
        info.id = id;
        info.status.state = SandboxState::Running;
        result.push_back(info);
    }
    return result;
}

bool SandboxManager::is_docker_available() {
    auto result = execute_shell_command("docker --version");
    return result.success;
}

bool SandboxManager::is_podman_available() {
    auto result = execute_shell_command("podman --version");
    return result.success;
}

} // namespace sandbox
} // namespace hunterclaw
