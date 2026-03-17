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

// ============================================================================
// Platform Detection
// ============================================================================

Platform DetectPlatform() {
#ifdef __APPLE__
  return Platform::kMacOS;
#elif defined(__linux__)
  return Platform::kLinux;
#elif defined(_WIN32)
  return Platform::kWindows;
#else
  throw std::runtime_error("Unsupported platform");
#endif
}

// ============================================================================
// Factory Functions
// ============================================================================

std::unique_ptr<DaemonService> CreateDaemonService(Platform platform) {
  switch (platform) {
#ifdef __APPLE__
    case Platform::kMacOS:
      return std::make_unique<LaunchdService>();
#endif
#ifdef __linux__
    case Platform::kLinux:
      return std::make_unique<SystemdService>();
#endif
#ifdef _WIN32
    case Platform::kWindows:
      return std::make_unique<ScheduledTaskService>();
#endif
    default:
      throw std::runtime_error("Platform not supported in this build");
  }
}

std::unique_ptr<DaemonService> CreateDaemonServiceForCurrentPlatform() {
  return CreateDaemonService(DetectPlatform());
}

// ============================================================================
// Service Name Generators
// ============================================================================

std::string ResolveServiceName(Platform platform, const std::string& profile) {
  std::string base_name = "hunterclaw-gateway";
  if (!profile.empty() && profile != "default") {
    base_name += "-" + profile;
  }
  return base_name;
}

std::string ResolveServiceDescription(const std::string& profile, 
                                     const std::string& version) {
  std::string desc = "HunterClaw Gateway";
  if (!profile.empty() && profile != "default") {
    desc += " (" + profile + ")";
  }
  if (!version.empty()) {
    desc += " v" + version;
  }
  return desc;
}

}  // namespace daemon
}  // namespace hunterclaw
