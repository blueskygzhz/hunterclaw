#include "hunterclaw/daemon/systemd_impl.h"

#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace daemon {

namespace {

// Default service name (aligned with OpenClaw systemd.ts:9)
constexpr const char* DEFAULT_SERVICE_NAME = "openclaw-gateway";

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

// Parse key=value output (aligned with OpenClaw runtime-parse.ts)
std::unordered_map<std::string, std::string> ParseKeyValueOutput(
    const std::string& output, const std::string& delimiter) {
  std::unordered_map<std::string, std::string> result;
  std::istringstream stream(output);
  std::string line;

  while (std::getline(stream, line)) {
    auto pos = line.find(delimiter);
    if (pos == std::string::npos) continue;

    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + delimiter.length());

    // Trim whitespace
    key.erase(0, key.find_first_not_of(" \t\r\n"));
    key.erase(key.find_last_not_of(" \t\r\n") + 1);
    value.erase(0, value.find_first_not_of(" \t\r\n"));
    value.erase(value.find_last_not_of(" \t\r\n") + 1);

    // Convert key to lowercase
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    result[key] = value;
  }

  return result;
}

// Shell escape for systemd ExecStart
std::string ShellEscape(const std::string& arg) {
  if (arg.empty()) {
    return "''";
  }
  
  // Check if needs quoting
  bool needs_quote = false;
  for (char c : arg) {
    if (!std::isalnum(c) && c != '/' && c != '_' && c != '-' && c != '.') {
      needs_quote = true;
      break;
    }
  }
  
  if (!needs_quote) {
    return arg;
  }
  
  // Quote and escape
  std::string result = "'";
  for (char c : arg) {
    if (c == '\'') {
      result += "'\\''";
    } else {
      result += c;
    }
  }
  result += "'";
  return result;
}

// Build systemd unit file content
// (aligned with OpenClaw systemd.ts:476-481 + systemd-unit.ts)
std::string BuildSystemdUnitContent(
    const std::string& description,
    const std::vector<std::string>& program_arguments,
    const std::string& working_directory,
    const std::unordered_map<std::string, std::string>& environment) {
  
  std::ostringstream unit;
  
  // [Unit] section
  unit << "[Unit]\n"
       << "Description=" << description << "\n"
       << "After=network.target\n"
       << "\n";
  
  // [Service] section
  unit << "[Service]\n"
       << "Type=simple\n"
       << "Restart=always\n"
       << "RestartSec=5\n";
  
  // ExecStart
  unit << "ExecStart=";
  for (size_t i = 0; i < program_arguments.size(); ++i) {
    if (i > 0) unit << " ";
    unit << ShellEscape(program_arguments[i]);
  }
  unit << "\n";
  
  // WorkingDirectory
  if (!working_directory.empty()) {
    unit << "WorkingDirectory=" << working_directory << "\n";
  }
  
  // Environment variables
  for (const auto& [key, value] : environment) {
    unit << "Environment=\"" << key << "=" << value << "\"\n";
  }
  
  // StandardOutput and StandardError
  unit << "StandardOutput=journal\n"
       << "StandardError=journal\n"
       << "SyslogIdentifier=hunterclaw-gateway\n"
       << "\n";
  
  // [Install] section
  unit << "[Install]\n"
       << "WantedBy=default.target\n";
  
  return unit.str();
}

}  // namespace

// Resolve systemd service name (aligned with OpenClaw systemd.ts:40-46)
std::string SystemdService::ResolveSystemdServiceName(
    const std::unordered_map<std::string, std::string>& env) const {
  auto it = env.find("OPENCLAW_SYSTEMD_UNIT");
  if (it != env.end() && !it->second.empty()) {
    std::string name = it->second;
    if (name.size() > 8 && name.substr(name.size() - 8) == ".service") {
      return name.substr(0, name.size() - 8);
    }
    return name;
  }
  
  // Default service name (can be customized by OPENCLAW_PROFILE)
  auto profile_it = env.find("OPENCLAW_PROFILE");
  if (profile_it != env.end() && !profile_it->second.empty()) {
    return std::string(DEFAULT_SERVICE_NAME) + "-" + profile_it->second;
  }
  
  return DEFAULT_SERVICE_NAME;
}

// Resolve systemd unit path (aligned with OpenClaw systemd.ts:35-50)
std::string SystemdService::ResolveSystemdUnitPath(
    const std::unordered_map<std::string, std::string>& env) const {
  const char* home = std::getenv("HOME");
  if (!home) {
    throw std::runtime_error("HOME environment variable not set");
  }

  std::string service_name = ResolveSystemdServiceName(env);
  return std::string(home) + "/.config/systemd/user/" + service_name + ".service";
}

// Execute systemctl (aligned with OpenClaw systemd.ts:254-258)
SystemdService::SystemctlResult SystemdService::ExecSystemctl(
    const std::vector<std::string>& args) const {
  std::string cmd = "systemctl --user";
  for (const auto& arg : args) {
    cmd += " " + arg;
  }

  auto [output, exit_code] = ExecuteCommand(cmd);
  
  return {output, "", exit_code};
}

// Parse systemctl show (aligned with OpenClaw systemd.ts:222-252)
SystemdService::SystemctlShowInfo SystemdService::ParseSystemctlShow(
    const std::string& output) const {
  auto entries = ParseKeyValueOutput(output, "=");
  
  SystemctlShowInfo info;
  
  if (entries.count("activestate")) {
    info.active_state = entries["activestate"];
  }
  
  if (entries.count("substate")) {
    info.sub_state = entries["substate"];
  }
  
  if (entries.count("mainpid")) {
    try {
      info.main_pid = std::stoi(entries["mainpid"]);
    } catch (...) {
      info.main_pid = -1;
    }
  }
  
  if (entries.count("execmainstatus")) {
    try {
      info.exit_code = std::stoi(entries["execmainstatus"]);
    } catch (...) {
      info.exit_code = 0;
    }
  }
  
  return info;
}

// Enable systemd user linger (aligned with OpenClaw systemd-linger.ts)
void SystemdService::EnableUserLinger() const {
  auto res = ExecuteCommand("loginctl enable-linger");
  // Ignore errors - best effort
}

// Install systemd service (aligned with OpenClaw systemd.ts:451-521)
void SystemdService::Install(const InstallConfig& config) {
  std::string unit_path = ResolveSystemdUnitPath(config.env);
  
  // Create directory
  fs::create_directories(fs::path(unit_path).parent_path());

  // Backup existing unit file
  if (fs::exists(unit_path)) {
    std::string backup_path = unit_path + ".bak";
    try {
      fs::copy_file(unit_path, backup_path, fs::copy_options::overwrite_existing);
      config.stdout.write("[i] Backed up existing unit to " + backup_path + "\n");
    } catch (...) {
      // Ignore backup failure
    }
  }

  // Build unit content
  std::string unit_content = BuildSystemdUnitContent(
      config.description,
      config.program_arguments,
      config.working_directory,
      config.environment);

  // Write unit file
  std::ofstream unit_file(unit_path);
  if (!unit_file) {
    throw std::runtime_error("Failed to write unit file: " + unit_path);
  }
  unit_file << unit_content;
  unit_file.close();

  // Reload daemon
  auto reload = ExecSystemctl({"daemon-reload"});
  if (reload.exit_code != 0) {
    throw std::runtime_error("systemctl daemon-reload failed: " + reload.stdout_output);
  }

  // Enable service
  std::string service_name = ResolveSystemdServiceName(config.env);
  std::string unit_name = service_name + ".service";
  
  auto enable = ExecSystemctl({"enable", unit_name});
  if (enable.exit_code != 0) {
    throw std::runtime_error("systemctl enable failed: " + enable.stdout_output);
  }

  // Start/restart service
  auto restart = ExecSystemctl({"restart", unit_name});
  if (restart.exit_code != 0) {
    throw std::runtime_error("systemctl restart failed: " + restart.stdout_output);
  }

  // Enable user linger (best effort)
  EnableUserLinger();

  config.stdout.write("\n[✓] Installed systemd service: " + unit_path + "\n");
}

// Uninstall systemd service (aligned with OpenClaw systemd.ts:523-539)
void SystemdService::Uninstall(const ManageConfig& config) {
  std::string service_name = ResolveSystemdServiceName(config.env);
  std::string unit_name = service_name + ".service";
  std::string unit_path = ResolveSystemdUnitPath(config.env);

  // Disable and stop
  ExecSystemctl({"disable", "--now", unit_name});

  // Remove unit file
  if (fs::exists(unit_path)) {
    try {
      fs::remove(unit_path);
      config.stdout.write("[✓] Removed systemd service: " + unit_path + "\n");
    } catch (...) {
      config.stdout.write("[!] Failed to remove unit file: " + unit_path + "\n");
    }
  } else {
    config.stdout.write("[!] Systemd service not found at " + unit_path + "\n");
  }
}

// Stop systemd service (aligned with OpenClaw systemd.ts:558-568)
void SystemdService::Stop(const ControlConfig& config) {
  std::string service_name = ResolveSystemdServiceName(config.env);
  std::string unit_name = service_name + ".service";

  auto res = ExecSystemctl({"stop", unit_name});
  if (res.exit_code != 0) {
    throw std::runtime_error("systemctl stop failed: " + res.stdout_output);
  }

  config.stdout.write("[✓] Stopped systemd service: " + unit_name + "\n");
}

// Restart systemd service (aligned with OpenClaw systemd.ts:570-580)
void SystemdService::Restart(const ControlConfig& config) {
  std::string service_name = ResolveSystemdServiceName(config.env);
  std::string unit_name = service_name + ".service";

  auto res = ExecSystemctl({"restart", unit_name});
  if (res.exit_code != 0) {
    throw std::runtime_error("systemctl restart failed: " + res.stdout_output);
  }

  config.stdout.write("[✓] Restarted systemd service: " + unit_name + "\n");
}

// Check if enabled (aligned with OpenClaw systemd.ts:582-604)
bool SystemdService::IsLoaded(const EnvConfig& config) {
  std::string unit_path = ResolveSystemdUnitPath(config.env);
  
  // Check if unit file exists
  if (!fs::exists(unit_path)) {
    return false;
  }

  std::string service_name = ResolveSystemdServiceName(config.env);
  std::string unit_name = service_name + ".service";

  auto res = ExecSystemctl({"is-enabled", unit_name});
  if (res.exit_code == 0) {
    return true;
  }

  // Check if unit is disabled (not missing)
  std::string output_lower = res.stdout_output;
  std::transform(output_lower.begin(), output_lower.end(), 
                 output_lower.begin(), ::tolower);
  
  if (output_lower.find("disabled") != std::string::npos ||
      output_lower.find("static") != std::string::npos ||
      output_lower.find("indirect") != std::string::npos) {
    return false;
  }

  return false;
}

// Read command from unit (aligned with OpenClaw systemd.ts:61-122)
std::optional<DaemonService::CommandConfig> SystemdService::ReadCommand(
    const std::unordered_map<std::string, std::string>& env) {
  std::string unit_path = ResolveSystemdUnitPath(env);
  
  if (!fs::exists(unit_path)) {
    return std::nullopt;
  }

  std::ifstream file(unit_path);
  if (!file) {
    return std::nullopt;
  }

  CommandConfig config;
  std::string line;
  
  while (std::getline(file, line)) {
    // Trim whitespace
    line.erase(0, line.find_first_not_of(" \t\r\n"));
    line.erase(line.find_last_not_of(" \t\r\n") + 1);
    
    if (line.empty() || line[0] == '#') continue;
    
    if (line.substr(0, 10) == "ExecStart=") {
      std::string exec_start = line.substr(10);
      // Simple argument splitting (real implementation should handle shell quoting)
      std::istringstream iss(exec_start);
      std::string arg;
      while (iss >> arg) {
        config.program_arguments.push_back(arg);
      }
    } else if (line.substr(0, 17) == "WorkingDirectory=") {
      config.working_directory = line.substr(17);
    } else if (line.substr(0, 12) == "Environment=") {
      std::string env_line = line.substr(12);
      // Parse "KEY=VALUE" or "KEY=\"VALUE\""
      size_t eq_pos = env_line.find('=');
      if (eq_pos != std::string::npos) {
        std::string key = env_line.substr(0, eq_pos);
        std::string value = env_line.substr(eq_pos + 1);
        
        // Remove quotes if present
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
          value = value.substr(1, value.size() - 2);
        }
        
        config.environment[key] = value;
      }
    }
  }

  if (config.program_arguments.empty()) {
    return std::nullopt;
  }

  return config;
}

// Read runtime status (aligned with OpenClaw systemd.ts:606-646)
DaemonService::ServiceRuntime SystemdService::ReadRuntime(
    const std::unordered_map<std::string, std::string>& env) {
  std::string service_name = ResolveSystemdServiceName(env);
  std::string unit_name = service_name + ".service";

  auto res = ExecSystemctl({
      "show", unit_name, "--no-page", "--property",
      "ActiveState,SubState,MainPID,ExecMainStatus,ExecMainCode"});

  ServiceRuntime runtime;

  if (res.exit_code != 0) {
    std::string detail = res.stdout_output;
    std::string detail_lower = detail;
    std::transform(detail_lower.begin(), detail_lower.end(), 
                   detail_lower.begin(), ::tolower);
    
    bool missing = detail_lower.find("not found") != std::string::npos;
    
    runtime.status = missing ? "stopped" : "unknown";
    runtime.detail = detail;
    runtime.missing_unit = missing;
    return runtime;
  }

  auto parsed = ParseSystemctlShow(res.stdout_output);

  std::string active_state_lower = parsed.active_state;
  std::transform(active_state_lower.begin(), active_state_lower.end(),
                 active_state_lower.begin(), ::tolower);

  if (active_state_lower == "active") {
    runtime.status = "running";
  } else if (!active_state_lower.empty()) {
    runtime.status = "stopped";
  } else {
    runtime.status = "unknown";
  }

  runtime.state = parsed.active_state;
  runtime.sub_state = parsed.sub_state;
  runtime.pid = parsed.main_pid;
  runtime.last_exit_status = parsed.exit_code;

  return runtime;
}

}  // namespace daemon
}  // namespace hunterclaw
