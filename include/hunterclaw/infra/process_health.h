#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <chrono>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>

namespace hunterclaw {
namespace infra {

// 健康状态
struct HealthStatus {
    bool is_healthy = false;
    uint32_t pid = 0;
    uint64_t uptime_ms = 0;
    float cpu_percent = 0.0f;
    uint64_t memory_mb = 0;
    std::string status_message;
    
    // 额外指标
    uint64_t thread_count = 0;
    uint64_t fd_count = 0;
    std::chrono::system_clock::time_point last_check;
};

// 心跳回调
using HeartbeatCallback = std::function<void(const HealthStatus&)>;

// 心跳配置
struct HeartbeatConfig {
    std::string service_name;
    std::chrono::milliseconds interval{5000};  // 5s
    HeartbeatCallback on_healthy;
    HeartbeatCallback on_unhealthy;
    
    // 健康阈值
    float max_cpu_percent = 80.0f;
    uint64_t max_memory_mb = 1024;  // 1GB
};

class ProcessHealthChecker {
public:
    // 健康检查 (单次)
    static Result<HealthStatus> Check(uint32_t pid);
    
    // 健康检查 (按服务名)
    static Result<HealthStatus> CheckByService(const std::string& service_name);
    
    // 启动心跳监控 (后台线程)
    static Result<void> StartHeartbeat(const HeartbeatConfig& config);
    
    // 停止心跳监控
    static void StopHeartbeat();
    
    // 是否正在监控
    static bool IsHeartbeatRunning();
    
private:
    class HeartbeatWorker;
    static std::unique_ptr<HeartbeatWorker> worker_;
    
    // 平台相关实现
    static Result<HealthStatus> CheckLinux(uint32_t pid);
    static Result<HealthStatus> CheckMacOS(uint32_t pid);
    static Result<HealthStatus> CheckWindows(uint32_t pid);
    
    // 解析 /proc/[pid]/stat (Linux)
    static Result<HealthStatus> ParseProcStat(uint32_t pid);
    
    // 使用 ps (macOS)
    static Result<HealthStatus> ParsePsOutput(uint32_t pid);
};

// 心跳工作线程
class ProcessHealthChecker::HeartbeatWorker {
public:
    explicit HeartbeatWorker(const HeartbeatConfig& config);
    ~HeartbeatWorker();
    
    void Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }
    
private:
    void Run();
    
    HeartbeatConfig config_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> should_stop_{false};
};

}  // namespace infra
}  // namespace hunterclaw
