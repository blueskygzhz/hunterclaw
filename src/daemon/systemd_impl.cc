#include "hunterclaw/daemon/systemd_impl.h"

#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace daemon {

namespace {

// Default service name
constexpr const char* DEFAULT_SERVICE_NAME = "hunterclaw-gateway";

// Execute command and return output
std::pair<std::string, int> ExecuteCommand(const std::string& cmd) {
  std::array<char, 128> buffer;
  std::string result;
  int exit_code = -1;

  FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
  if (!pipe) {
    return {result, exit_code};
  }

  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result += buffer.data();
  }

  exit_code = pclose(pipe);
  return {result, WEXITSTATUS(exit_code)};
}

}  // namespace

// ============================================================================
// SystemdService Implementation
// ============================================================================

std::string SystemdService::GetLabel() const {
  return "systemd";
}

std::string SystemdService::GetLoadedText() const {
  return "enabled";
}

std::string SystemdService::GetNotLoadedText() const {
  return "disabled";
}

std::string SystemdService::ResolveSystemdServiceName(const std::string& profile) const {
  std::string name = DEFAULT_SERVICE_NAME;
  if (!profile.empty() && profile != "default") {
    name += "-" + profile;
  }
  return name;
}

std::string SystemdService::ResolveSystemdUnitPath(const std::string& profile) const {
  std::string service_name = ResolveSystemdServiceName(profile);
  
  // Get home directory
  const char* home = getenv("HOME");
  if (!home) {
    struct passwd* pw = getpwuid(getuid());
    if (pw) {
      home = pw->pw_dir;
    }
  }
  
  std::string unit_path = std::string(home) + "/.config/systemd/user/" + 
                          service_name + ".service";
  return unit_path;
}

std::string SystemdService::BuildSystemdUnit(const ServiceConfig& config) const {
  std::ostringstream ss;
  
  ss << "[Unit]\n";
  ss << "Description=" << config.description << "\n";
  ss << "After=network.target\n\n";
  
  ss << "[Service]\n";
  ss << "Type=simple\n";
  
  // ExecStart
  ss << "ExecStart=";
  for (size_t i = 0; i < config.program_arguments.size(); ++i) {
    if (i > 0) ss << " ";
    ss << config.program_arguments[i];
  }
  ss << "\n";
  
  // WorkingDirectory
  if (!config.working_directory.empty()) {
    ss << "WorkingDirectory=" << config.working_directory << "\n";
  }
  
  // Environment variables
  for (const auto& [key, value] : config.environment) {
    ss << "Environment=\"" << key << "=" << value << "\"\n";
  }
  
  // Stdout/Stderr
  if (!config.stdout_path.empty()) {
    ss << "StandardOutput=file:" << config.stdout_path << "\n";
  }
  if (!config.stderr_path.empty()) {
    ss << "StandardError=file:" << config.stderr_path << "\n";
  }
  
  ss << "Restart=always\n";
  ss << "RestartSec=10\n\n";
  
  ss << "[Install]\n";
  ss << "WantedBy=default.target\n";
  
  return ss.str();
}

SystemdService::SystemctlResult SystemdService::ExecSystemctl(
    const std::vector<std::string>& args) const {
  std::string cmd = "systemctl --user";
  for (const auto& arg : args) {
    cmd += " " + arg;
  }
  
  auto [output, exit_code] = ExecuteCommand(cmd);
  return {output, "", exit_code};
}

SystemdService::SystemctlShowInfo SystemdService::ParseSystemctlShow(
    const std::string& output) const {
  SystemctlShowInfo info;
  std::istringstream stream(output);
  std::string line;
  
  while (std::getline(stream, line)) {
    auto pos = line.find('=');
    if (pos != std::string::npos) {
      std::string key = line.substr(0, pos);
      std::string value = line.substr(pos + 1);
      
      if (key == "ActiveState") {
        info.active_state = value;
      } else if (key == "SubState") {
        info.sub_state = value;
      } else if (key == "MainPID") {
        info.main_pid = std::stoi(value);
      } else if (key == "ExecMainStatus") {
        info.exit_code = std::stoi(value);
      }
    }
  }
  
  return info;
}

void SystemdService::EnableUserLinger() const {
  ExecuteCommand("loginctl enable-linger");
}

// ============================================================================
// Public API Implementation
// ============================================================================

absl::Status SystemdService::Install(const ServiceConfig& config) {
  std::string unit_path = ResolveSystemdUnitPath(config.profile);
  std::string unit_content = BuildSystemdUnit(config);
  
  // Create directory
  fs::path dir_path = fs::path(unit_path).parent_path();
  std::error_code ec;
  fs::create_directories(dir_path, ec);
  if (ec) {
    return absl::Status{};  // Error creating directory
  }
  
  // Write unit file
  std::ofstream ofs(unit_path);
  if (!ofs) {
    return absl::Status{};  // Error opening file
  }
  ofs << unit_content;
  ofs.close();
  
  // Enable user linger
  EnableUserLinger();
  
  // Reload systemd and enable service
  ExecSystemctl({"daemon-reload"});
  std::string service_name = ResolveSystemdServiceName(config.profile);
  ExecSystemctl({"enable", service_name + ".service"});
  
  return absl::Status{};  // Success
}

absl::Status SystemdService::Uninstall(const std::string& profile) {
  std::string service_name = ResolveSystemdServiceName(profile);
  std::string unit_path = ResolveSystemdUnitPath(profile);
  
  // Stop and disable service
  ExecSystemctl({"stop", service_name + ".service"});
  ExecSystemctl({"disable", service_name + ".service"});
  
  // Remove unit file
  std::error_code ec;
  fs::remove(unit_path, ec);
  
  // Reload systemd
  ExecSystemctl({"daemon-reload"});
  
  return absl::Status{};  // Success
}

absl::Status SystemdService::Start(const std::string& profile) {
  std::string service_name = ResolveSystemdServiceName(profile);
  auto result = ExecSystemctl({"start", service_name + ".service"});
  
  if (result.exit_code != 0) {
    return absl::Status{};  // Error
  }
  
  return absl::Status{};  // Success
}

absl::Status SystemdService::Stop(const std::string& profile) {
  std::string service_name = ResolveSystemdServiceName(profile);
  auto result = ExecSystemctl({"stop", service_name + ".service"});
  
  if (result.exit_code != 0) {
    return absl::Status{};  // Error
  }
  
  return absl::Status{};  // Success
}

absl::Status SystemdService::Restart(const std::string& profile) {
  std::string service_name = ResolveSystemdServiceName(profile);
  auto result = ExecSystemctl({"restart", service_name + ".service"});
  
  if (result.exit_code != 0) {
    return absl::Status{};  // Error
  }
  
  return absl::Status{};  // Success
}

absl::StatusOr<bool> SystemdService::IsLoaded(const std::string& profile) {
  std::string service_name = ResolveSystemdServiceName(profile);
  auto result = ExecSystemctl({"is-enabled", service_name + ".service"});
  
  // is-enabled returns 0 if enabled
  return result.exit_code == 0;
}

absl::StatusOr<ServiceStatus> SystemdService::GetStatus(const std::string& profile) {
  std::string service_name = ResolveSystemdServiceName(profile);
  auto result = ExecSystemctl({
      "show", service_name + ".service",
      "--property=ActiveState,SubState,MainPID,ExecMainStatus"
  });
  
  auto info = ParseSystemctlShow(result.stdout_output);
  
  ServiceStatus status;
  status.pid = info.main_pid;
  status.last_exit_code = info.exit_code;
  status.state_text = info.active_state + "/" + info.sub_state;
  
  if (info.active_state == "active" && info.sub_state == "running") {
    status.state = ServiceState::kRunning;
  } else if (info.active_state == "inactive") {
    status.state = ServiceState::kStopped;
  } else if (info.active_state == "failed") {
    status.state = ServiceState::kFailed;
  } else {
    status.state = ServiceState::kUnknown;
  }
  
  return status;
}

absl::StatusOr<ServiceConfig> SystemdService::ReadConfig(const std::string& profile) {
  std::string unit_path = ResolveSystemdUnitPath(profile);
  
  std::ifstream ifs(unit_path);
  if (!ifs) {
    return std::nullopt;  // File not found
  }
  
  ServiceConfig config;
  config.service_name = ResolveSystemdServiceName(profile);
  config.profile = profile;
  
  // Parse unit file (simplified - just read ExecStart)
  std::string line;
  while (std::getline(ifs, line)) {
    if (line.find("ExecStart=") == 0) {
      std::string exec_start = line.substr(10);  // Skip "ExecStart="
      
      // Split by space (simplified parsing)
      std::istringstream ss(exec_start);
      std::string arg;
      while (ss >> arg) {
        config.program_arguments.push_back(arg);
      }
    } else if (line.find("WorkingDirectory=") == 0) {
      config.working_directory = line.substr(17);
    } else if (line.find("Description=") == 0) {
      config.description = line.substr(12);
    }
  }
  
  return config;
}

}  // namespace daemon
}  // namespace hunterclaw
