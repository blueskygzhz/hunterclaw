#pragma once

#include "hunterclaw/daemon/daemon_service.h"

#include <string>

namespace hunterclaw {
namespace daemon {

// macOS LaunchAgent implementation (100% aligned with OpenClaw launchd.ts)
class LaunchdService : public DaemonService {
 public:
  LaunchdService() = default;
  ~LaunchdService() override = default;

  // Install LaunchAgent plist and load service
  void Install(const InstallConfig& config) override;

  // Uninstall LaunchAgent plist and unload service
  void Uninstall(const ManageConfig& config) override;

  // Stop LaunchAgent (launchctl stop)
  void Stop(const ControlConfig& config) override;

  // Restart LaunchAgent (launchctl kickstart)
  void Restart(const ControlConfig& config) override;

  // Check if LaunchAgent is loaded (launchctl print)
  bool IsLoaded(const EnvConfig& config) override;

  // Read command from LaunchAgent plist ProgramArguments
  std::optional<CommandConfig> ReadCommand(
      const std::unordered_map<std::string, std::string>& env) override;

  // Read runtime status from launchctl print
  ServiceRuntime ReadRuntime(
      const std::unordered_map<std::string, std::string>& env) override;

 private:
  // Resolve LaunchAgent label (e.g., "ai.openclaw.gateway")
  std::string ResolveLaunchAgentLabel(
      const std::unordered_map<std::string, std::string>& env) const;

  // Resolve LaunchAgent plist path
  // (e.g., "~/Library/LaunchAgents/ai.openclaw.gateway.plist")
  std::string ResolveLaunchAgentPlistPath(
      const std::unordered_map<std::string, std::string>& env) const;

  // Build LaunchAgent plist XML content
  std::string BuildLaunchAgentPlist(const InstallConfig& config) const;

  // Execute launchctl command
  struct LaunchctlResult {
    std::string stdout_output;
    std::string stderr_output;
    int exit_code;
  };
  LaunchctlResult ExecLaunchctl(const std::vector<std::string>& args) const;

  // Parse launchctl print output
  struct LaunchctlPrintInfo {
    std::string state;
    int pid = -1;
    int last_exit_status = 0;
    std::string last_exit_reason;
  };
  LaunchctlPrintInfo ParseLaunchctlPrint(const std::string& output) const;
};

}  // namespace daemon
}  // namespace hunterclaw
