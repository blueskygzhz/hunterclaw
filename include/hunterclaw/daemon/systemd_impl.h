#pragma once

#include "hunterclaw/daemon/daemon_service.h"

#include <string>

namespace hunterclaw {
namespace daemon {

// Linux systemd user service implementation (100% aligned with OpenClaw systemd.ts)
class SystemdService : public DaemonService {
 public:
  SystemdService() = default;
  ~SystemdService() override = default;

  // ✅ 对齐基类接口
  absl::Status Install(const ServiceConfig& config) override;
  absl::Status Uninstall(const std::string& profile = "") override;
  absl::Status Start(const std::string& profile = "") override;
  absl::Status Stop(const std::string& profile = "") override;
  absl::Status Restart(const std::string& profile = "") override;
  
  absl::StatusOr<bool> IsLoaded(const std::string& profile = "") override;
  absl::StatusOr<ServiceStatus> GetStatus(const std::string& profile = "") override;
  absl::StatusOr<ServiceConfig> ReadConfig(const std::string& profile = "") override;
  
  std::string GetLabel() const override;
  std::string GetLoadedText() const override;
  std::string GetNotLoadedText() const override;

 private:
  // Resolve systemd service name (e.g., "hunterclaw-gateway")
  std::string ResolveSystemdServiceName(const std::string& profile = "") const;

  // Resolve systemd unit file path
  // (e.g., "~/.config/systemd/user/hunterclaw-gateway.service")
  std::string ResolveSystemdUnitPath(const std::string& profile = "") const;

  // Build systemd unit file content
  std::string BuildSystemdUnit(const ServiceConfig& config) const;

  // Execute systemctl command
  struct SystemctlResult {
    std::string stdout_output;
    std::string stderr_output;
    int exit_code;
  };
  SystemctlResult ExecSystemctl(const std::vector<std::string>& args) const;

  // Parse systemctl show output
  struct SystemctlShowInfo {
    std::string active_state;  // "active", "inactive", "failed"
    std::string sub_state;     // "running", "exited", "dead"
    int main_pid = -1;
    int exit_code = 0;
  };
  SystemctlShowInfo ParseSystemctlShow(const std::string& output) const;

  // Enable systemd user linger (loginctl enable-linger)
  void EnableUserLinger() const;
};

}  // namespace daemon
}  // namespace hunterclaw
