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

  // Install systemd user service unit file and enable
  void Install(const InstallConfig& config) override;

  // Disable and remove systemd user service
  void Uninstall(const ManageConfig& config) override;

  // Stop systemd user service (systemctl --user stop)
  void Stop(const ControlConfig& config) override;

  // Restart systemd user service (systemctl --user restart)
  void Restart(const ControlConfig& config) override;

  // Check if systemd service is enabled
  bool IsLoaded(const EnvConfig& config) override;

  // Read command from systemd unit ExecStart
  std::optional<CommandConfig> ReadCommand(
      const std::unordered_map<std::string, std::string>& env) override;

  // Read runtime status from systemctl --user show
  ServiceRuntime ReadRuntime(
      const std::unordered_map<std::string, std::string>& env) override;

 private:
  // Resolve systemd service name (e.g., "openclaw-gateway")
  std::string ResolveSystemdServiceName(
      const std::unordered_map<std::string, std::string>& env) const;

  // Resolve systemd unit file path
  // (e.g., "~/.config/systemd/user/openclaw-gateway.service")
  std::string ResolveSystemdUnitPath(
      const std::unordered_map<std::string, std::string>& env) const;

  // Build systemd unit file content
  std::string BuildSystemdUnit(const InstallConfig& config) const;

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
