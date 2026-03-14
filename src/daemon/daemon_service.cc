#include "hunterclaw/daemon/daemon_service.h"

#include <memory>
#include <stdexcept>

#ifdef __APPLE__
#include "hunterclaw/daemon/launchd_impl.h"
#elif defined(__linux__)
#include "hunterclaw/daemon/systemd_impl.h"
#elif defined(_WIN32)
#include "hunterclaw/daemon/schtasks_impl.h"
#endif

namespace hunterclaw {
namespace daemon {

// Factory function to create platform-specific daemon service
std::unique_ptr<DaemonService> DaemonService::Create() {
#ifdef __APPLE__
  return std::make_unique<LaunchdService>();
#elif defined(__linux__)
  return std::make_unique<SystemdService>();
#elif defined(_WIN32)
  return std::make_unique<ScheduledTaskService>();
#else
  throw std::runtime_error(
      "DaemonService not supported on this platform");
#endif
}

std::string DaemonService::GetPlatformLabel() const {
#ifdef __APPLE__
  return "LaunchAgent";
#elif defined(__linux__)
  return "systemd";
#elif defined(_WIN32)
  return "Scheduled Task";
#else
  return "unknown";
#endif
}

}  // namespace daemon
}  // namespace hunterclaw
