#include "hunterclaw/daemon/schtasks_impl.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace daemon {

namespace {

// Default task name (aligned with OpenClaw schtasks.ts:6)
constexpr const char* DEFAULT_TASK_NAME = "OpenClaw\\Gateway";

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

// Parse key:value output (aligned with OpenClaw runtime-parse.ts)
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

// Quote argument for cmd.exe (aligned with OpenClaw cmd-argv.ts)
std::string QuoteCmdArg(const std::string& arg) {
  if (arg.empty()) {
    return "\"\"";
  }
  
  // Check if needs quoting
  bool needs_quote = false;
  for (char c : arg) {
    if (c == ' ' || c == '\t' || c == '"') {
      needs_quote = true;
      break;
    }
  }
  
  if (!needs_quote) {
    return arg;
  }
  
  // Quote and escape
  std::string result = "\"";
  for (char c : arg) {
    if (c == '"') {
      result += "\\\"";
    } else {
      result += c;
    }
  }
  result += "\"";
  return result;
}

// Quote for schtasks /TR parameter (aligned with OpenClaw schtasks.ts:41-46)
std::string QuoteSchtasksArg(const std::string& arg) {
  if (arg.find_first_of(" \t\"") == std::string::npos) {
    return arg;
  }
  
  std::string result = "\"";
  for (char c : arg) {
    if (c == '"') {
      result += "\\\"";
    } else {
      result += c;
    }
  }
  result += "\"";
  return result;
}

// Build task script (aligned with OpenClaw schtasks.ts:182-211)
std::string BuildTaskScript(
    const std::string& description,
    const std::vector<std::string>& program_arguments,
    const std::string& working_directory,
    const std::unordered_map<std::string, std::string>& environment) {
  
  std::ostringstream script;
  script << "@echo off\r\n";
  
  if (!description.empty()) {
    script << "rem " << description << "\r\n";
  }
  
  if (!working_directory.empty()) {
    script << "cd /d " << QuoteCmdArg(working_directory) << "\r\n";
  }
  
  // Set environment variables (skip PATH)
  for (const auto& [key, value] : environment) {
    std::string key_upper = key;
    std::transform(key_upper.begin(), key_upper.end(), 
                   key_upper.begin(), ::toupper);
    
    if (key_upper != "PATH" && !value.empty()) {
      script << "set " << key << "=" << value << "\r\n";
    }
  }
  
  // Build command line
  for (size_t i = 0; i < program_arguments.size(); ++i) {
    if (i > 0) script << " ";
    script << QuoteCmdArg(program_arguments[i]);
  }
  script << "\r\n";
  
  return script.str();
}

}  // namespace

// Resolve task name (aligned with OpenClaw schtasks.ts:21-27)
std::string ScheduledTaskService::ResolveScheduledTaskName(
    const std::unordered_map<std::string, std::string>& env) const {
  auto it = env.find("OPENCLAW_WINDOWS_TASK_NAME");
  if (it != env.end() && !it->second.empty()) {
    return it->second;
  }
  
  // Default task name (can be customized by OPENCLAW_PROFILE)
  auto profile_it = env.find("OPENCLAW_PROFILE");
  if (profile_it != env.end() && !profile_it->second.empty()) {
    return std::string(DEFAULT_TASK_NAME) + "\\" + profile_it->second;
  }
  
  return DEFAULT_TASK_NAME;
}

// Execute schtasks.exe (aligned with OpenClaw schtasks-exec.ts)
ScheduledTaskService::SchtasksResult ScheduledTaskService::ExecSchtasks(
    const std::vector<std::string>& args) const {
  std::string cmd = "schtasks.exe";
  for (const auto& arg : args) {
    cmd += " " + arg;
  }

  auto [output, exit_code] = ExecuteCommand(cmd);
  
  return {output, "", exit_code};
}

// Parse schtasks query (aligned with OpenClaw schtasks.ts:117-133)
ScheduledTaskService::SchtasksQueryInfo ScheduledTaskService::ParseSchtasksQuery(
    const std::string& output) const {
  auto entries = ParseKeyValueOutput(output, ":");
  
  SchtasksQueryInfo info;
  
  if (entries.count("status")) {
    info.status = entries["status"];
  }
  
  if (entries.count("last run time")) {
    info.last_result = entries["last run time"];
  }
  
  if (entries.count("last run result")) {
    info.last_result = entries["last run result"];
  }
  
  if (entries.count("next run time")) {
    info.next_run_time = entries["next run time"];
  }
  
  return info;
}

// Install scheduled task (aligned with OpenClaw schtasks.ts:222-282)
void ScheduledTaskService::Install(const InstallConfig& config) {
  const char* home = std::getenv("HOME");
  if (!home) {
    home = std::getenv("USERPROFILE");
  }
  if (!home) {
    throw std::runtime_error("HOME/USERPROFILE environment variable not set");
  }

  // Resolve script path
  std::string state_dir = std::string(home) + "\\.hunterclaw\\state";
  std::string script_path = state_dir + "\\gateway.cmd";
  
  // Create state directory
  fs::create_directories(state_dir);

  // Build and write script
  std::string script_content = BuildTaskScript(
      config.description,
      config.program_arguments,
      config.working_directory,
      config.environment);

  std::ofstream script_file(script_path);
  if (!script_file) {
    throw std::runtime_error("Failed to write task script: " + script_path);
  }
  script_file << script_content;
  script_file.close();

  // Create scheduled task
  std::string task_name = ResolveScheduledTaskName(config.env);
  std::string quoted_script = QuoteSchtasksArg(script_path);

  std::vector<std::string> base_args = {
      "/Create", "/F", "/SC", "ONLOGON", "/RL", "LIMITED",
      "/TN", task_name, "/TR", quoted_script
  };

  // Try to get current user
  const char* username = std::getenv("USERNAME");
  const char* userdomain = std::getenv("USERDOMAIN");
  
  std::string task_user;
  if (userdomain && username) {
    task_user = std::string(userdomain) + "\\" + username;
  } else if (username) {
    task_user = username;
  }

  // Try with user first
  auto create = ExecSchtasks(base_args);
  if (create.exit_code != 0 && !task_user.empty()) {
    std::vector<std::string> user_args = base_args;
    user_args.push_back("/RU");
    user_args.push_back(task_user);
    user_args.push_back("/NP");
    user_args.push_back("/IT");
    
    create = ExecSchtasks(user_args);
  }

  if (create.exit_code != 0) {
    std::string detail = create.stdout_output;
    std::string detail_lower = detail;
    std::transform(detail_lower.begin(), detail_lower.end(),
                   detail_lower.begin(), ::tolower);
    
    std::string hint;
    if (detail_lower.find("access is denied") != std::string::npos) {
      hint = " Run PowerShell as Administrator or rerun without installing the daemon.";
    }
    
    throw std::runtime_error("schtasks create failed: " + detail + hint);
  }

  // Start task
  ExecSchtasks({"/Run", "/TN", task_name});

  config.stdout.write("\n[✓] Installed Scheduled Task: " + task_name + "\n");
  config.stdout.write("    Task script: " + script_path + "\n");
}

// Uninstall scheduled task (aligned with OpenClaw schtasks.ts:284-299)
void ScheduledTaskService::Uninstall(const ManageConfig& config) {
  std::string task_name = ResolveScheduledTaskName(config.env);

  // Delete task
  ExecSchtasks({"/Delete", "/F", "/TN", task_name});

  // Remove script
  const char* home = std::getenv("HOME");
  if (!home) {
    home = std::getenv("USERPROFILE");
  }
  if (home) {
    std::string script_path = std::string(home) + "\\.hunterclaw\\state\\gateway.cmd";
    
    if (fs::exists(script_path)) {
      try {
        fs::remove(script_path);
        config.stdout.write("[✓] Removed task script: " + script_path + "\n");
      } catch (...) {
        config.stdout.write("[!] Failed to remove script: " + script_path + "\n");
      }
    } else {
      config.stdout.write("[!] Task script not found at " + script_path + "\n");
    }
  }
}

// Stop scheduled task (aligned with OpenClaw schtasks.ts:306-314)
void ScheduledTaskService::Stop(const ControlConfig& config) {
  std::string task_name = ResolveScheduledTaskName(config.env);

  auto res = ExecSchtasks({"/End", "/TN", task_name});
  
  // Ignore "not running" error
  if (res.exit_code != 0) {
    std::string output_lower = res.stdout_output;
    std::transform(output_lower.begin(), output_lower.end(),
                   output_lower.begin(), ::tolower);
    
    if (output_lower.find("not running") == std::string::npos) {
      throw std::runtime_error("schtasks end failed: " + res.stdout_output);
    }
  }

  config.stdout.write("[✓] Stopped Scheduled Task: " + task_name + "\n");
}

// Restart scheduled task (aligned with OpenClaw schtasks.ts:316-328)
void ScheduledTaskService::Restart(const ControlConfig& config) {
  std::string task_name = ResolveScheduledTaskName(config.env);

  // End existing instance
  ExecSchtasks({"/End", "/TN", task_name});

  // Run task
  auto res = ExecSchtasks({"/Run", "/TN", task_name});
  if (res.exit_code != 0) {
    throw std::runtime_error("schtasks run failed: " + res.stdout_output);
  }

  config.stdout.write("[✓] Restarted Scheduled Task: " + task_name + "\n");
}

// Check if installed (aligned with OpenClaw schtasks.ts:330-335)
bool ScheduledTaskService::IsLoaded(const EnvConfig& config) {
  std::string task_name = ResolveScheduledTaskName(config.env);

  auto res = ExecSchtasks({"/Query", "/TN", task_name});
  return res.exit_code == 0;
}

// Read command from script (aligned with OpenClaw schtasks.ts:63-109)
std::optional<DaemonService::CommandConfig> ScheduledTaskService::ReadCommand(
    const std::unordered_map<std::string, std::string>& env) {
  const char* home = std::getenv("HOME");
  if (!home) {
    home = std::getenv("USERPROFILE");
  }
  if (!home) {
    return std::nullopt;
  }

  std::string script_path = std::string(home) + "\\.hunterclaw\\state\\gateway.cmd";
  
  if (!fs::exists(script_path)) {
    return std::nullopt;
  }

  std::ifstream file(script_path);
  if (!file) {
    return std::nullopt;
  }

  CommandConfig config;
  std::string line;
  std::string command_line;
  
  while (std::getline(file, line)) {
    // Trim whitespace
    line.erase(0, line.find_first_not_of(" \t\r\n"));
    line.erase(line.find_last_not_of(" \t\r\n") + 1);
    
    if (line.empty()) continue;
    
    std::string line_lower = line;
    std::transform(line_lower.begin(), line_lower.end(),
                   line_lower.begin(), ::tolower);
    
    if (line_lower.substr(0, 5) == "@echo") continue;
    if (line_lower.substr(0, 4) == "rem ") continue;
    
    if (line_lower.substr(0, 4) == "set ") {
      // Parse environment variable
      std::string assignment = line.substr(4);
      size_t eq_pos = assignment.find('=');
      if (eq_pos != std::string::npos) {
        std::string key = assignment.substr(0, eq_pos);
        std::string value = assignment.substr(eq_pos + 1);
        config.environment[key] = value;
      }
      continue;
    }
    
    if (line_lower.substr(0, 6) == "cd /d ") {
      std::string wd = line.substr(6);
      // Remove quotes
      if (wd.size() >= 2 && wd.front() == '"' && wd.back() == '"') {
        wd = wd.substr(1, wd.size() - 2);
      }
      config.working_directory = wd;
      continue;
    }
    
    // This is the command line
    command_line = line;
    break;
  }

  if (command_line.empty()) {
    return std::nullopt;
  }

  // Simple argument splitting (real implementation should handle cmd quoting)
  std::istringstream iss(command_line);
  std::string arg;
  while (iss >> arg) {
    // Remove quotes
    if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
      arg = arg.substr(1, arg.size() - 2);
    }
    config.program_arguments.push_back(arg);
  }

  return config;
}

// Read runtime status (aligned with OpenClaw schtasks.ts:337-368)
DaemonService::ServiceRuntime ScheduledTaskService::ReadRuntime(
    const std::unordered_map<std::string, std::string>& env) {
  std::string task_name = ResolveScheduledTaskName(env);

  auto res = ExecSchtasks({"/Query", "/TN", task_name, "/V", "/FO", "LIST"});

  ServiceRuntime runtime;

  if (res.exit_code != 0) {
    std::string detail = res.stdout_output;
    std::string detail_lower = detail;
    std::transform(detail_lower.begin(), detail_lower.end(),
                   detail_lower.begin(), ::tolower);
    
    bool missing = detail_lower.find("cannot find the file") != std::string::npos;
    
    runtime.status = missing ? "stopped" : "unknown";
    runtime.detail = detail;
    runtime.missing_unit = missing;
    return runtime;
  }

  auto parsed = ParseSchtasksQuery(res.stdout_output);

  // Derive status from Last Run Result (aligned with OpenClaw schtasks.ts:162-180)
  std::string result_lower = parsed.last_result;
  std::transform(result_lower.begin(), result_lower.end(),
                 result_lower.begin(), ::tolower);
  
  // Running result codes: 0x41301
  if (result_lower.find("0x41301") != std::string::npos ||
      result_lower.find("267009") != std::string::npos) {  // Decimal for 0x41301
    runtime.status = "running";
  } else if (!parsed.status.empty()) {
    runtime.status = "stopped";
    runtime.detail = "Task Last Run Result=" + parsed.last_result + "; treating as not running.";
  } else {
    runtime.status = "unknown";
    runtime.detail = "Task status is locale-dependent and no numeric Last Run Result was available.";
  }

  runtime.state = parsed.status;
  runtime.last_run_time = parsed.last_result;
  runtime.last_run_result = parsed.last_result;

  return runtime;
}

}  // namespace daemon
}  // namespace hunterclaw
