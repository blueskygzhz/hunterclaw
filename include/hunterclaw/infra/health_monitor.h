#pragma once

#include "hunterclaw/core/result.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace hunterclaw {
namespace infra {

// 健康状态
struct HealthStatus {
    bool is_healthy = true;
    uint32_t pid = 0;
    uint64_t uptime_ms = 0;
    float cpu_percent = 0.0f;
    uint64_t memory_mb = 0;
    uint64_t thread_count = 0;
    uint64_t fd_count = 0;
    std::string error_message;
    
    core::JsonValue to_json() const;
};

// 健康检查配置
struct HealthMonitorConfig {
    std::chrono::milliseconds check_interval{5000};  // 5秒
    float max_cpu_percent = 80.0f;
    uint64_t max_memory_mb = 1024;
    uint64_t max_thread_count = 100;
    uint64_t max_fd_count = 1000;
};

// 进程健康检查器
class ProcessHealthChecker {
public:
    static core::Result<HealthStatus> check(uint32_t pid);
    static core::Result<HealthStatus> check_current();
    
private:
    static float get_cpu_usage(uint32_t pid);
    static uint64_t get_memory_usage_mb(uint32_t pid);
    static uint64_t get_thread_count(uint32_t pid);
    static uint64_t get_fd_count(uint32_t pid);
};

// 健康监控器
class HealthMonitor {
public:
    using HealthyCallback = std::function<void(const HealthStatus&)>;
    using UnhealthyCallback = std::function<void(const HealthStatus&)>;
    
    explicit HealthMonitor(const HealthMonitorConfig& config);
    ~HealthMonitor();
    
    void start();
    void stop();
    
    void set_healthy_callback(HealthyCallback callback) { on_healthy_ = callback; }
    void set_unhealthy_callback(UnhealthyCallback callback) { on_unhealthy_ = callback; }
    
    std::optional<HealthStatus> latest_status() const;
    
private:
    void monitor_worker();
    bool is_healthy(const HealthStatus& status);
    
    HealthMonitorConfig config_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> thread_;
    
    HealthyCallback on_healthy_;
    UnhealthyCallback on_unhealthy_;
    
    mutable std::mutex mutex_;
    std::optional<HealthStatus> latest_status_;
};

}  // namespace infra
}  // namespace hunterclaw
