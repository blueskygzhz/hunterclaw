#ifndef HUNTERCLAW_DAEMON_DAEMON_SERVICE_H_
#define HUNTERCLAW_DAEMON_DAEMON_SERVICE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <system_error>

// Use std types instead of abseil
namespace absl {
    using Status = std::error_code;
    template<typename T>
    using StatusOr = std::optional<T>;
}

namespace hunterclaw::daemon {

// 跨平台服务管理器
// 100% 对齐 OpenClaw src/daemon/service.ts

enum class Platform {
  kMacOS,   // LaunchAgent
  kLinux,   // systemd
  kWindows  // Scheduled Task
};

enum class ServiceState {
  kRunning,
  kStopped,
  kFailed,
  kUnknown
};

struct ServiceConfig {
  std::string service_name;
  std::string description;
  std::vector<std::string> program_arguments;
  std::string working_directory;
  std::map<std::string, std::string> environment;
  std::string stdout_path;
  std::string stderr_path;
  std::string profile;  // e.g. "default", "dev", "prod"
};

struct ServiceStatus {
  ServiceState state;
  int pid = -1;
  int last_exit_code = 0;
  std::string last_exit_reason;
  std::string state_text;
};

// 服务抽象基类
// 对齐 OpenClaw GatewayService interface
class DaemonService {
 public:
  virtual ~DaemonService() = default;

  // 服务生命周期管理
  virtual absl::Status Install(const ServiceConfig& config) = 0;
  virtual absl::Status Uninstall(const std::string& profile = "") = 0;
  virtual absl::Status Start(const std::string& profile = "") = 0;
  virtual absl::Status Stop(const std::string& profile = "") = 0;
  virtual absl::Status Restart(const std::string& profile = "") = 0;

  // 服务状态查询
  virtual absl::StatusOr<bool> IsLoaded(const std::string& profile = "") = 0;
  virtual absl::StatusOr<ServiceStatus> GetStatus(const std::string& profile = "") = 0;

  // 服务配置读取
  virtual absl::StatusOr<ServiceConfig> ReadConfig(const std::string& profile = "") = 0;

  // 获取服务标签 (对齐 OpenClaw label 字段)
  virtual std::string GetLabel() const = 0;
  virtual std::string GetLoadedText() const = 0;
  virtual std::string GetNotLoadedText() const = 0;
};

// 工厂函数 - 根据平台创建对应的服务管理器
// 对齐 OpenClaw resolveGatewayService()
std::unique_ptr<DaemonService> CreateDaemonService(Platform platform);

// 自动检测平台并创建服务管理器
std::unique_ptr<DaemonService> CreateDaemonServiceForCurrentPlatform();

// 平台检测
Platform DetectPlatform();

// 服务名称生成器 (对齐 OpenClaw constants.ts)
std::string ResolveServiceName(Platform platform, const std::string& profile = "");
std::string ResolveServiceDescription(const std::string& profile = "", 
                                     const std::string& version = "");

}  // namespace hunterclaw::daemon

#endif  // HUNTERCLAW_DAEMON_DAEMON_SERVICE_H_
