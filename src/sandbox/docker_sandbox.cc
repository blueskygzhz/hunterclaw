#include "hunterclaw/sandbox/sandbox.h"
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <array>
#include <chrono>
#include <filesystem>
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
    
    // 检查Docker是否可用
    auto check_result = execute_shell_command("docker --version");
    if (!check_result.success) {
        return false;
    }
    
    // 拉取镜像(如果不存在)
    std::string pull_cmd = "docker pull " + config_.image + " 2>&1";
    auto pull_result = execute_shell_command(pull_cmd);
    // 忽略拉取结果,可能镜像已存在
    
    // 创建容器
    std::string run_cmd = build_docker_run_command();
    auto run_result = execute_shell_command(run_cmd);
    
    if (!run_result.success) {
        return false;
    }
    
    // 从输出中提取container ID (实际ID可能与我们生成的不同)
    if (!run_result.stdout_output.empty()) {
        container_id_ = run_result.stdout_output.substr(0, 12);  // Docker短ID
    }
    
    initialized_ = true;
    return true;
}

std::string DockerSandbox::build_docker_run_command() {
    std::stringstream cmd;
    
    cmd << "docker run -d";
    cmd << " --name " << container_id_;
    
    // 资源限制
    
    // 资源限制
    if (!config_.resource_limits.cpu.empty()) {
        cmd << " --cpus=" << config_.resource_limits.cpu;
    }
    
    if (!config_.resource_limits.memory.empty()) {
        cmd << " --memory=" << config_.resource_limits.memory;
    }
    
    // 网络配置
    if (config_.network_mode == NetworkMode::None) {
        cmd << " --network=none";
    } else if (config_.network_mode == NetworkMode::Host) {
        cmd << " --network=host";
    }
    
    // 环境变量
    for (const auto& [key, value] : config_.env) {
        cmd << " -e " << key << "=" << value;
    }
    
    // 卷挂载
    for (const auto& vol : config_.volumes) {
        if (vol.has_host()) {
            cmd << " -v " << vol.host.path << ":" << vol.mount_path;
            if (vol.read_only) {
                cmd << ":ro";
            }
        }
    }
    
    // 工作目录
    cmd << " -w " << config_.workdir;
    
    // 镜像和启动命令 (保持容器运行)
    cmd << " " << config_.image;
    cmd << " tail -f /dev/null";  // 保持容器运行
    
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

// 执行命令
SandboxResult DockerSandbox::execute_command(
    const std::string& command,
    std::function<void(const StreamEvent&)> event_callback) {
    
    if (!initialized_) {
        SandboxResult result;
        result.success = false;
        result.stderr_output = "Sandbox not initialized";
        return result;
    }
    
    std::string exec_cmd = "docker exec " + container_id_ + " sh -c '" + command + "'";
    return execute_shell_command(exec_cmd);
}

// 中断命令
bool DockerSandbox::interrupt_command(const std::string& command_id) {
    return interrupt_code(command_id);
}

// 获取后台命令状态
SandboxResult DockerSandbox::get_background_command_status(const std::string& command_id) {
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
        return result;
    }
    
    return execute_command(exec_cmd, event_callback);
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
