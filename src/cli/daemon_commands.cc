#include "hunterclaw/cli/daemon_commands.h"
#include "hunterclaw/daemon/launchd_impl.h"
#include "hunterclaw/daemon/systemd_impl.h"
#include "hunterclaw/daemon/schtasks_impl.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/infra/path_safety.h"
#include "hunterclaw/infra/fs_safe.h"
#include "hunterclaw/infra/exec_approvals.h"
#include <iostream>
#include <fstream>
#include <cstdlib>

namespace hunterclaw {
namespace cli {

namespace {

// Resolve platform-specific daemon service
std::shared_ptr<daemon::DaemonService> resolve_daemon_service() {
#ifdef __APPLE__
    return std::make_shared<daemon::LaunchdImpl>();
#elif __linux__
    return std::make_shared<daemon::SystemdImpl>();
#elif _WIN32
    return std::make_shared<daemon::SchtasksImpl>();
#else
    return nullptr;
#endif
}

// Resolve HunterClaw binary path
std::string resolve_hunterclaw_binary() {
    // Check environment variable first
    const char* env_binary = std::getenv("HUNTERCLAW_BINARY");
    if (env_binary && core::utils::file_exists(env_binary)) {
        return env_binary;
    }
    
    // Check common locations
    std::vector<std::string> candidates = {
        "/usr/local/bin/hunterclaw",
        "/usr/bin/hunterclaw",
        core::utils::expand_path("~/.hunterclaw/bin/hunterclaw"),
        core::utils::get_current_executable_path()
    };
    
    for (const auto& path : candidates) {
        if (core::utils::file_exists(path)) {
            return path;
        }
    }
    
    return "hunterclaw"; // Fallback to PATH
}

// Resolve working directory
std::string resolve_working_directory() {
    const char* env_dir = std::getenv("HUNTERCLAW_WORKING_DIR");
    if (env_dir) {
        return env_dir;
    }
    return core::utils::expand_path("~/.hunterclaw");
}

// Resolve daemon port
uint16_t resolve_daemon_port(const CommandArgs& args) {
    if (args.has_option("port")) {
        return static_cast<uint16_t>(args.get_int("port", 18789));
    }
    
    const char* env_port = std::getenv("HUNTERCLAW_PORT");
    if (env_port) {
        try {
            return static_cast<uint16_t>(std::stoi(env_port));
        } catch (...) {
            // Fallback to default
        }
    }
    
    return 18789;
}

// Build daemon install plan
// Aligned with OpenClaw's buildGatewayInstallPlan() (daemon-install-helpers.ts)
struct DaemonInstallPlan {
    std::vector<std::string> program_arguments;
    std::string working_directory;
    std::map<std::string, std::string> environment;
    std::string description;
};

DaemonInstallPlan build_install_plan(const CommandArgs& args) {
    DaemonInstallPlan plan;
    
    // Program arguments
    std::string binary = resolve_hunterclaw_binary();
    uint16_t port = resolve_daemon_port(args);
    
    plan.program_arguments = {
        binary,
        "--port", std::to_string(port),
        "--daemon"
    };
    
    // Working directory
    plan.working_directory = resolve_working_directory();
    
    // Environment variables
    plan.environment["HUNTERCLAW_PORT"] = std::to_string(port);
    plan.environment["HUNTERCLAW_LOG_PREFIX"] = "daemon";
    plan.environment["HUNTERCLAW_DAEMON"] = "true";
    
    if (args.has_option("token")) {
        plan.environment["HUNTERCLAW_GATEWAY_TOKEN"] = args.get_option("token");
    }
    
    if (args.get_bool("tls", false)) {
        plan.environment["HUNTERCLAW_TLS"] = "true";
    }
    
    if (args.has_option("tls-fingerprint")) {
        plan.environment["HUNTERCLAW_TLS_FINGERPRINT"] = args.get_option("tls-fingerprint");
    }
    
    // Description
    plan.description = "HunterClaw AI Agent Platform Gateway";
    
    return plan;
}

// Platform-specific log paths
// Aligned with OpenClaw's buildPlatformRuntimeLogHints() (runtime-hints.ts)
std::vector<std::string> get_daemon_log_paths() {
    std::vector<std::string> paths;
    
#ifdef __APPLE__
    paths.push_back(core::utils::expand_path("~/Library/Logs/HunterClaw/daemon.log"));
    paths.push_back(core::utils::expand_path("~/Library/Logs/HunterClaw/daemon-error.log"));
#elif __linux__
    paths.push_back("journalctl --user -u hunterclaw -n 100 -f");
#elif _WIN32
    paths.push_back(core::utils::expand_path("%USERPROFILE%\\.hunterclaw\\logs\\daemon.log"));
#endif
    
    return paths;
}

// Print service status in human-readable format
// Aligned with OpenClaw's formatRuntimeStatus() (shared.ts)
void print_service_status(
    const std::shared_ptr<daemon::DaemonService>& service,
    bool loaded,
    const daemon::DaemonCommand& command,
    const daemon::DaemonRuntime& runtime,
    bool json
) {
    if (json) {
        JsonOutput output;
        output.ok = true;
        output.result = loaded ? "loaded" : "not-loaded";
        output.data["service"] = service->Label();
        output.data["status"] = loaded ? service->LoadedText() : service->NotLoadedText();
        output.data["runtime_status"] = runtime.status;
        if (runtime.pid > 0) {
            output.data["pid"] = std::to_string(runtime.pid);
        }
        if (runtime.exit_code.has_value()) {
            output.data["exit_code"] = std::to_string(*runtime.exit_code);
        }
        output.print();
        return;
    }
    
    // Human-readable output
    std::cout << "Service: " << service->Label();
    if (loaded) {
        std::cout << " (\033[32m" << service->LoadedText() << "\033[0m)\n";
    } else {
        std::cout << " (\033[33m" << service->NotLoadedText() << "\033[0m)\n";
    }
    
    if (!command.program_arguments.empty()) {
        std::cout << "Command: ";
        for (size_t i = 0; i < command.program_arguments.size(); i++) {
            if (i > 0) std::cout << " ";
            std::cout << command.program_arguments[i];
        }
        std::cout << "\n";
    }
    
    if (!command.source_path.empty()) {
        std::cout << "Service file: " << command.source_path << "\n";
    }
    
    if (!command.working_directory.empty()) {
        std::cout << "Working dir: " << command.working_directory << "\n";
    }
    
    // Runtime status
    std::cout << "Runtime: ";
    if (runtime.status == "running") {
        std::cout << "\033[32m" << runtime.status << "\033[0m";
        if (runtime.pid > 0) {
            std::cout << " (PID: " << runtime.pid << ")";
        }
    } else if (runtime.status == "stopped") {
        std::cout << "\033[33m" << runtime.status << "\033[0m";
        if (runtime.exit_code.has_value()) {
            std::cout << " (exit code: " << *runtime.exit_code << ")";
        }
    } else {
        std::cout << "\033[31m" << runtime.status << "\033[0m";
    }
    
    if (!runtime.detail.empty()) {
        std::cout << " - " << runtime.detail;
    }
    std::cout << "\n";
    
    // Log paths
    if (!loaded) {
        std::cout << "\n\033[33mStart with:\033[0m hunterclaw daemon start\n";
    } else if (runtime.status != "running") {
        std::cout << "\n\033[31mService is loaded but not running.\033[0m\n";
        std::cout << "\033[33mCheck logs:\033[0m\n";
        for (const auto& path : get_daemon_log_paths()) {
            std::cout << "  " << path << "\n";
        }
    }
}

} // anonymous namespace

// ============================================================================
// DaemonInstallCommand
// ============================================================================

std::vector<CommandOption> DaemonInstallCommand::get_options() const {
    return {
        {"port", "-p", "--port", "Gateway port", "18789", false},
        {"token", "", "--token", "Gateway authentication token", "", false},
        {"tls", "", "--tls", "Enable TLS", "false", false},
        {"tls-fingerprint", "", "--tls-fingerprint", "TLS certificate fingerprint", "", false},
        {"force", "-f", "--force", "Force reinstall", "false", false},
        {"json", "", "--json", "JSON output", "false", false},
        {"help", "-h", "--help", "Show help", "false", false}
    };
}

void DaemonInstallCommand::print_help() const {
    std::cout << CommandParser::format_help(
        "daemon install",
        "Install HunterClaw as a system daemon (LaunchAgent/systemd/Scheduled Task)",
        get_options()
    );
}

core::Result<void> DaemonInstallCommand::execute(const std::vector<std::string>& args) {
    // Parse arguments
    auto parse_result = CommandParser::parse(args, get_options());
    if (parse_result.is_err()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    CommandArgs cmd_args = parse_result.value;
    
    if (cmd_args.get_bool("help")) {
        print_help();
        return core::Result<void>::ok();
    }
    
    bool json = cmd_args.get_bool("json");
    bool force = cmd_args.get_bool("force");
    
    // Resolve service
    auto service = resolve_daemon_service();
    if (!service) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "unsupported-platform";
            output.message = "Daemon service not supported on this platform";
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Daemon service not supported on this platform\n";
        }
        return core::Result<void>::err("Unsupported platform");
    }
    
    // Check if already loaded
    auto loaded_result = service->IsLoaded();
    if (loaded_result.is_err()) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "check-failed";
            output.message = "Failed to check daemon status: " + loaded_result.error;
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Failed to check daemon status: " << loaded_result.error << "\n";
        }
        return core::Result<void>::err(loaded_result.error);
    }
    
    bool loaded = loaded_result.value;
    if (loaded && !force) {
        if (json) {
            JsonOutput output;
            output.ok = true;
            output.result = "already-installed";
            output.message = "Daemon service already " + service->LoadedText();
            output.data["service"] = service->Label();
            output.print();
        } else {
            std::cout << "Daemon service already " << service->LoadedText() << ".\n";
            std::cout << "Reinstall with: hunterclaw daemon install --force\n";
        }
        return core::Result<void>::ok();
    }
    
    // Build install plan
    auto plan = build_install_plan(cmd_args);
    
    if (!json) {
        std::cout << "Installing daemon service: " << service->Label() << "\n";
        std::cout << "Command: ";
        for (size_t i = 0; i < plan.program_arguments.size(); i++) {
            if (i > 0) std::cout << " ";
            std::cout << plan.program_arguments[i];
        }
        std::cout << "\n";
    }
    
    // Install service
    auto install_result = service->Install(
        plan.program_arguments,
        plan.working_directory,
        plan.environment,
        plan.description
    );
    
    if (install_result.is_err()) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "install-failed";
            output.message = "Failed to install daemon: " + install_result.error;
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Failed to install daemon: " << install_result.error << "\n";
        }
        return core::Result<void>::err(install_result.error);
    }
    
    if (json) {
        JsonOutput output;
        output.ok = true;
        output.result = "installed";
        output.message = "Daemon service installed successfully";
        output.data["service"] = service->Label();
        output.print();
    } else {
        std::cout << "\033[32m✓\033[0m Daemon service installed successfully\n";
        std::cout << "\nStart with: hunterclaw daemon start\n";
        std::cout << "Status: hunterclaw daemon status\n";
    }
    
    return core::Result<void>::ok();
}

// ============================================================================
// DaemonUninstallCommand
// ============================================================================

std::vector<CommandOption> DaemonUninstallCommand::get_options() const {
    return {
        {"json", "", "--json", "JSON output", "false", false},
        {"help", "-h", "--help", "Show help", "false", false}
    };
}

void DaemonUninstallCommand::print_help() const {
    std::cout << CommandParser::format_help(
        "daemon uninstall",
        "Uninstall HunterClaw daemon service",
        get_options()
    );
}

core::Result<void> DaemonUninstallCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (parse_result.is_err()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    CommandArgs cmd_args = parse_result.value;
    
    if (cmd_args.get_bool("help")) {
        print_help();
        return core::Result<void>::ok();
    }
    
    bool json = cmd_args.get_bool("json");
    
    auto service = resolve_daemon_service();
    if (!service) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "unsupported-platform";
            output.message = "Daemon service not supported on this platform";
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Daemon service not supported on this platform\n";
        }
        return core::Result<void>::err("Unsupported platform");
    }
    
    // Check if loaded
    auto loaded_result = service->IsLoaded();
    if (loaded_result.is_err()) {
        if (!json) {
            std::cerr << "\033[33mWarning:\033[0m Failed to check daemon status: " << loaded_result.error << "\n";
        }
    }
    
    bool loaded = loaded_result.is_ok() && loaded_result.value;
    
    if (!loaded) {
        if (json) {
            JsonOutput output;
            output.ok = true;
            output.result = "not-installed";
            output.message = "Daemon service is not installed";
            output.print();
        } else {
            std::cout << "Daemon service is not installed.\n";
        }
        return core::Result<void>::ok();
    }
    
    if (!json) {
        std::cout << "Uninstalling daemon service: " << service->Label() << "\n";
    }
    
    // Stop first (aligned with OpenClaw stopBeforeUninstall=true)
    auto stop_result = service->Stop();
    if (stop_result.is_err() && !json) {
        std::cerr << "\033[33mWarning:\033[0m Failed to stop daemon: " << stop_result.error << "\n";
    }
    
    // Uninstall
    auto uninstall_result = service->Uninstall();
    if (uninstall_result.is_err()) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "uninstall-failed";
            output.message = "Failed to uninstall daemon: " + uninstall_result.error;
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Failed to uninstall daemon: " << uninstall_result.error << "\n";
        }
        return core::Result<void>::err(uninstall_result.error);
    }
    
    if (json) {
        JsonOutput output;
        output.ok = true;
        output.result = "uninstalled";
        output.message = "Daemon service uninstalled successfully";
        output.print();
    } else {
        std::cout << "\033[32m✓\033[0m Daemon service uninstalled successfully\n";
    }
    
    return core::Result<void>::ok();
}

// ============================================================================
// DaemonStartCommand
// ============================================================================

std::vector<CommandOption> DaemonStartCommand::get_options() const {
    return {
        {"json", "", "--json", "JSON output", "false", false},
        {"help", "-h", "--help", "Show help", "false", false}
    };
}

void DaemonStartCommand::print_help() const {
    std::cout << CommandParser::format_help(
        "daemon start",
        "Start HunterClaw daemon service",
        get_options()
    );
}

core::Result<void> DaemonStartCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (parse_result.is_err()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    CommandArgs cmd_args = parse_result.value;
    
    if (cmd_args.get_bool("help")) {
        print_help();
        return core::Result<void>::ok();
    }
    
    bool json = cmd_args.get_bool("json");
    
    auto service = resolve_daemon_service();
    if (!service) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "unsupported-platform";
            output.message = "Daemon service not supported on this platform";
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Daemon service not supported on this platform\n";
        }
        return core::Result<void>::err("Unsupported platform");
    }
    
    // Check if loaded
    auto loaded_result = service->IsLoaded();
    if (loaded_result.is_err()) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "check-failed";
            output.message = "Failed to check daemon status: " + loaded_result.error;
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Failed to check daemon status: " << loaded_result.error << "\n";
        }
        return core::Result<void>::err(loaded_result.error);
    }
    
    bool loaded = loaded_result.value;
    if (!loaded) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "not-installed";
            output.message = "Daemon service is not installed. Install with: hunterclaw daemon install";
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Daemon service is not installed\n";
            std::cerr << "Install with: hunterclaw daemon install\n";
        }
        return core::Result<void>::err("Not installed");
    }
    
    // Check if already running
    auto runtime_result = service->ReadRuntime();
    if (runtime_result.is_ok() && runtime_result.value.status == "running") {
        if (json) {
            JsonOutput output;
            output.ok = true;
            output.result = "already-running";
            output.message = "Daemon service is already running";
            output.data["pid"] = std::to_string(runtime_result.value.pid);
            output.print();
        } else {
            std::cout << "Daemon service is already running (PID: " << runtime_result.value.pid << ")\n";
        }
        return core::Result<void>::ok();
    }
    
    if (!json) {
        std::cout << "Starting daemon service: " << service->Label() << "\n";
    }
    
    // Start (Note: LaunchAgent/systemd auto-start on bootstrap, this is a no-op for most platforms)
    // But we implement explicit start for consistency
    auto restart_result = service->Restart(); // Restart ensures it starts
    if (restart_result.is_err()) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "start-failed";
            output.message = "Failed to start daemon: " + restart_result.error;
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Failed to start daemon: " << restart_result.error << "\n";
        }
        return core::Result<void>::err(restart_result.error);
    }
    
    if (json) {
        JsonOutput output;
        output.ok = true;
        output.result = "started";
        output.message = "Daemon service started successfully";
        output.print();
    } else {
        std::cout << "\033[32m✓\033[0m Daemon service started successfully\n";
        std::cout << "\nCheck status: hunterclaw daemon status\n";
    }
    
    return core::Result<void>::ok();
}

// ============================================================================
// DaemonStopCommand
// ============================================================================

std::vector<CommandOption> DaemonStopCommand::get_options() const {
    return {
        {"json", "", "--json", "JSON output", "false", false},
        {"help", "-h", "--help", "Show help", "false", false}
    };
}

void DaemonStopCommand::print_help() const {
    std::cout << CommandParser::format_help(
        "daemon stop",
        "Stop HunterClaw daemon service",
        get_options()
    );
}

core::Result<void> DaemonStopCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (parse_result.is_err()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    CommandArgs cmd_args = parse_result.value;
    
    if (cmd_args.get_bool("help")) {
        print_help();
        return core::Result<void>::ok();
    }
    
    bool json = cmd_args.get_bool("json");
    
    auto service = resolve_daemon_service();
    if (!service) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "unsupported-platform";
            output.message = "Daemon service not supported on this platform";
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Daemon service not supported on this platform\n";
        }
        return core::Result<void>::err("Unsupported platform");
    }
    
    if (!json) {
        std::cout << "Stopping daemon service: " << service->Label() << "\n";
    }
    
    auto stop_result = service->Stop();
    if (stop_result.is_err()) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "stop-failed";
            output.message = "Failed to stop daemon: " + stop_result.error;
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Failed to stop daemon: " + stop_result.error << "\n";
        }
        return core::Result<void>::err(stop_result.error);
    }
    
    if (json) {
        JsonOutput output;
        output.ok = true;
        output.result = "stopped";
        output.message = "Daemon service stopped successfully";
        output.print();
    } else {
        std::cout << "\033[32m✓\033[0m Daemon service stopped successfully\n";
    }
    
    return core::Result<void>::ok();
}

// ============================================================================
// DaemonRestartCommand
// ============================================================================

std::vector<CommandOption> DaemonRestartCommand::get_options() const {
    return {
        {"json", "", "--json", "JSON output", "false", false},
        {"help", "-h", "--help", "Show help", "false", false}
    };
}

void DaemonRestartCommand::print_help() const {
    std::cout << CommandParser::format_help(
        "daemon restart",
        "Restart HunterClaw daemon service",
        get_options()
    );
}

core::Result<void> DaemonRestartCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (parse_result.is_err()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    CommandArgs cmd_args = parse_result.value;
    
    if (cmd_args.get_bool("help")) {
        print_help();
        return core::Result<void>::ok();
    }
    
    bool json = cmd_args.get_bool("json");
    
    auto service = resolve_daemon_service();
    if (!service) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "unsupported-platform";
            output.message = "Daemon service not supported on this platform";
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Daemon service not supported on this platform\n";
        }
        return core::Result<void>::err("Unsupported platform");
    }
    
    if (!json) {
        std::cout << "Restarting daemon service: " << service->Label() << "\n";
    }
    
    auto restart_result = service->Restart();
    if (restart_result.is_err()) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "restart-failed";
            output.message = "Failed to restart daemon: " + restart_result.error;
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Failed to restart daemon: " << restart_result.error << "\n";
        }
        return core::Result<void>::err(restart_result.error);
    }
    
    if (json) {
        JsonOutput output;
        output.ok = true;
        output.result = "restarted";
        output.message = "Daemon service restarted successfully";
        output.print();
    } else {
        std::cout << "\033[32m✓\033[0m Daemon service restarted successfully\n";
        std::cout << "\nCheck status: hunterclaw daemon status\n";
    }
    
    return core::Result<void>::ok();
}

// ============================================================================
// DaemonStatusCommand
// ============================================================================

std::vector<CommandOption> DaemonStatusCommand::get_options() const {
    return {
        {"json", "", "--json", "JSON output", "false", false},
        {"help", "-h", "--help", "Show help", "false", false}
    };
}

void DaemonStatusCommand::print_help() const {
    std::cout << CommandParser::format_help(
        "daemon status",
        "Show HunterClaw daemon service status",
        get_options()
    );
}

core::Result<void> DaemonStatusCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (parse_result.is_err()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    CommandArgs cmd_args = parse_result.value;
    
    if (cmd_args.get_bool("help")) {
        print_help();
        return core::Result<void>::ok();
    }
    
    bool json = cmd_args.get_bool("json");
    
    auto service = resolve_daemon_service();
    if (!service) {
        if (json) {
            JsonOutput output;
            output.ok = false;
            output.result = "unsupported-platform";
            output.message = "Daemon service not supported on this platform";
            output.print();
        } else {
            std::cerr << "\033[31mError:\033[0m Daemon service not supported on this platform\n";
        }
        return core::Result<void>::err("Unsupported platform");
    }
    
    // Gather status
    auto loaded_result = service->IsLoaded();
    bool loaded = loaded_result.is_ok() && loaded_result.value;
    
    daemon::DaemonCommand command;
    if (loaded) {
        auto cmd_result = service->ReadCommand();
        if (cmd_result.is_ok()) {
            command = cmd_result.value;
        }
    }
    
    daemon::DaemonRuntime runtime;
    if (loaded) {
        auto runtime_result = service->ReadRuntime();
        if (runtime_result.is_ok()) {
            runtime = runtime_result.value;
        } else {
            runtime.status = "unknown";
            runtime.detail = runtime_result.error;
        }
    } else {
        runtime.status = "stopped";
    }
    
    print_service_status(service, loaded, command, runtime, json);
    
    return core::Result<void>::ok();
}

// ============================================================================
// DaemonLogsCommand
// ============================================================================

std::vector<CommandOption> DaemonLogsCommand::get_options() const {
    return {
        {"follow", "-f", "--follow", "Follow log output", "false", false},
        {"lines", "-n", "--lines", "Number of lines", "100", false},
        {"help", "-h", "--help", "Show help", "false", false}
    };
}

void DaemonLogsCommand::print_help() const {
    std::cout << CommandParser::format_help(
        "daemon logs",
        "View HunterClaw daemon service logs",
        get_options()
    );
}

core::Result<void> DaemonLogsCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (parse_result.is_err()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    CommandArgs cmd_args = parse_result.value;
    
    if (cmd_args.get_bool("help")) {
        print_help();
        return core::Result<void>::ok();
    }
    
    bool follow = cmd_args.get_bool("follow");
    int lines = cmd_args.get_int("lines", 100);
    
#ifdef __APPLE__
    std::string log_path = core::utils::expand_path("~/Library/Logs/HunterClaw/daemon.log");
    if (!core::utils::file_exists(log_path)) {
        std::cerr << "\033[31mError:\033[0m Log file not found: " << log_path << "\n";
        return core::Result<void>::err("Log file not found");
    }
    
    std::string cmd = follow ? "tail -f -n " + std::to_string(lines) + " " + log_path
                             : "tail -n " + std::to_string(lines) + " " + log_path;
    return std::system(cmd.c_str()) == 0 ? core::Result<void>::ok() : core::Result<void>::err("Failed to read logs");
    
#elif __linux__
    std::string cmd = "journalctl --user -u hunterclaw -n " + std::to_string(lines);
    if (follow) {
        cmd += " -f";
    }
    return std::system(cmd.c_str()) == 0 ? core::Result<void>::ok() : core::Result<void>::err("Failed to read logs");
    
#elif _WIN32
    std::string log_path = core::utils::expand_path("%USERPROFILE%\\.hunterclaw\\logs\\daemon.log");
    if (!core::utils::file_exists(log_path)) {
        std::cerr << "Error: Log file not found: " << log_path << "\n";
        return core::Result<void>::err("Log file not found");
    }
    
    // Windows: use PowerShell Get-Content
    std::string cmd = follow ? "powershell Get-Content " + log_path + " -Tail " + std::to_string(lines) + " -Wait"
                             : "powershell Get-Content " + log_path + " -Tail " + std::to_string(lines);
    return std::system(cmd.c_str()) == 0 ? core::Result<void>::ok() : core::Result<void>::err("Failed to read logs");
    
#else
    std::cerr << "\033[31mError:\033[0m Log viewing not supported on this platform\n";
    return core::Result<void>::err("Unsupported platform");
#endif
}

// ============================================================================
// Registration
// ============================================================================

void register_daemon_commands() {
    auto& registry = CommandRegistry::instance();
    
    registry.register_command(std::make_shared<DaemonInstallCommand>());
    registry.register_command(std::make_shared<DaemonUninstallCommand>());
    registry.register_command(std::make_shared<DaemonStartCommand>());
    registry.register_command(std::make_shared<DaemonStopCommand>());
    registry.register_command(std::make_shared<DaemonRestartCommand>());
    registry.register_command(std::make_shared<DaemonStatusCommand>());
    registry.register_command(std::make_shared<DaemonLogsCommand>());
}

} // namespace cli
} // namespace hunterclaw
