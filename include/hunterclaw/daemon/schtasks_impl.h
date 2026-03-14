#pragma once

#include "hunterclaw/daemon/daemon_service.h"

#include <string>

namespace hunterclaw {
namespace daemon {

// Windows Scheduled Task implementation (100% aligned with OpenClaw schtasks.ts)
class ScheduledTaskService : public DaemonService {
 public:
  ScheduledTaskService() = default;
  ~ScheduledTaskService() override = default;

  // Create scheduled task using schtasks.exe /Create
  void Install(const InstallConfig& config) override;

  // Delete scheduled task using schtasks.exe /Delete
  void Uninstall(const ManageConfig& config) override;

  // End scheduled task using schtasks.exe /End
  void Stop(const ControlConfig& config) override;

  // Run scheduled task using schtasks.exe /Run
  void Restart(const ControlConfig& config) override;

  // Check if scheduled task exists using schtasks.exe /Query
  bool IsLoaded(const EnvConfig& config) override;

  // Read command from scheduled task XML
  std::optional<CommandConfig> ReadCommand(
      const std::unordered_map<std::string, std::string>& env) override;

  // Read runtime status from schtasks.exe /Query /V
  ServiceRuntime ReadRuntime(
      const std::unordered_map<std::string, std::string>& env) override;

 private:
  // Resolve scheduled task name (e.g., "OpenClaw\\Gateway")
  std::string ResolveScheduledTaskName(
      const std::unordered_map<std::string, std::string>& env) const;

  // Build scheduled task XML content
  std::string BuildScheduledTaskXml(const InstallConfig& config) const;

  // Execute schtasks.exe command
  struct SchtasksResult {
    std::string stdout_output;
    std::string stderr_output;
    int exit_code;
  };
  SchtasksResult ExecSchtasks(const std::vector<std::string>& args) const;

  // Parse schtasks /Query /V /FO CSV output
  struct SchtasksQueryInfo {
    std::string status;  // "Running", "Ready", "Disabled"
    int pid = -1;
    std::string last_result;
    std::string next_run_time;
  };
  SchtasksQueryInfo ParseSchtasksQuery(const std::string& output) const;
};

}  // namespace daemon
}  // namespace hunterclaw
