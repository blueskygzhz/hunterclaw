#include <gtest/gtest.h>
#include "hunterclaw/cli/daemon_commands.h"
#include "hunterclaw/daemon/daemon_service.h"
#include "hunterclaw/core/config_loader.h"
#include "hunterclaw/infra/instance_lock.h"
#include "hunterclaw/infra/process_health.h"
#include <chrono>
#include <thread>

using namespace hunterclaw;

// ==================== Daemon 生命周期测试 ====================

TEST(DaemonIntegration, FullLifecycle) {
    // 1. 安装 Daemon
    std::vector<std::string> install_args = {"--port", "18789", "--json"};
    auto install_cmd = cli::DaemonInstallCommand();
    auto install_result = install_cmd.Execute(install_args);
    
    ASSERT_TRUE(install_result.is_ok()) << "Install failed: " << install_result.error;
    
    // 2. 检查服务已加载
    auto service = daemon::DaemonService::GetPlatformService();
    ASSERT_TRUE(service);
    
    auto is_loaded = service->IsLoaded();
    ASSERT_TRUE(is_loaded.is_ok() && is_loaded.unwrap()) << "Service not loaded after install";
    
    // 3. 启动 Daemon
    std::vector<std::string> start_args = {"--json"};
    auto start_cmd = cli::DaemonStartCommand();
    auto start_result = start_cmd.Execute(start_args);
    
    ASSERT_TRUE(start_result.is_ok()) << "Start failed: " << start_result.error;
    
    // 等待启动
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // 4. 检查状态 (应该正在运行)
    auto status_cmd = cli::DaemonStatusCommand();
    auto status_result = status_cmd.Execute({"--json"});
    
    ASSERT_TRUE(status_result.is_ok()) << "Status check failed";
    
    // 解析 JSON 输出
    auto json = nlohmann::json::parse(status_result.unwrap());
    ASSERT_EQ(json["runtime"]["status"], "running") << "Service not running";
    ASSERT_GT(json["runtime"]["pid"], 0) << "Invalid PID";
    
    // 5. 重启 Daemon
    auto restart_cmd = cli::DaemonRestartCommand();
    auto restart_result = restart_cmd.Execute({"--json"});
    
    ASSERT_TRUE(restart_result.is_ok()) << "Restart failed: " << restart_result.error;
    
    // 等待重启
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // 6. 检查 PID 变化
    auto status_result2 = status_cmd.Execute({"--json"});
    ASSERT_TRUE(status_result2.is_ok());
    
    auto json2 = nlohmann::json::parse(status_result2.unwrap());
    ASSERT_NE(json["runtime"]["pid"], json2["runtime"]["pid"]) << "PID should change after restart";
    
    // 7. 停止 Daemon
    auto stop_cmd = cli::DaemonStopCommand();
    auto stop_result = stop_cmd.Execute({"--json"});
    
    ASSERT_TRUE(stop_result.is_ok()) << "Stop failed: " << stop_result.error;
    
    // 等待停止
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // 8. 检查状态 (应该已停止)
    auto status_result3 = status_cmd.Execute({"--json"});
    ASSERT_TRUE(status_result3.is_ok());
    
    auto json3 = nlohmann::json::parse(status_result3.unwrap());
    ASSERT_EQ(json3["runtime"]["status"], "stopped") << "Service still running";
    
    // 9. 卸载 Daemon
    auto uninstall_cmd = cli::DaemonUninstallCommand();
    auto uninstall_result = uninstall_cmd.Execute({"--json"});
    
    ASSERT_TRUE(uninstall_result.is_ok()) << "Uninstall failed: " << uninstall_result.error;
    
    // 10. 检查服务已卸载
    auto is_loaded2 = service->IsLoaded();
    ASSERT_TRUE(is_loaded2.is_ok() && !is_loaded2.unwrap()) << "Service still loaded after uninstall";
}

// ==================== 配置管理测试 ====================

TEST(ConfigIntegration, LoadAndMerge) {
    // 1. 创建测试配置文件
    core::Config test_config;
    test_config.gateway.port = 19999;
    test_config.gateway.token = "test-token-123";
    test_config.security.exec_policy = "full";
    
    std::string config_path = "/tmp/hunterclaw_test_config.json";
    auto save_result = core::ConfigLoader::SaveToFile(test_config, config_path);
    ASSERT_TRUE(save_result.is_ok()) << "Failed to save config";
    
    // 2. 加载配置
    core::LoadOptions opts;
    opts.config_file = config_path;
    
    auto load_result = core::ConfigLoader::Load(opts);
    ASSERT_TRUE(load_result.is_ok()) << "Failed to load config: " << load_result.error;
    
    auto loaded_config = load_result.unwrap();
    ASSERT_EQ(loaded_config.gateway.port, 19999);
    ASSERT_EQ(loaded_config.gateway.token, "test-token-123");
    ASSERT_EQ(loaded_config.security.exec_policy, "full");
    
    // 3. CLI 参数覆盖
    opts.cli_port = 20000;
    opts.cli_token = "cli-override";
    
    auto load_result2 = core::ConfigLoader::Load(opts);
    ASSERT_TRUE(load_result2.is_ok());
    
    auto overridden_config = load_result2.unwrap();
    ASSERT_EQ(overridden_config.gateway.port, 20000) << "CLI should override file";
    ASSERT_EQ(overridden_config.gateway.token, "cli-override");
    
    // 4. 清理
    unlink(config_path.c_str());
}

// ==================== 单实例锁测试 ====================

TEST(InstanceLockIntegration, SingleInstance) {
    // 1. 获取锁
    auto lock1 = infra::InstanceLock::Acquire("hunterclaw-test");
    ASSERT_TRUE(lock1.is_ok()) << "Failed to acquire lock";
    
    // 2. 检查锁状态
    ASSERT_TRUE(infra::InstanceLock::IsLocked("hunterclaw-test"));
    
    // 3. 尝试再次获取 (应该失败)
    auto lock2 = infra::InstanceLock::TryAcquire("hunterclaw-test");
    ASSERT_TRUE(lock2.is_err()) << "Should not acquire lock twice";
    
    // 4. 获取锁持有者 PID
    auto owner = infra::InstanceLock::GetLockOwner("hunterclaw-test");
    ASSERT_TRUE(owner.is_ok());
    ASSERT_EQ(owner.unwrap(), getpid());
    
    // 5. 释放锁 (通过析构)
    lock1.unwrap() = infra::InstanceLock(std::move(infra::InstanceLock::TryAcquire("another-lock").unwrap()));
    
    // 6. 再次尝试获取 (应该成功)
    auto lock3 = infra::InstanceLock::TryAcquire("hunterclaw-test");
    ASSERT_TRUE(lock3.is_ok()) << "Should acquire lock after release";
}

// ==================== 健康检查测试 ====================

TEST(ProcessHealthIntegration, HealthCheck) {
    // 1. 检查当前进程
    auto health = infra::ProcessHealthChecker::Check(getpid());
    ASSERT_TRUE(health.is_ok()) << "Health check failed";
    
    auto status = health.unwrap();
    ASSERT_EQ(status.pid, getpid());
    ASSERT_TRUE(status.is_healthy);
    ASSERT_GT(status.memory_mb, 0) << "Memory usage should be > 0";
    
    // 2. 启动心跳监控
    bool healthy_callback_called = false;
    
    infra::HeartbeatConfig config;
    config.service_name = "hunterclaw-test";
    config.interval = std::chrono::milliseconds(1000);
    config.on_healthy = [&](const infra::HealthStatus& s) {
        healthy_callback_called = true;
    };
    
    auto start_result = infra::ProcessHealthChecker::StartHeartbeat(config);
    ASSERT_TRUE(start_result.is_ok()) << "Failed to start heartbeat";
    
    // 等待回调
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    ASSERT_TRUE(healthy_callback_called) << "Heartbeat callback not called";
    
    // 3. 停止心跳
    infra::ProcessHealthChecker::StopHeartbeat();
    ASSERT_FALSE(infra::ProcessHealthChecker::IsHeartbeatRunning());
}

// ==================== 性能测试 ====================

TEST(PerformanceIntegration, ConfigLoadSpeed) {
    // 测试配置加载性能 (应该 < 10ms)
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < 100; i++) {
        auto config = core::ConfigLoader::Load();
        ASSERT_TRUE(config.is_ok());
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    ASSERT_LT(duration.count(), 1000) << "100 config loads should take < 1s";
}

TEST(PerformanceIntegration, InstanceLockSpeed) {
    // 测试单实例锁性能 (应该 < 1ms)
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < 1000; i++) {
        std::string lock_name = "test-lock-" + std::to_string(i);
        auto lock = infra::InstanceLock::Acquire(lock_name);
        ASSERT_TRUE(lock.is_ok());
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    ASSERT_LT(duration.count(), 1000) << "1000 lock acquisitions should take < 1s";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
