#include "daemon/daemon_system.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>

#ifdef __linux__
#include <systemd/sd-daemon.h>
#endif

namespace hunterclaw {
namespace daemon {

// ============================================================================
// 4. Systemd Service Implementation
// ============================================================================

bool SystemdService::install(const GatewayServiceInstallArgs& args) {
    std::string unit_path = resolve_systemd_unit_path(args.env);
    std::string unit_content = build_systemd_unit(args);
    
    // Create directory
    std::string dir = unit_path.substr(0, unit_path.find_last_of('/'));
    mkdir(dir.c_str(), 0755);
    
    // Write unit file
    std::ofstream ofs(unit_path);
    if (!ofs) return false;
    ofs << unit_content;
    ofs.close();
    
    // Enable and start service
    execute_systemctl({"daemon-reload"});
    execute_systemctl({"--user", "enable", "--now", resolve_systemd_unit_path(args.env)});
    
    return true;
}

bool SystemdService::uninstall(const GatewayServiceManageArgs& args) {
    std::string unit_path = resolve_systemd_unit_path(args.env);
    
    // Stop and disable service
    execute_systemctl({"--user", "disable", "--now", unit_path});
    
    // Remove unit file
    std::remove(unit_path.c_str());
    
    execute_systemctl({"daemon-reload"});
    
    return true;
}

bool SystemdService::stop(const GatewayServiceControlArgs& args) {
    if (!args.env) return false;
    return execute_systemctl({"--user", "stop", resolve_systemd_unit_path(*args.env)});
}

GatewayServiceRestartResult SystemdService::restart(const GatewayServiceControlArgs& args) {
    if (!args.env) {
        return {GatewayServiceRestartResult::Outcome::Completed};
    }
    
    bool success = execute_systemctl({"--user", "restart", resolve_systemd_unit_path(*args.env)});
    
    return {success ? GatewayServiceRestartResult::Outcome::Completed
                   : GatewayServiceRestartResult::Outcome::Scheduled};
}

bool SystemdService::is_loaded(const GatewayServiceEnv& env) {
    // Check if service is enabled
    std::string cmd = "systemctl --user is-enabled " + resolve_systemd_unit_path(env);
    int result = std::system(cmd.c_str());
    return (result == 0);
}

std::optional<GatewayServiceCommandConfig> SystemdService::read_command(const GatewayServiceEnv& env) {
    std::string unit_path = resolve_systemd_unit_path(env);
    
    std::ifstream ifs(unit_path);
    if (!ifs) return std::nullopt;
    
    GatewayServiceCommandConfig config;
    std::string line;
    
    while (std::getline(ifs, line)) {
        if (line.find("ExecStart=") == 0) {
            std::string exec = line.substr(10);
            // Parse exec start
            std::istringstream iss(exec);
            std::string arg;
            while (iss >> arg) {
                config.program_arguments.push_back(arg);
            }
        } else if (line.find("WorkingDirectory=") == 0) {
            config.working_directory = line.substr(17);
        }
    }
    
    config.source_path = unit_path;
    return config;
}

GatewayServiceRuntime SystemdService::read_runtime(const GatewayServiceEnv& env) {
    GatewayServiceRuntime runtime;
    
    // Get systemctl status
    std::string cmd = "systemctl --user status " + resolve_systemd_unit_path(env);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            std::string line(buffer);
            if (line.find("Main PID:") != std::string::npos) {
                // Extract PID
                size_t pos = line.find("Main PID:") + 10;
                runtime.pid = std::stoi(line.substr(pos));
            } else if (line.find("Active:") != std::string::npos) {
                runtime.status = line.substr(line.find("Active:") + 8);
            }
        }
        pclose(pipe);
    }
    
    return runtime;
}

std::string SystemdService::resolve_systemd_unit_path(const GatewayServiceEnv& env) {
    auto it = env.find("HOME");
    std::string home = (it != env.end()) ? it->second : "/root";
    
    std::string service_name = "openclaw-gateway";
    if (env.find("OPENCLAW_PROFILE") != env.end()) {
        service_name = "openclaw-gateway-" + env.at("OPENCLAW_PROFILE");
    }
    
    return home + "/.config/systemd/user/" + service_name + ".service";
}

std::string SystemdService::build_systemd_unit(const GatewayServiceInstallArgs& args) {
    std::ostringstream oss;
    
    oss << "[Unit]\n";
    oss << "Description=" << args.description.value_or("OpenClaw Gateway") << "\n";
    oss << "After=network.target\n\n";
    
    oss << "[Service]\n";
    oss << "Type=simple\n";
    oss << "ExecStart=";
    for (const auto& arg : args.program_arguments) {
        oss << arg << " ";
    }
    oss << "\n";
    
    if (args.working_directory) {
        oss << "WorkingDirectory=" << *args.working_directory << "\n";
    }
    
    if (args.environment) {
        for (const auto& [key, value] : *args.environment) {
            oss << "Environment=\"" << key << "=" << value << "\"\n";
        }
    }
    
    oss << "Restart=always\n";
    oss << "RestartSec=10\n\n";
    
    oss << "[Install]\n";
    oss << "WantedBy=default.target\n";
    
    return oss.str();
}

bool SystemdService::execute_systemctl(const std::vector<std::string>& args) {
    std::ostringstream cmd;
    cmd << "systemctl";
    for (const auto& arg : args) {
        cmd << " " << arg;
    }
    
    int result = std::system(cmd.str().c_str());
    return (result == 0);
}

// ============================================================================
// 5. LaunchAgent Service Implementation
// ============================================================================

bool LaunchAgentService::install(const GatewayServiceInstallArgs& args) {
    std::string plist_path = resolve_launch_agent_plist_path(args.env);
    std::string plist_content = build_launch_agent_plist(args);
    
    // Create directory
    std::string dir = plist_path.substr(0, plist_path.find_last_of('/'));
    mkdir(dir.c_str(), 0755);
    
    // Write plist file
    std::ofstream ofs(plist_path);
    if (!ofs) return false;
    ofs << plist_content;
    ofs.close();
    
    // Load launch agent
    execute_launchctl({"load", "-w", plist_path});
    
    return true;
}

bool LaunchAgentService::uninstall(const GatewayServiceManageArgs& args) {
    std::string plist_path = resolve_launch_agent_plist_path(args.env);
    
    // Unload launch agent
    execute_launchctl({"unload", "-w", plist_path});
    
    // Remove plist file
    std::remove(plist_path.c_str());
    
    return true;
}

bool LaunchAgentService::stop(const GatewayServiceControlArgs& args) {
    if (!args.env) return false;
    return execute_launchctl({"stop", "com.openclaw.gateway"});
}

GatewayServiceRestartResult LaunchAgentService::restart(const GatewayServiceControlArgs& args) {
    if (!args.env) {
        return {GatewayServiceRestartResult::Outcome::Completed};
    }
    
    execute_launchctl({"stop", "com.openclaw.gateway"});
    bool success = execute_launchctl({"start", "com.openclaw.gateway"});
    
    return {success ? GatewayServiceRestartResult::Outcome::Completed
                   : GatewayServiceRestartResult::Outcome::Scheduled};
}

bool LaunchAgentService::is_loaded(const GatewayServiceEnv& env) {
    std::string cmd = "launchctl list | grep com.openclaw.gateway";
    int result = std::system(cmd.c_str());
    return (result == 0);
}

std::optional<GatewayServiceCommandConfig> LaunchAgentService::read_command(const GatewayServiceEnv& env) {
    std::string plist_path = resolve_launch_agent_plist_path(env);
    
    // Parse plist (simplified)
    std::ifstream ifs(plist_path);
    if (!ifs) return std::nullopt;
    
    GatewayServiceCommandConfig config;
    config.source_path = plist_path;
    
    // Would need XML parser for full implementation
    return config;
}

GatewayServiceRuntime LaunchAgentService::read_runtime(const GatewayServiceEnv& env) {
    GatewayServiceRuntime runtime;
    
    // Get launchctl list
    std::string cmd = "launchctl list com.openclaw.gateway";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            std::string line(buffer);
            if (line.find("PID") != std::string::npos) {
                // Extract PID
                runtime.status = "running";
            }
        }
        pclose(pipe);
    }
    
    return runtime;
}

std::string LaunchAgentService::resolve_launch_agent_plist_path(const GatewayServiceEnv& env) {
    auto it = env.find("HOME");
    std::string home = (it != env.end()) ? it->second : "/Users/default";
    
    return home + "/Library/LaunchAgents/com.openclaw.gateway.plist";
}

std::string LaunchAgentService::build_launch_agent_plist(const GatewayServiceInstallArgs& args) {
    std::ostringstream oss;
    
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    oss << "<plist version=\"1.0\">\n";
    oss << "<dict>\n";
    oss << "  <key>Label</key>\n";
    oss << "  <string>com.openclaw.gateway</string>\n";
    oss << "  <key>ProgramArguments</key>\n";
    oss << "  <array>\n";
    for (const auto& arg : args.program_arguments) {
        oss << "    <string>" << arg << "</string>\n";
    }
    oss << "  </array>\n";
    
    if (args.working_directory) {
        oss << "  <key>WorkingDirectory</key>\n";
        oss << "  <string>" << *args.working_directory << "</string>\n";
    }
    
    oss << "  <key>RunAtLoad</key>\n";
    oss << "  <true/>\n";
    oss << "  <key>KeepAlive</key>\n";
    oss << "  <true/>\n";
    oss << "</dict>\n";
    oss << "</plist>\n";
    
    return oss.str();
}

bool LaunchAgentService::execute_launchctl(const std::vector<std::string>& args) {
    std::ostringstream cmd;
    cmd << "launchctl";
    for (const auto& arg : args) {
        cmd << " " << arg;
    }
    
    int result = std::system(cmd.str().c_str());
    return (result == 0);
}

// ============================================================================
// 6. Scheduled Task Service Implementation (Windows - Stub)
// ============================================================================

bool ScheduledTaskService::install(const GatewayServiceInstallArgs& args) {
    // Windows schtasks implementation
    return false;
}

bool ScheduledTaskService::uninstall(const GatewayServiceManageArgs& args) {
    return false;
}

bool ScheduledTaskService::stop(const GatewayServiceControlArgs& args) {
    return false;
}

GatewayServiceRestartResult ScheduledTaskService::restart(const GatewayServiceControlArgs& args) {
    return {GatewayServiceRestartResult::Outcome::Completed};
}

bool ScheduledTaskService::is_loaded(const GatewayServiceEnv& env) {
    return false;
}

std::optional<GatewayServiceCommandConfig> ScheduledTaskService::read_command(const GatewayServiceEnv& env) {
    return std::nullopt;
}

GatewayServiceRuntime ScheduledTaskService::read_runtime(const GatewayServiceEnv& env) {
    return {};
}

std::string ScheduledTaskService::resolve_task_name(const GatewayServiceEnv& env) {
    return "OpenClawGateway";
}

bool ScheduledTaskService::execute_schtasks(const std::vector<std::string>& args) {
    return false;
}

// ============================================================================
// 7-12. Other Daemon System Components (Simplified Stubs)
// ============================================================================

ServiceAuditResult ServiceAudit::audit_service(GatewayService& service,
                                              const GatewayServiceEnv& env) {
    ServiceAuditResult result;
    result.runtime = service.read_runtime(env);
    result.config = service.read_command(env);
    result.healthy = service.is_loaded(env);
    
    if (!result.healthy) {
        result.issues.push_back("Service not loaded");
    }
    
    return result;
}

bool ServiceAudit::is_running(GatewayService& service, const GatewayServiceEnv& env) {
    auto runtime = service.read_runtime(env);
    return runtime.pid.has_value();
}

std::string ServiceAudit::get_status_summary(const ServiceAuditResult& result) {
    std::ostringstream oss;
    oss << "Health: " << (result.healthy ? "OK" : "FAIL") << "\n";
    
    if (!result.issues.empty()) {
        oss << "Issues: " << result.issues.size() << "\n";
    }
    
    return oss.str();
}

std::optional<std::string> RuntimeBinary::resolve_runtime_binary(const GatewayServiceEnv& env) {
    // Try Node.js first
    auto node = get_node_binary();
    if (node) return node;
    
    // Try Bun
    auto bun = get_bun_binary();
    if (bun) return bun;
    
    // Try Deno
    return get_deno_binary();
}

std::optional<std::string> RuntimeBinary::get_node_binary() {
    FILE* pipe = popen("which node", "r");
    if (!pipe) return std::nullopt;
    
    char buffer[256];
    if (fgets(buffer, sizeof(buffer), pipe)) {
        pclose(pipe);
        std::string path(buffer);
        path.erase(path.find_last_not_of(" \t\n\r") + 1);
        return path;
    }
    
    pclose(pipe);
    return std::nullopt;
}

std::optional<std::string> RuntimeBinary::get_bun_binary() {
    FILE* pipe = popen("which bun", "r");
    if (!pipe) return std::nullopt;
    
    char buffer[256];
    if (fgets(buffer, sizeof(buffer), pipe)) {
        pclose(pipe);
        std::string path(buffer);
        path.erase(path.find_last_not_of(" \t\n\r") + 1);
        return path;
    }
    
    pclose(pipe);
    return std::nullopt;
}

std::optional<std::string> RuntimeBinary::get_deno_binary() {
    FILE* pipe = popen("which deno", "r");
    if (!pipe) return std::nullopt;
    
    char buffer[256];
    if (fgets(buffer, sizeof(buffer), pipe)) {
        pclose(pipe);
        std::string path(buffer);
        path.erase(path.find_last_not_of(" \t\n\r") + 1);
        return path;
    }
    
    pclose(pipe);
    return std::nullopt;
}

std::string ServiceConstants::default_service_name() {
    return "openclaw-gateway";
}

std::string ServiceConstants::resolve_service_name(const std::optional<std::string>& profile) {
    if (profile && !profile->empty()) {
        return "openclaw-gateway-" + *profile;
    }
    return default_service_name();
}

std::string ServiceConstants::default_service_description() {
    return "OpenClaw Gateway Service";
}

// ============================================================================
// Daemon System Implementation
// ============================================================================

DaemonSystem::DaemonSystem() = default;
DaemonSystem::~DaemonSystem() = default;

bool DaemonSystem::initialize() {
    if (initialized_) return true;
    
    create_platform_service();
    initialized_ = true;
    return true;
}

void DaemonSystem::shutdown() {
    if (!initialized_) return;
    service_.reset();
    initialized_ = false;
}

void DaemonSystem::create_platform_service() {
    Platform platform = detect_platform();
    
    switch (platform) {
        case Platform::Linux:
            service_ = std::make_unique<SystemdService>();
            break;
        case Platform::Darwin:
            service_ = std::make_unique<LaunchAgentService>();
            break;
        case Platform::Windows:
            service_ = std::make_unique<ScheduledTaskService>();
            break;
        default:
            // Use systemd as fallback
            service_ = std::make_unique<SystemdService>();
            break;
    }
}

GatewayService* DaemonSystem::get_service() {
    return service_.get();
}

bool DaemonSystem::install_service(const GatewayServiceInstallArgs& args) {
    if (!service_) return false;
    return service_->install(args);
}

bool DaemonSystem::uninstall_service(const GatewayServiceManageArgs& args) {
    if (!service_) return false;
    return service_->uninstall(args);
}

bool DaemonSystem::stop_service(const GatewayServiceControlArgs& args) {
    if (!service_) return false;
    return service_->stop(args);
}

GatewayServiceRestartResult DaemonSystem::restart_service(const GatewayServiceControlArgs& args) {
    if (!service_) return {GatewayServiceRestartResult::Outcome::Completed};
    return service_->restart(args);
}

bool DaemonSystem::is_service_loaded(const GatewayServiceEnv& env) {
    if (!service_) return false;
    return service_->is_loaded(env);
}

std::optional<GatewayServiceCommandConfig> DaemonSystem::read_service_command(const GatewayServiceEnv& env) {
    if (!service_) return std::nullopt;
    return service_->read_command(env);
}

GatewayServiceRuntime DaemonSystem::read_service_runtime(const GatewayServiceEnv& env) {
    if (!service_) return {};
    return service_->read_runtime(env);
}

ServiceAuditResult DaemonSystem::audit_service(const GatewayServiceEnv& env) {
    if (!service_) return {};
    return ServiceAudit::audit_service(*service_, env);
}

DaemonSystem::Platform DaemonSystem::detect_platform() {
#ifdef __linux__
    return Platform::Linux;
#elif defined(__APPLE__)
    return Platform::Darwin;
#elif defined(_WIN32)
    return Platform::Windows;
#else
    return Platform::Unknown;
#endif
}

std::string DaemonSystem::platform_name(Platform platform) {
    switch (platform) {
        case Platform::Linux: return "Linux";
        case Platform::Darwin: return "macOS";
        case Platform::Windows: return "Windows";
        default: return "Unknown";
    }
}

} // namespace daemon
} // namespace hunterclaw
