#include "hunterclaw/infra/process_health.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif

namespace hunterclaw {
namespace infra {

std::unique_ptr<ProcessHealthChecker::HeartbeatWorker> ProcessHealthChecker::worker_;

// ==================== 公共接口 ====================

core::Result<HealthStatus> ProcessHealthChecker::Check(uint32_t pid) {
#ifdef __linux__
    return CheckLinux(pid);
#elif defined(__APPLE__)
    return CheckMacOS(pid);
#elif defined(_WIN32)
    return CheckWindows(pid);
#else
    return core::Result<HealthStatus>::Err("Unsupported platform");
#endif
}

core::Result<HealthStatus> ProcessHealthChecker::CheckByService(const std::string& service_name) {
    // 这里需要与 daemon 服务集成,读取 PID
    // 简化实现:从 /var/run/hunterclaw.pid 读取
    std::string pid_file = "/var/run/" + service_name + ".pid";
    std::ifstream file(pid_file);
    if (!file.is_open()) {
        return core::Result<HealthStatus>::Err("Service not running or PID file not found");
    }
    
    uint32_t pid;
    file >> pid;
    file.close();
    
    return Check(pid);
}

core::Result<void> ProcessHealthChecker::StartHeartbeat(const HeartbeatConfig& config) {
    if (IsHeartbeatRunning()) {
        return core::Result<void>::Err("Heartbeat already running");
    }
    
    worker_ = std::make_unique<HeartbeatWorker>(config);
    worker_->Start();
    
    return core::Result<void>::Ok();
}

void ProcessHealthChecker::StopHeartbeat() {
    if (worker_) {
        worker_->Stop();
        worker_.reset();
    }
}

bool ProcessHealthChecker::IsHeartbeatRunning() {
    return worker_ && worker_->IsRunning();
}

// ==================== Linux 实现 ====================

#ifdef __linux__
core::Result<HealthStatus> ProcessHealthChecker::CheckLinux(uint32_t pid) {
    return ParseProcStat(pid);
}

core::Result<HealthStatus> ProcessHealthChecker::ParseProcStat(uint32_t pid) {
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream stat_file(stat_path);
    
    if (!stat_file.is_open()) {
        return core::Result<HealthStatus>::Err("Process not found or no access");
    }
    
    HealthStatus status;
    status.pid = pid;
    status.is_healthy = true;
    status.last_check = std::chrono::system_clock::now();
    
    // 解析 /proc/[pid]/stat
    std::string line;
    std::getline(stat_file, line);
    
    // 简化解析 (实际应该用更健壮的解析器)
    std::istringstream iss(line);
    std::string pid_str, comm, state;
    unsigned long utime, stime;
    long rss;
    
    iss >> pid_str >> comm >> state;
    for (int i = 0; i < 10; i++) iss >> pid_str;  // skip fields
    iss >> utime >> stime;
    for (int i = 0; i < 7; i++) iss >> pid_str;   // skip more fields
    iss >> rss;
    
    // 计算 CPU 使用率 (简化)
    status.cpu_percent = 0.0f;  // 需要两次采样才能计算
    
    // 内存使用 (RSS in pages)
    long page_size = sysconf(_SC_PAGESIZE);
    status.memory_mb = (rss * page_size) / (1024 * 1024);
    
    // 读取启动时间
    std::string stat_path_uptime = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream uptime_file("/proc/uptime");
    if (uptime_file.is_open()) {
        double uptime;
        uptime_file >> uptime;
        status.uptime_ms = static_cast<uint64_t>(uptime * 1000);
    }
    
    // 读取线程数
    std::string status_path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream status_file(status_path);
    if (status_file.is_open()) {
        std::string line;
        while (std::getline(status_file, line)) {
            if (line.find("Threads:") == 0) {
                sscanf(line.c_str(), "Threads: %lu", &status.thread_count);
                break;
            }
        }
    }
    
    // 读取 fd 数量
    std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
    status.fd_count = 0;  // 需要遍历目录计数
    
    status.status_message = "healthy";
    return core::Result<HealthStatus>::Ok(status);
}
#endif

// ==================== macOS 实现 ====================

#ifdef __APPLE__
core::Result<HealthStatus> ProcessHealthChecker::CheckMacOS(uint32_t pid) {
    return ParsePsOutput(pid);
}

core::Result<HealthStatus> ProcessHealthChecker::ParsePsOutput(uint32_t pid) {
    // 使用 ps 命令
    std::string cmd = "ps -p " + std::to_string(pid) + " -o pid,vsz,rss,%cpu,etime";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return core::Result<HealthStatus>::Err("Failed to execute ps command");
    }
    
    HealthStatus status;
    status.pid = pid;
    status.is_healthy = true;
    status.last_check = std::chrono::system_clock::now();
    
    char buffer[256];
    bool found = false;
    
    // Skip header
    fgets(buffer, sizeof(buffer), pipe);
    
    // Parse output
    if (fgets(buffer, sizeof(buffer), pipe)) {
        unsigned long vsz, rss;
        float cpu;
        char etime[64];
        
        if (sscanf(buffer, "%*d %lu %lu %f %s", &vsz, &rss, &cpu, etime) == 4) {
            status.memory_mb = rss / 1024;  // KB to MB
            status.cpu_percent = cpu;
            status.status_message = "healthy";
            found = true;
        }
    }
    
    pclose(pipe);
    
    if (!found) {
        return core::Result<HealthStatus>::Err("Process not found");
    }
    
    return core::Result<HealthStatus>::Ok(status);
}
#endif

// ==================== Windows 实现 (Stub) ====================

#ifdef _WIN32
core::Result<HealthStatus> ProcessHealthChecker::CheckWindows(uint32_t pid) {
    // TODO: Windows implementation using Performance Counters or WMI
    return core::Result<HealthStatus>::Err("Windows health check not implemented yet");
}
#endif

// ==================== HeartbeatWorker ====================

ProcessHealthChecker::HeartbeatWorker::HeartbeatWorker(const HeartbeatConfig& config)
    : config_(config) {}

ProcessHealthChecker::HeartbeatWorker::~HeartbeatWorker() {
    Stop();
}

void ProcessHealthChecker::HeartbeatWorker::Start() {
    if (running_.load()) return;
    
    running_.store(true);
    should_stop_.store(false);
    
    thread_ = std::thread(&HeartbeatWorker::Run, this);
}

void ProcessHealthChecker::HeartbeatWorker::Stop() {
    if (!running_.load()) return;
    
    should_stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
}

void ProcessHealthChecker::HeartbeatWorker::Run() {
    while (!should_stop_.load()) {
        // 执行健康检查
        auto check_result = ProcessHealthChecker::CheckByService(config_.service_name);
        
        if (check_result.is_ok()) {
            auto status = check_result.unwrap();
            
            // 判断健康状态
            bool is_healthy = true;
            if (status.cpu_percent > config_.max_cpu_percent) {
                is_healthy = false;
                status.status_message = "CPU usage too high";
            }
            if (status.memory_mb > config_.max_memory_mb) {
                is_healthy = false;
                status.status_message = "Memory usage too high";
            }
            
            // 调用回调
            if (is_healthy && config_.on_healthy) {
                config_.on_healthy(status);
            } else if (!is_healthy && config_.on_unhealthy) {
                config_.on_unhealthy(status);
            }
        }
        
        // 等待下一次检查
        auto sleep_duration = config_.interval;
        auto sleep_until = std::chrono::steady_clock::now() + sleep_duration;
        
        while (!should_stop_.load() && std::chrono::steady_clock::now() < sleep_until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace infra
}  // namespace hunterclaw
