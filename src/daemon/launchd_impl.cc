#include "hunterclaw/daemon/launchd_impl.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>  // ✅ 添加缺失的头文件

namespace fs = std::filesystem;

namespace hunterclaw {
namespace daemon {

namespace {

// Constants (aligned with OpenClaw launchd.ts:28-29)
constexpr mode_t LAUNCH_AGENT_DIR_MODE = 0755;
constexpr mode_t LAUNCH_AGENT_PLIST_MODE = 0644;
constexpr const char* DEFAULT_LAUNCH_AGENT_LABEL = "ai.openclaw.gateway";

// Resolve GUI domain (aligned with OpenClaw launchd.ts:111-116)
std::string ResolveGuiDomain() {
  return "gui/" + std::to_string(getuid());
}

// Execute shell command and return output
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

// Ensure directory exists with secure permissions
// (aligned with OpenClaw launchd.ts:118-130)
void EnsureSecureDirectory(const std::string& path) {
  fs::create_directories(path);
  
  // Tighten permissions: remove group/other write
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    mode_t current_mode = st.st_mode & 0777;
    mode_t tightened_mode = current_mode & ~0022;  // Remove group/other write
    if (tightened_mode != current_mode) {
      chmod(path.c_str(), tightened_mode);
    }
  }
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

    // Convert key to lowercase for case-insensitive matching
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    result[key] = value;
  }

  return result;
}

// Build LaunchAgent plist XML
// (aligned with OpenClaw launchd.ts:74-100 + launchd-plist.ts)
std::string BuildLaunchAgentPlistXml(
    const std::string& label,
    const std::string& comment,
    const std::vector<std::string>& program_arguments,
    const std::string& working_directory,
    const std::string& stdout_path,
    const std::string& stderr_path,
    const std::unordered_map<std::string, std::string>& environment) {
  
  std::ostringstream xml;
  xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\">\n"
      << "<dict>\n"
      << "  <key>Label</key>\n"
      << "  <string>" << label << "</string>\n";

  if (!comment.empty()) {
    xml << "  <key>Comment</key>\n"
        << "  <string>" << comment << "</string>\n";
  }

  xml << "  <key>ProgramArguments</key>\n"
      << "  <array>\n";
  for (const auto& arg : program_arguments) {
    xml << "    <string>" << arg << "</string>\n";
  }
  xml << "  </array>\n";

  if (!working_directory.empty()) {
    xml << "  <key>WorkingDirectory</key>\n"
        << "  <string>" << working_directory << "</string>\n";
  }

  xml << "  <key>StandardOutPath</key>\n"
      << "  <string>" << stdout_path << "</string>\n"
      << "  <key>StandardErrorPath</key>\n"
      << "  <string>" << stderr_path << "</string>\n";

  if (!environment.empty()) {
    xml << "  <key>EnvironmentVariables</key>\n"
        << "  <dict>\n";
    for (const auto& [key, value] : environment) {
      xml << "    <key>" << key << "</key>\n"
          << "    <string>" << value << "</string>\n";
    }
    xml << "  </dict>\n";
  }

  xml << "  <key>RunAtLoad</key>\n"
      << "  <true/>\n"
      << "  <key>KeepAlive</key>\n"
      << "  <true/>\n"
      << "  <key>ProcessType</key>\n"
      << "  <string>Interactive</string>\n"
      << "</dict>\n"
      << "</plist>\n";

  return xml.str();
}

}  // namespace

// Resolve LaunchAgent label (aligned with OpenClaw launchd.ts:31-37)
std::string LaunchdService::ResolveLaunchAgentLabel(
    const std::unordered_map<std::string, std::string>& env) const {
  auto it = env.find("OPENCLAW_LAUNCHD_LABEL");
  if (it != env.end() && !it->second.empty()) {
    return it->second;
  }
  
  // Default label (can be customized by OPENCLAW_PROFILE)
  auto profile_it = env.find("OPENCLAW_PROFILE");
  if (profile_it != env.end() && !profile_it->second.empty()) {
    return "ai.openclaw.gateway." + profile_it->second;
  }
  
  return DEFAULT_LAUNCH_AGENT_LABEL;
}

// Resolve LaunchAgent plist path (aligned with OpenClaw launchd.ts:39-50)
std::string LaunchdService::ResolveLaunchAgentPlistPath(
    const std::unordered_map<std::string, std::string>& env) const {
  const char* home = std::getenv("HOME");
  if (!home) {
    throw std::runtime_error("HOME environment variable not set");
  }

  std::string label = ResolveLaunchAgentLabel(env);
  return std::string(home) + "/Library/LaunchAgents/" + label + ".plist";
}

// Execute launchctl (aligned with OpenClaw launchd.ts:102-109)
LaunchdService::LaunchctlResult LaunchdService::ExecLaunchctl(
    const std::vector<std::string>& args) const {
  std::string cmd = "launchctl";
  for (const auto& arg : args) {
    cmd += " " + arg;
  }

  auto [output, exit_code] = ExecuteCommand(cmd);
  
  return {output, "", exit_code};
}

// Parse launchctl print (aligned with OpenClaw launchd.ts:139-165)
LaunchdService::LaunchctlPrintInfo LaunchdService::ParseLaunchctlPrint(
    const std::string& output) const {
  auto entries = ParseKeyValueOutput(output, "=");
  
  LaunchctlPrintInfo info;
  
  if (entries.count("state")) {
    info.state = entries["state"];
  }
  
  if (entries.count("pid")) {
    try {
      info.pid = std::stoi(entries["pid"]);
    } catch (...) {
      info.pid = -1;
    }
  }
  
  if (entries.count("last exit status")) {
    try {
      info.last_exit_status = std::stoi(entries["last exit status"]);
    } catch (...) {
      info.last_exit_status = 0;
    }
  }
  
  if (entries.count("last exit reason")) {
    info.last_exit_reason = entries["last exit reason"];
  }
  
  return info;
}

// Install LaunchAgent (aligned with OpenClaw launchd.ts:393-469)
void LaunchdService::Install(const InstallConfig& config) {
  const char* home = std::getenv("HOME");
  if (!home) {
    throw std::runtime_error("HOME environment variable not set");
  }

  std::string domain = ResolveGuiDomain();
  std::string label = ResolveLaunchAgentLabel(config.env);
  std::string plist_path = ResolveLaunchAgentPlistPath(config.env);

  // Resolve log paths (aligned with OpenClaw launchd.ts:52-65)
  std::string state_dir = std::string(home) + "/.hunterclaw/state";
  std::string log_dir = state_dir + "/logs";
  std::string stdout_path = log_dir + "/gateway.log";
  std::string stderr_path = log_dir + "/gateway.err.log";

  // Ensure secure directories
  EnsureSecureDirectory(log_dir);
  EnsureSecureDirectory(std::string(home) + "/Library");
  EnsureSecureDirectory(std::string(home) + "/Library/LaunchAgents");

  // Build plist content
  std::string plist_content = BuildLaunchAgentPlistXml(
      label,
      config.description,
      config.program_arguments,
      config.working_directory,
      stdout_path,
      stderr_path,
      config.environment);

  // Write plist file
  std::ofstream plist_file(plist_path);
  if (!plist_file) {
    throw std::runtime_error("Failed to write plist file: " + plist_path);
  }
  plist_file << plist_content;
  plist_file.close();
  chmod(plist_path.c_str(), LAUNCH_AGENT_PLIST_MODE);

  // Unload old service
  ExecLaunchctl({"bootout", domain + "/" + label});
  ExecLaunchctl({"unload", plist_path});

  // Enable and bootstrap (aligned with OpenClaw launchd.ts:440-456)
  ExecLaunchctl({"enable", domain + "/" + label});
  
  auto boot = ExecLaunchctl({"bootstrap", domain, plist_path});
  if (boot.exit_code != 0) {
    throw std::runtime_error(
        "launchctl bootstrap failed: " + boot.stdout_output + "\n" +
        "LaunchAgent install requires a logged-in macOS GUI session. "
        "If running via SSH, sign in to the macOS desktop first.");
  }

  // Kickstart service
  ExecLaunchctl({"kickstart", "-k", domain + "/" + label});

  config.stdout.write("\n[✓] Installed LaunchAgent: " + plist_path + "\n");
  config.stdout.write("    Logs: " + stdout_path + "\n");
}

// Uninstall LaunchAgent (aligned with OpenClaw launchd.ts:309-336)
void LaunchdService::Uninstall(const ManageConfig& config) {
  std::string domain = ResolveGuiDomain();
  std::string label = ResolveLaunchAgentLabel(config.env);
  std::string plist_path = ResolveLaunchAgentPlistPath(config.env);

  // Bootout and unload
  ExecLaunchctl({"bootout", domain, plist_path});
  ExecLaunchctl({"unload", plist_path});

  // Move to Trash
  if (fs::exists(plist_path)) {
    const char* home = std::getenv("HOME");
    std::string trash_dir = std::string(home) + "/.Trash";
    fs::create_directories(trash_dir);
    
    std::string dest = trash_dir + "/" + label + ".plist";
    try {
      fs::rename(plist_path, dest);
      config.stdout.write("[✓] Moved LaunchAgent to Trash: " + dest + "\n");
    } catch (...) {
      config.stdout.write("[!] LaunchAgent remains at " + plist_path + "\n");
    }
  } else {
    config.stdout.write("[!] LaunchAgent not found at " + plist_path + "\n");
  }
}

// Stop LaunchAgent (aligned with OpenClaw launchd.ts:383-391)
void LaunchdService::Stop(const ControlConfig& config) {
  std::string domain = ResolveGuiDomain();
  std::string label = ResolveLaunchAgentLabel(config.env);

  auto res = ExecLaunchctl({"bootout", domain + "/" + label});
  if (res.exit_code != 0 && 
      res.stdout_output.find("no such process") == std::string::npos) {
    throw std::runtime_error("launchctl bootout failed: " + res.stdout_output);
  }

  config.stdout.write("[✓] Stopped LaunchAgent: " + domain + "/" + label + "\n");
}

// Restart LaunchAgent (aligned with OpenClaw launchd.ts:471-525)
void LaunchdService::Restart(const ControlConfig& config) {
  std::string domain = ResolveGuiDomain();
  std::string label = ResolveLaunchAgentLabel(config.env);
  std::string plist_path = ResolveLaunchAgentPlistPath(config.env);

  // Get previous PID for graceful shutdown
  auto runtime = ExecLaunchctl({"print", domain + "/" + label});
  int previous_pid = -1;
  if (runtime.exit_code == 0) {
    auto info = ParseLaunchctlPrint(runtime.stdout_output);
    previous_pid = info.pid;
  }

  // Bootout
  auto stop = ExecLaunchctl({"bootout", domain + "/" + label});
  if (stop.exit_code != 0 && 
      stop.stdout_output.find("no such process") == std::string::npos) {
    throw std::runtime_error("launchctl bootout failed: " + stop.stdout_output);
  }

  // Wait for process to exit (if PID available)
  if (previous_pid > 0) {
    int retry = 0;
    while (retry++ < 50 && kill(previous_pid, 0) == 0) {
      usleep(200000);  // 200ms
    }
  }

  // Enable and bootstrap
  ExecLaunchctl({"enable", domain + "/" + label});
  
  auto boot = ExecLaunchctl({"bootstrap", domain, plist_path});
  if (boot.exit_code != 0) {
    throw std::runtime_error("launchctl bootstrap failed: " + boot.stdout_output);
  }

  // Kickstart
  auto start = ExecLaunchctl({"kickstart", "-k", domain + "/" + label});
  if (start.exit_code != 0) {
    throw std::runtime_error("launchctl kickstart failed: " + start.stdout_output);
  }

  config.stdout.write("[✓] Restarted LaunchAgent: " + domain + "/" + label + "\n");
}

// Check if loaded (aligned with OpenClaw launchd.ts:167-172)
bool LaunchdService::IsLoaded(const EnvConfig& config) {
  std::string domain = ResolveGuiDomain();
  std::string label = ResolveLaunchAgentLabel(config.env);

  auto res = ExecLaunchctl({"print", domain + "/" + label});
  return res.exit_code == 0;
}

// Read command from plist (aligned with OpenClaw launchd.ts:67-72)
std::optional<DaemonService::CommandConfig> LaunchdService::ReadCommand(
    const std::unordered_map<std::string, std::string>& env) {
  std::string plist_path = ResolveLaunchAgentPlistPath(env);
  
  if (!fs::exists(plist_path)) {
    return std::nullopt;
  }

  // Simple XML parsing for ProgramArguments
  std::ifstream file(plist_path);
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

  CommandConfig config;
  
  // Extract ProgramArguments (simple XML extraction)
  size_t args_start = content.find("<key>ProgramArguments</key>");
  if (args_start == std::string::npos) {
    return std::nullopt;
  }
  
  size_t array_start = content.find("<array>", args_start);
  size_t array_end = content.find("</array>", array_start);
  if (array_start == std::string::npos || array_end == std::string::npos) {
    return std::nullopt;
  }

  std::string args_section = content.substr(array_start, array_end - array_start);
  
  // Extract each <string> tag
  size_t pos = 0;
  while ((pos = args_section.find("<string>", pos)) != std::string::npos) {
    size_t end = args_section.find("</string>", pos);
    if (end == std::string::npos) break;
    
    std::string arg = args_section.substr(pos + 8, end - pos - 8);
    config.program_arguments.push_back(arg);
    pos = end;
  }

  // Extract WorkingDirectory
  size_t wd_start = content.find("<key>WorkingDirectory</key>");
  if (wd_start != std::string::npos) {
    size_t str_start = content.find("<string>", wd_start);
    size_t str_end = content.find("</string>", str_start);
    if (str_start != std::string::npos && str_end != std::string::npos) {
      config.working_directory = content.substr(str_start + 8, str_end - str_start - 8);
    }
  }

  return config;
}

// Read runtime status (aligned with OpenClaw launchd.ts:193-218)
DaemonService::ServiceRuntime LaunchdService::ReadRuntime(
    const std::unordered_map<std::string, std::string>& env) {
  std::string domain = ResolveGuiDomain();
  std::string label = ResolveLaunchAgentLabel(env);

  auto res = ExecLaunchctl({"print", domain + "/" + label});
  
  ServiceRuntime runtime;
  
  if (res.exit_code != 0) {
    runtime.status = "unknown";
    runtime.detail = res.stdout_output;
    runtime.missing_unit = true;
    return runtime;
  }

  auto parsed = ParseLaunchctlPrint(res.stdout_output);
  
  // Check if plist exists
  std::string plist_path = ResolveLaunchAgentPlistPath(env);
  bool plist_exists = fs::exists(plist_path);

  std::string state_lower = parsed.state;
  std::transform(state_lower.begin(), state_lower.end(), 
                 state_lower.begin(), ::tolower);

  if (state_lower == "running" || parsed.pid > 0) {
    runtime.status = "running";
  } else if (!state_lower.empty()) {
    runtime.status = "stopped";
  } else {
    runtime.status = "unknown";
  }

  runtime.state = parsed.state;
  runtime.pid = parsed.pid;
  runtime.last_exit_status = parsed.last_exit_status;
  runtime.last_exit_reason = parsed.last_exit_reason;
  runtime.cached_label = !plist_exists;

  return runtime;
}

}  // namespace daemon
}  // namespace hunterclaw
