#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>
#include <functional>

namespace hunterclaw {
namespace sandbox {

// 沙箱类型
enum class SandboxType {
    None,      // 不使用沙箱
    Docker,    // Docker容器
    Podman,    // Podman容器
    Process    // 进程隔离
};

// 沙箱状态 (对齐OpenClaw)
enum class SandboxState {
    Pending,      // 待创建
    Running,      // 运行中
    Paused,       // 已暂停
    Stopping,     // 停止中
    Terminated,   // 已终止
    Failed        // 失败
};

// 网络模式 (对齐OpenClaw)
enum class NetworkMode {
    Bridge,    // 桥接模式
    Host,      // 主机模式
    None,      // 无网络
    Container  // 容器网络命名空间
};

// 网络策略规则
struct NetworkRule {
    std::string action;  // "allow" | "deny"
    std::string target;  // FQDN或通配符域名
};

// 网络策略 (对齐OpenClaw)
struct NetworkPolicy {
    std::string default_action = "deny";  // "allow" | "deny"
    std::vector<NetworkRule> egress;      // 出站规则
};

// 卷挂载 (对齐OpenClaw)
struct Volume {
    std::string name;
    std::string mount_path;
    bool read_only = false;
    std::string sub_path;
    
    // 后端 (二选一)
    struct {
        std::string path;  // 主机路径
    } host;
    
    struct {
        std::string claim_name;  // PVC名称
    } pvc;
    
    bool has_host() const { return !host.path.empty(); }
    bool has_pvc() const { return !pvc.claim_name.empty(); }
};

// 资源限制 (对齐OpenClaw)
struct ResourceLimits {
    std::string cpu = "1000m";      // CPU限制 (e.g., "500m", "2")
    std::string memory = "512Mi";   // 内存限制 (e.g., "512Mi", "1Gi")
    std::string gpu;                // GPU限制 (可选)
    int64_t disk_bytes = 0;         // 磁盘限制 (字节)
};

// 沙箱配置 (严格对齐OpenClaw)
struct SandboxConfig {
    SandboxType type = SandboxType::None;
    
    // 镜像配置
    std::string image = "ubuntu:22.04";
    std::vector<std::string> entrypoint;  // 入口命令 (必须非空)
    
    // 环境变量
    std::map<std::string, std::string> env;
    
    // 元数据 (用于过滤和标签)
    std::map<std::string, std::string> metadata;
    
    // 资源限制
    ResourceLimits resource_limits;
    int timeout_seconds = 3600;   // 执行超时 (60-86400秒)
    
    // 网络配置
    NetworkMode network_mode = NetworkMode::Bridge;
    NetworkPolicy network_policy;
    
    // 卷挂载
    std::vector<Volume> volumes;
    
    // 工作目录
    std::string workdir = "/workspace";
    
    // 安全配置 (对齐OpenClaw)
    std::vector<std::string> drop_capabilities = {
        "AUDIT_WRITE", "MKNOD", "NET_ADMIN", "NET_RAW",
        "SYS_ADMIN", "SYS_MODULE", "SYS_PTRACE", "SYS_TIME", "SYS_TTY_CONFIG"
    };
    bool no_new_privileges = true;
    int pids_limit = 512;
    std::string apparmor_profile;
    std::string seccomp_profile;
    
    // 过期时间 (对齐OpenClaw)
    std::chrono::system_clock::time_point expires_at;
    std::chrono::system_clock::time_point created_at;
};

// 资源使用情况 (对齐OpenClaw execd)
struct ResourceUsage {
    double cpu_percent = 0.0;
    double memory_total_gb = 0.0;
    double memory_used_gb = 0.0;
    double memory_percent = 0.0;
    double disk_total_gb = 0.0;
    double disk_used_gb = 0.0;
    double disk_percent = 0.0;
    int64_t uptime_seconds = 0;
    std::string timestamp;
};

// 流式事件 (对齐OpenClaw execd)
struct StreamEvent {
    std::string type;  // "output" | "result" | "error" | "done" | "started"
    std::map<std::string, std::string> data;
    std::string timestamp;
};

// 沙箱执行结果
struct SandboxResult {
    bool success = false;
    std::string stdout_output;
    std::string stderr_output;
    int exit_code = 0;
    int64_t execution_time_ms = 0;
    ResourceUsage resource_usage;
    
    // 后台命令ID (可选)
    std::string command_id;
};

// 沙箱状态 (对齐OpenClaw)
struct SandboxStatus {
    SandboxState state = SandboxState::Pending;
    std::string reason;
    std::string message;
    std::chrono::system_clock::time_point last_transition_at;
};

// 沙箱完整信息 (对齐OpenClaw API)
struct SandboxInfo {
    std::string id;
    std::string image;
    SandboxStatus status;
    std::map<std::string, std::string> metadata;
    std::vector<std::string> entrypoint;
    std::chrono::system_clock::time_point expires_at;
    std::chrono::system_clock::time_point created_at;
};

// Sandbox接口 (对齐OpenClaw Lifecycle API + execd)
class Sandbox {
public:
    virtual ~Sandbox() = default;
    
    // ========== 生命周期管理 (对齐OpenClaw Lifecycle API) ==========
    
    // 初始化沙箱
    virtual bool initialize() = 0;
    
    // 暂停沙箱
    virtual bool pause() = 0;
    
    // 恢复沙箱
    virtual bool resume() = 0;
    
    // 清理沙箱
    virtual void cleanup() = 0;
    
    // 获取沙箱状态
    virtual SandboxStatus get_status() const = 0;
    
    // 续期
    virtual bool renew_expiration(const std::chrono::system_clock::time_point& new_expires_at) = 0;
    
    // ========== 命令执行 (对齐OpenClaw execd /command) ==========
    
    // 执行命令 (同步,流式输出)
    virtual SandboxResult execute_command(
        const std::string& command,
        std::function<void(const StreamEvent&)> event_callback = nullptr
    ) = 0;
    
    // 执行后台命令 (异步,立即返回ID)
    virtual SandboxResult execute_background_command(const std::string& command) = 0;
    
    // 中断命令
    virtual bool interrupt_command(const std::string& command_id) = 0;
    
    // 获取后台命令状态
    virtual SandboxResult get_background_command_status(const std::string& command_id) = 0;
    
    // 获取后台命令日志
    virtual std::string get_background_command_logs(const std::string& command_id) = 0;
    
    // ========== 代码执行 (对齐OpenClaw execd /code) ==========
    
    // 执行代码 (Jupyter Kernel)
    virtual SandboxResult execute_code(
        const std::string& language,
        const std::string& code,
        std::function<void(const StreamEvent&)> event_callback = nullptr
    ) = 0;
    
    // 中断代码执行
    virtual bool interrupt_code(const std::string& kernel_id) = 0;
    
    // ========== 文件操作 (对齐OpenClaw execd /files) ==========
    
    // 获取文件信息
    virtual std::map<std::string, std::string> get_file_info(const std::string& path) = 0;
    
    // 列出文件
    virtual std::vector<std::string> list_files(const std::string& path, bool recursive = false) = 0;
    
    // 读取文件
    virtual std::string read_file(const std::string& path) = 0;
    
    // 写入文件
    virtual bool write_file(const std::string& path, const std::string& content, bool append = false) = 0;
    
    // 删除文件
    virtual bool delete_files(const std::vector<std::string>& paths) = 0;
    
    // 搜索文件
    virtual std::vector<std::string> search_files(const std::string& pattern) = 0;
    
    // 复制文件 (主机 <-> 容器)
    virtual bool copy_to_sandbox(const std::string& host_path, const std::string& sandbox_path) = 0;
    virtual bool copy_from_sandbox(const std::string& sandbox_path, const std::string& host_path) = 0;
    
    // ========== 监控 (对齐OpenClaw execd /metrics) ==========
    
    // 获取资源指标
    virtual ResourceUsage get_metrics() = 0;
    
    // 监听指标 (SSE流)
    virtual void watch_metrics(std::function<void(const ResourceUsage&)> callback) = 0;
    
    // ========== 访问端点 (对齐OpenClaw) ==========
    
    // 获取访问端点
    virtual std::string get_endpoint(int port = 44772, bool resolve_internal = false) = 0;
    
    // 检查是否运行中
    virtual bool is_running() const = 0;
};

// Docker Sandbox实现
class DockerSandbox : public Sandbox {
public:
    explicit DockerSandbox(const SandboxConfig& config);
    
    bool initialize() override;
    SandboxResult execute_command(
        const std::string& command,
        std::function<void(const StreamEvent&)> event_callback = nullptr
    ) override;
    SandboxResult execute_background_command(const std::string& command) override;
    bool interrupt_command(const std::string& command_id) override;
    SandboxResult get_background_command_status(const std::string& command_id) override;
    std::string get_background_command_logs(const std::string& command_id) override;
    
    SandboxResult execute_code(
        const std::string& language,
        const std::string& code,
        std::function<void(const StreamEvent&)> event_callback = nullptr
    ) override;
    bool interrupt_code(const std::string& kernel_id) override;
    
    std::map<std::string, std::string> get_file_info(const std::string& path) override;
    std::vector<std::string> list_files(const std::string& path, bool recursive = false) override;
    std::string read_file(const std::string& path) override;
    bool write_file(const std::string& path, const std::string& content, bool append = false) override;
    bool delete_files(const std::vector<std::string>& paths) override;
    std::vector<std::string> search_files(const std::string& pattern) override;
    
    bool copy_to_sandbox(const std::string& host_path, const std::string& sandbox_path) override;
    bool copy_from_sandbox(const std::string& sandbox_path, const std::string& host_path) override;
    
    ResourceUsage get_metrics() override;
    void watch_metrics(std::function<void(const ResourceUsage&)> callback) override;
    
    std::string get_endpoint(int port = 44772, bool resolve_internal = false) override;
    void cleanup() override;
    bool is_running() const override;
    
    // 生命周期管理 (纯虚函数实现)
    bool pause() override;
    bool resume() override;
    SandboxStatus get_status() const override;
    bool renew_expiration(const std::chrono::system_clock::time_point& new_expires_at) override;

private:
    SandboxConfig config_;
    std::string container_id_;
    bool initialized_ = false;
    
    std::string build_docker_run_command();
    SandboxResult execute_docker_command(const std::string& command);
    void apply_network_policy();  // 应用网络策略 (iptables)
};

// Process Sandbox实现 (简化版,使用cgroups)
class ProcessSandbox : public Sandbox {
public:
    explicit ProcessSandbox(const SandboxConfig& config);
    
    bool initialize() override;
    SandboxResult execute_command(
        const std::string& command,
        std::function<void(const StreamEvent&)> event_callback = nullptr
    ) override;
    SandboxResult execute_background_command(const std::string& command) override;
    bool interrupt_command(const std::string& command_id) override;
    SandboxResult get_background_command_status(const std::string& command_id) override;
    std::string get_background_command_logs(const std::string& command_id) override;
    
    SandboxResult execute_code(
        const std::string& language,
        const std::string& code,
        std::function<void(const StreamEvent&)> event_callback = nullptr
    ) override;
    bool interrupt_code(const std::string& kernel_id) override;
    
    std::map<std::string, std::string> get_file_info(const std::string& path) override;
    std::vector<std::string> list_files(const std::string& path, bool recursive = false) override;
    std::string read_file(const std::string& path) override;
    bool write_file(const std::string& path, const std::string& content, bool append = false) override;
    bool delete_files(const std::vector<std::string>& paths) override;
    std::vector<std::string> search_files(const std::string& pattern) override;
    
    bool copy_to_sandbox(const std::string& host_path, const std::string& sandbox_path) override;
    bool copy_from_sandbox(const std::string& sandbox_path, const std::string& host_path) override;
    
    ResourceUsage get_metrics() override;
    void watch_metrics(std::function<void(const ResourceUsage&)> callback) override;
    
    std::string get_endpoint(int port = 44772, bool resolve_internal = false) override;
    void cleanup() override;
    bool is_running() const override;

private:
    SandboxConfig config_;
    std::string temp_dir_;
    bool initialized_ = false;
};

// Sandbox Manager (对齐OpenClaw SandboxService)
class SandboxManager {
public:
    static std::shared_ptr<SandboxManager> create();
    ~SandboxManager();
    
    // ========== 生命周期API (对齐OpenClaw) ==========
    
    // 创建沙箱
    SandboxInfo create_sandbox(const SandboxConfig& config);
    
    // 列出所有沙箱
    std::vector<SandboxInfo> list_sandboxes(const std::map<std::string, std::string>& filters = {}) const;
    
    // 获取指定沙箱
    SandboxInfo get_sandbox(const std::string& sandbox_id) const;
    
    // 获取沙箱实例（用于执行命令）
    std::shared_ptr<Sandbox> get_sandbox_instance(const std::string& sandbox_id) const;
    
    // 删除沙箱
    void delete_sandbox(const std::string& sandbox_id);
    
    // 暂停沙箱
    bool pause_sandbox(const std::string& sandbox_id);
    
    // 恢复沙箱
    bool resume_sandbox(const std::string& sandbox_id);
    
    // 续期沙箱
    bool renew_expiration(const std::string& sandbox_id, 
                         const std::chrono::system_clock::time_point& new_expires_at);
    
    // 获取访问端点
    std::string get_endpoint(const std::string& sandbox_id, int port = 44772, 
                           bool resolve_internal = false);
    
    // ========== 运行时检测 ==========
    
    static bool is_docker_available();
    static bool is_podman_available();
    
    // ========== 启动时恢复 (对齐OpenClaw) ==========
    
    void restore_existing_sandboxes();
    
private:
    SandboxManager();
    
    std::map<std::string, std::shared_ptr<Sandbox>> sandboxes_;
    std::map<std::string, std::chrono::system_clock::time_point> sandbox_expirations_;
    
    // 辅助函数
    std::string generate_sandbox_id();
    void schedule_sandbox_expiration(const std::string& sandbox_id, 
                                     const std::chrono::system_clock::time_point& expires_at);
    void expire_sandbox(const std::string& sandbox_id);
};

} // namespace sandbox
} // namespace hunterclaw
