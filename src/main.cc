#include "hunterclaw/core/types.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/manager.h"
#include "hunterclaw/agents/types.h"
// #include "hunterclaw/cli/command.h"  // Temporarily disabled
// #include "hunterclaw/cli/daemon_commands.h"  // Temporarily disabled
#include <iostream>
#include <csignal>
#include <atomic>

using namespace hunterclaw;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

void print_banner() {
    std::cout << R"(
   _   _             _            ____ _               
  | | | |_   _ _ __ | |_ ___ _ __/ ___| | __ _ __      __
  | |_| | | | | '_ \| __/ _ \ '__| |   | |/ _` \ \ /\ / /
  |  _  | |_| | | | | ||  __/ |  | |___| | (_| |\ V  V / 
  |_| |_|\__,_|_| |_|\__\___|_|   \____|_|\__,_| \_/\_/  
                                                          
  High-Performance AI Agent Platform (C++)
  Compatible with OpenClaw
  Version: 0.1.0
    )" << std::endl;
}

void print_help() {
    std::cout << "Usage: hunterclaw [command] [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  daemon install     Install HunterClaw as a system daemon" << std::endl;
    std::cout << "  daemon uninstall   Uninstall HunterClaw daemon" << std::endl;
    std::cout << "  daemon start       Start HunterClaw daemon" << std::endl;
    std::cout << "  daemon stop        Stop HunterClaw daemon" << std::endl;
    std::cout << "  daemon restart     Restart HunterClaw daemon" << std::endl;
    std::cout << "  daemon status      Show daemon status" << std::endl;
    std::cout << "  daemon logs        View daemon logs" << std::endl;
    std::cout << std::endl;
    std::cout << "Options (when running directly):" << std::endl;
    std::cout << "  --config <path>    Configuration file path (default: ~/.hunterclaw/config.json)" << std::endl;
    std::cout << "  --port <port>      Gateway port (default: 18789)" << std::endl;
    std::cout << "  --daemon           Run in daemon mode (used by daemon service)" << std::endl;
    std::cout << "  --help             Show this help message" << std::endl;
    std::cout << "  --version          Show version information" << std::endl;
    std::cout << std::endl;
    std::cout << "Run 'hunterclaw daemon <command> --help' for more information on a command." << std::endl;
}

int main(int argc, char** argv) {
    // Temporarily disabled CLI commands
    /*
    // Register daemon CLI commands
    cli::register_daemon_commands();
    
    // Check for daemon subcommands first
    if (argc >= 2 && std::string(argv[1]) == "daemon") {
        if (argc >= 3) {
            std::string subcommand = argv[2];
            
            // Build subcommand args
            std::vector<std::string> sub_args;
            for (int i = 3; i < argc; i++) {
                sub_args.push_back(argv[i]);
            }
            
            // Execute daemon subcommand
            auto& registry = cli::CommandRegistry::instance();
            auto result = registry.execute(subcommand, sub_args);
            
            if (result.is_err()) {
                std::cerr << "\033[31mError:\033[0m " << result.error << std::endl;
                return 1;
            }
            
            return 0;
        } else {
            std::cerr << "\033[31mError:\033[0m Missing daemon subcommand" << std::endl;
            std::cerr << "Run 'hunterclaw daemon --help' for available commands" << std::endl;
            return 1;
        }
    }
    */
    
    // Parse command line arguments for direct execution
    std::string config_path = core::utils::expand_path("~/.hunterclaw/config.json");
    uint16_t port = 18789;
    bool daemon_mode = false;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        }
        
        if (arg == "--version" || arg == "-v") {
            std::cout << "HunterClaw version 0.1.0" << std::endl;
            return 0;
        }
        
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
        
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
        
        if (arg == "--daemon") {
            daemon_mode = true;
        }
    }
    
    if (daemon_mode) {
        // Daemon mode: suppress banner, use structured logging
        std::cout << "[HunterClaw] Starting in daemon mode" << std::endl;
    } else {
        print_banner();
    }
    
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Load configuration
    core::Config config;
    if (core::utils::file_exists(config_path)) {
        std::cout << "Loading configuration from: " << config_path << std::endl;
        auto config_result = core::load_config(config_path);
        if (config_result.is_err()) {
            std::cerr << "Failed to load config: " << config_result.error << std::endl;
            std::cout << "Using default configuration" << std::endl;
            config = core::create_default_config();
        } else {
            config = config_result.value;
        }
    } else {
        std::cout << "Configuration file not found, using defaults" << std::endl;
        config = core::create_default_config();
        
        // Create config directory
        std::string config_dir = core::utils::expand_path("~/.hunterclaw");
        core::utils::mkdir_p(config_dir);
        
        // Save default config
        auto save_result = core::save_config(config_path, config);
        if (save_result.is_ok()) {
            std::cout << "Default configuration saved to: " << config_path << std::endl;
        }
    }
    
    // Override port if specified
    if (port != 18789) {
        config.gateway.port = port;
    }
    
    std::cout << "\n=== HunterClaw Initialization ===" << std::endl;
    
    // Initialize Plugin Manager
    std::cout << "[1/4] Initializing Plugin Manager..." << std::endl;
    auto plugin_mgr = plugins::PluginManager::create();
    
    // Load plugins from ~/.hunterclaw/plugins
    std::string plugins_dir = core::utils::expand_path("~/.hunterclaw/plugins");
    if (core::utils::file_exists(plugins_dir)) {
        // TODO: Scan and load plugins
        std::cout << "  Loading plugins from: " << plugins_dir << std::endl;
    }
    
    // Initialize Skill Manager
    std::cout << "[2/4] Initializing Skill Manager..." << std::endl;
    auto skill_mgr = skills::SkillManager::create(config.skills);
    
    auto load_result = skill_mgr->load_from_roots();
    if (load_result.is_ok()) {
        auto skills_list = skill_mgr->list();
        std::cout << "  Loaded " << skills_list.size() << " skills" << std::endl;
    } else {
        std::cout << "  Warning: Failed to load skills: " << load_result.error << std::endl;
    }
    
    // Initialize Channel Manager
    std::cout << "[3/4] Initializing Channel Manager..." << std::endl;
    auto channel_mgr = channels::ChannelManager::create();
    
    // TODO: Set up message callback when available
    // channel_mgr->set_message_callback([](const channels::ChannelMessage& msg) {
    //     std::cout << "[Channel Message] " << msg.channel_id << " - " 
    //               << msg.sender_name << ": " << msg.text << std::endl;
    // });
    
    // Initialize Gateway Server
    std::cout << "[4/4] Starting Gateway Server..." << std::endl;
    auto gateway = gateway::GatewayServer::create(config.gateway);
    
    auto start_result = gateway->start();
    if (start_result.is_err()) {
        std::cerr << "Failed to start gateway: " << start_result.error << std::endl;
        return 1;
    }
    
    std::cout << "\n==================================" << std::endl;
    std::cout << "🚀 HunterClaw is running!" << std::endl;
    std::cout << "Gateway: ws://" << config.gateway.host << ":" << config.gateway.port << std::endl;
    std::cout << "Config: " << config_path << std::endl;
    std::cout << "Skills: " << skill_mgr->list().size() << " loaded" << std::endl;
    // std::cout << "Agents: " << agent_mgr->list_agents().size() << " loaded" << std::endl;
    std::cout << "==================================" << std::endl;
    std::cout << "\nPress Ctrl+C to stop" << std::endl;
    
    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Graceful shutdown
    std::cout << "\n=== Shutting down ===" << std::endl;
    
    std::cout << "Stopping Gateway..." << std::endl;
    gateway->stop();
    
    std::cout << "Stopping Channels..." << std::endl;
    auto channels_list = channel_mgr->list_channels();
    for (const auto& channel_id : channels_list) {
        channel_mgr->stop_channel(channel_id, "default");
    }
    
    std::cout << "Unloading Plugins..." << std::endl;
    auto plugins_list = plugin_mgr->list();
    for (const auto& plugin_id : plugins_list) {
        plugin_mgr->unload(plugin_id);
    }
    
    std::cout << "Goodbye! 👋" << std::endl;
    
    return 0;
}
