#include "hunterclaw/cli/gateway_commands.h"
#include "hunterclaw/cli/command.h"
#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/core/json.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>

namespace hunterclaw {
namespace cli {

namespace {
    // Gateway 默认配置
    const std::string DEFAULT_HOST = "localhost";
    const int DEFAULT_PORT = 18789;
    const std::string DEFAULT_CONFIG_FILE = "~/.hunterclaw/gateway.json";
    
    // 获取 Gateway 运行时路径
    std::string get_runtime_dir() {
        const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
        if (xdg_runtime) {
            return std::string(xdg_runtime) + "/hunterclaw";
        }
        
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + "/.hunterclaw/runtime";
        }
        
        return "/tmp/hunterclaw";
    }
    
    // 获取 PID 文件路径
    std::string get_pid_file() {
        return get_runtime_dir() + "/gateway.pid";
    }
    
    // 获取日志文件路径
    std::string get_log_file() {
        return get_runtime_dir() + "/gateway.log";
    }
    
    // 检查 Gateway 是否运行
    core::Result<bool> is_gateway_running() {
        auto http_client = infra::HttpClient::create();
        std::string url = "http://" + DEFAULT_HOST + ":" + std::to_string(DEFAULT_PORT) + "/health";
        
        auto response = http_client->get(url, 1000);  // 1s timeout
        if (!response.is_ok()) {
            return core::Result<bool>::ok(false);
        }
        
        return core::Result<bool>::ok(response.value().ok);
    }
    
    // 读取 PID 文件
    core::Result<int> read_pid_file() {
        std::ifstream file(get_pid_file());
        if (!file.is_open()) {
            return core::Result<int>::err("PID file not found");
        }
        
        int pid;
        file >> pid;
        return core::Result<int>::ok(pid);
    }
    
    // 写入 PID 文件
    core::Result<void> write_pid_file(int pid) {
        std::filesystem::create_directories(get_runtime_dir());
        
        std::ofstream file(get_pid_file());
        if (!file.is_open()) {
            return core::Result<void>::err("Failed to write PID file");
        }
        
        file << pid;
        return core::Result<void>::ok();
    }
    
    // 删除 PID 文件
    void remove_pid_file() {
        std::filesystem::remove(get_pid_file());
    }
}

// GatewayStartCommand implementation
std::vector<CommandOption> GatewayStartCommand::get_options() const {
    return {
        {"host", "-h", "--host", "Gateway host", DEFAULT_HOST, false},
        {"port", "-p", "--port", "Gateway port", std::to_string(DEFAULT_PORT), false},
        {"config", "-c", "--config", "Config file path", DEFAULT_CONFIG_FILE, false},
        {"daemon", "-d", "--daemon", "Run as daemon", "false", false},
        {"verbose", "-v", "--verbose", "Verbose logging", "false", false}
    };
}

core::Result<void> GatewayStartCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (!parse_result.is_ok()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    auto cmd_args = parse_result.value();
    
    // Check if already running
    auto running_result = is_gateway_running();
    if (running_result.is_ok() && running_result.value()) {
        JsonOutput output{false, "error", "Gateway already running"};
        output.print();
        return core::Result<void>::err("Gateway already running");
    }
    
    // Extract options
    std::string host = cmd_args.options["host"];
    int port = std::stoi(cmd_args.options["port"]);
    std::string config_file = cmd_args.options["config"];
    bool daemon = cmd_args.options["daemon"] == "true";
    bool verbose = cmd_args.options["verbose"] == "true";
    
    // Create Gateway configuration
    gateway::GatewayConfig config;
    config.host = host;
    config.port = port;
    config.enable_control_ui = true;
    config.enable_openai_adapter = true;
    
    // Load config file if exists
    if (std::filesystem::exists(config_file)) {
        std::ifstream file(config_file);
        // TODO: Parse JSON config
    }
    
    if (daemon) {
        // Fork to background
        pid_t pid = fork();
        if (pid < 0) {
            return core::Result<void>::err("Failed to fork daemon");
        }
        
        if (pid > 0) {
            // Parent process
            write_pid_file(pid);
            
            JsonOutput output{true, "started", "Gateway started successfully"};
            output.data["pid"] = std::to_string(pid);
            output.data["host"] = host;
            output.data["port"] = std::to_string(port);
            output.print();
            
            return core::Result<void>::ok();
        }
        
        // Child process continues...
        setsid();
        
        // Redirect stdout/stderr to log file
        std::filesystem::create_directories(get_runtime_dir());
        freopen(get_log_file().c_str(), "a", stdout);
        freopen(get_log_file().c_str(), "a", stderr);
    }
    
    // Start Gateway
    std::cout << "Starting Gateway at " << host << ":" << port << "...\n";
    
    auto gateway = gateway::Gateway::create(config);
    if (!gateway) {
        return core::Result<void>::err("Failed to create Gateway");
    }
    
    auto start_result = gateway->start();
    if (!start_result.is_ok()) {
        return core::Result<void>::err("Failed to start Gateway: " + start_result.error);
    }
    
    if (!daemon) {
        JsonOutput output{true, "started", "Gateway started successfully"};
        output.data["host"] = host;
        output.data["port"] = std::to_string(port);
        output.print();
    }
    
    // Keep running
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return core::Result<void>::ok();
}

// GatewayStopCommand implementation
std::vector<CommandOption> GatewayStopCommand::get_options() const {
    return {
        {"force", "-f", "--force", "Force stop", "false", false}
    };
}

core::Result<void> GatewayStopCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (!parse_result.is_ok()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    auto cmd_args = parse_result.value();
    bool force = cmd_args.options["force"] == "true";
    
    // Check if running
    auto running_result = is_gateway_running();
    if (!running_result.is_ok() || !running_result.value()) {
        JsonOutput output{false, "error", "Gateway not running"};
        output.print();
        return core::Result<void>::err("Gateway not running");
    }
    
    // Read PID
    auto pid_result = read_pid_file();
    if (!pid_result.is_ok()) {
        JsonOutput output{false, "error", "Failed to read PID file"};
        output.print();
        return core::Result<void>::err("Failed to read PID file");
    }
    
    int pid = pid_result.value();
    
    // Send shutdown signal
    int signal = force ? SIGKILL : SIGTERM;
    if (kill(pid, signal) != 0) {
        JsonOutput output{false, "error", "Failed to stop Gateway"};
        output.print();
        return core::Result<void>::err("Failed to stop Gateway");
    }
    
    // Wait for shutdown
    for (int i = 0; i < 30; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        auto running = is_gateway_running();
        if (!running.is_ok() || !running.value()) {
            remove_pid_file();
            
            JsonOutput output{true, "stopped", "Gateway stopped successfully"};
            output.print();
            return core::Result<void>::ok();
        }
    }
    
    JsonOutput output{false, "error", "Gateway stop timeout"};
    output.print();
    return core::Result<void>::err("Gateway stop timeout");
}

// GatewayRestartCommand implementation
std::vector<CommandOption> GatewayRestartCommand::get_options() const {
    return {};
}

core::Result<void> GatewayRestartCommand::execute(const std::vector<std::string>& args) {
    // Stop first
    GatewayStopCommand stop_cmd;
    auto stop_result = stop_cmd.execute({});
    if (!stop_result.is_ok()) {
        // Ignore error if not running
    }
    
    // Wait a bit
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Start
    GatewayStartCommand start_cmd;
    return start_cmd.execute(args);
}

// GatewayStatusCommand implementation
std::vector<CommandOption> GatewayStatusCommand::get_options() const {
    return {
        {"json", "-j", "--json", "Output as JSON", "true", false}
    };
}

core::Result<void> GatewayStatusCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (!parse_result.is_ok()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    // Check if running
    auto running_result = is_gateway_running();
    bool is_running = running_result.is_ok() && running_result.value();
    
    JsonOutput output{is_running, is_running ? "running" : "stopped", ""};
    
    if (is_running) {
        auto pid_result = read_pid_file();
        if (pid_result.is_ok()) {
            output.data["pid"] = std::to_string(pid_result.value());
        }
        
        // Get status from Gateway
        auto http_client = infra::HttpClient::create();
        std::string url = "http://" + DEFAULT_HOST + ":" + std::to_string(DEFAULT_PORT) + "/status";
        
        auto response = http_client->get(url, 1000);
        if (response.is_ok()) {
            output.data["host"] = DEFAULT_HOST;
            output.data["port"] = std::to_string(DEFAULT_PORT);
            output.message = "Gateway is running";
        }
    } else {
        output.message = "Gateway is not running";
    }
    
    output.print();
    return core::Result<void>::ok();
}

// GatewayLogsCommand implementation
std::vector<CommandOption> GatewayLogsCommand::get_options() const {
    return {
        {"lines", "-n", "--lines", "Number of lines to show", "50", false},
        {"follow", "-f", "--follow", "Follow log output", "false", false}
    };
}

core::Result<void> GatewayLogsCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (!parse_result.is_ok()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    auto cmd_args = parse_result.value();
    int lines = std::stoi(cmd_args.options["lines"]);
    bool follow = cmd_args.options["follow"] == "true";
    
    std::string log_file = get_log_file();
    if (!std::filesystem::exists(log_file)) {
        JsonOutput output{false, "error", "Log file not found"};
        output.print();
        return core::Result<void>::err("Log file not found");
    }
    
    // Use tail command
    std::string cmd = "tail ";
    if (follow) {
        cmd += "-f ";
    }
    cmd += "-n " + std::to_string(lines) + " " + log_file;
    
    system(cmd.c_str());
    
    return core::Result<void>::ok();
}

// GatewayConfigCommand implementation
std::vector<CommandOption> GatewayConfigCommand::get_options() const {
    return {
        {"get", "", "--get", "Get config value", "", false},
        {"set", "", "--set", "Set config value", "", false},
        {"list", "", "--list", "List all config", "false", false}
    };
}

core::Result<void> GatewayConfigCommand::execute(const std::vector<std::string>& args) {
    auto parse_result = CommandParser::parse(args, get_options());
    if (!parse_result.is_ok()) {
        return core::Result<void>::err(parse_result.error);
    }
    
    auto cmd_args = parse_result.value();
    
    // TODO: Implement config management
    JsonOutput output{false, "error", "Config management not implemented yet"};
    output.print();
    
    return core::Result<void>::err("Not implemented");
}

// GatewayHealthCommand implementation
std::vector<CommandOption> GatewayHealthCommand::get_options() const {
    return {};
}

core::Result<void> GatewayHealthCommand::execute(const std::vector<std::string>& args) {
    auto running_result = is_gateway_running();
    if (!running_result.is_ok() || !running_result.value()) {
        JsonOutput output{false, "unhealthy", "Gateway not running"};
        output.print();
        return core::Result<void>::err("Gateway not running");
    }
    
    // Get health status from Gateway
    auto http_client = infra::HttpClient::create();
    std::string url = "http://" + DEFAULT_HOST + ":" + std::to_string(DEFAULT_PORT) + "/health";
    
    auto response = http_client->get(url, 2000);
    if (!response.is_ok()) {
        JsonOutput output{false, "unhealthy", "Failed to get health status"};
        output.print();
        return core::Result<void>::err("Failed to get health status");
    }
    
    JsonOutput output{true, "healthy", "Gateway is healthy"};
    output.data["status_code"] = std::to_string(response.value().status_code);
    output.print();
    
    return core::Result<void>::ok();
}

// Register all Gateway commands
void register_gateway_commands() {
    auto& registry = CommandRegistry::instance();
    
    registry.register_command(std::make_shared<GatewayStartCommand>());
    registry.register_command(std::make_shared<GatewayStopCommand>());
    registry.register_command(std::make_shared<GatewayRestartCommand>());
    registry.register_command(std::make_shared<GatewayStatusCommand>());
    registry.register_command(std::make_shared<GatewayLogsCommand>());
    registry.register_command(std::make_shared<GatewayConfigCommand>());
    registry.register_command(std::make_shared<GatewayHealthCommand>());
}

} // namespace cli
} // namespace hunterclaw
