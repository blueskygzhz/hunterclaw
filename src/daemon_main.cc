#include "hunterclaw/daemon/daemon_service.h"
#include <iostream>
#include <cstring>

using namespace hunterclaw::daemon;

void print_help() {
    std::cout << "\nUsage: hunterclaw-daemon [command] [options]\n\n";
    std::cout << "Available Commands:\n";
    std::cout << "  install        Install gateway as system service\n";
    std::cout << "  uninstall      Uninstall gateway service\n";
    std::cout << "  start          Start gateway service\n";
    std::cout << "  stop           Stop gateway service\n";
    std::cout << "  restart        Restart gateway service\n";
    std::cout << "  status         Show gateway service status\n";
    std::cout << "  version        Show version information\n";
    std::cout << "  help           Show this help message\n\n";
    std::cout << "Options:\n";
    std::cout << "  --profile, -p  Service profile (default: default)\n";
    std::cout << "  --help, -h     Show this help message\n";
    std::cout << "  --version, -v  Show version information\n\n";
    std::cout << "Platform Detection:\n";
    
    Platform platform = DetectPlatform();
    switch (platform) {
        case Platform::kMacOS:
            std::cout << "  Current: macOS (launchd)\n";
            break;
        case Platform::kLinux:
            std::cout << "  Current: Linux (systemd)\n";
            break;
        case Platform::kWindows:
            std::cout << "  Current: Windows (Task Scheduler)\n";
            break;
    }
    
    std::cout << "\nFor more information, visit: https://github.com/hunterclaw\n\n";
}

void print_version() {
    std::cout << "\nHunterClaw Daemon Manager version 0.1.0\n";
    std::cout << "High-Performance AI Agent Platform (C++)\n";
    std::cout << "Cross-platform service management for HunterClaw Gateway\n";
    
    Platform platform = DetectPlatform();
    std::cout << "\nPlatform: ";
    switch (platform) {
        case Platform::kMacOS:
            std::cout << "macOS (launchd)";
            break;
        case Platform::kLinux:
            std::cout << "Linux (systemd)";
            break;
        case Platform::kWindows:
            std::cout << "Windows (Task Scheduler)";
            break;
    }
    std::cout << "\n\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "\nError: No command specified\n";
        std::cerr << "Run 'hunterclaw-daemon --help' for usage information\n\n";
        return 1;
    }
    
    std::string command = argv[1];
    
    // Parse flags
    if (command == "--help" || command == "-h" || command == "help") {
        print_help();
        return 0;
    }
    
    if (command == "--version" || command == "-v" || command == "version") {
        print_version();
        return 0;
    }
    
    // Service commands (not fully implemented yet)
    if (command == "install" || command == "uninstall" || 
        command == "start" || command == "stop" || 
        command == "restart" || command == "status") {
        
        std::cout << "\nService Management Commands\n";
        std::cout << "==========================\n\n";
        std::cout << "Command: " << command << "\n";
        
        try {
            auto service = CreateDaemonServiceForCurrentPlatform();
            Platform platform = DetectPlatform();
            
            std::cout << "Platform: ";
            switch (platform) {
                case Platform::kMacOS:
                    std::cout << "macOS (launchd)\n";
                    break;
                case Platform::kLinux:
                    std::cout << "Linux (systemd)\n";
                    break;
                case Platform::kWindows:
                    std::cout << "Windows (Task Scheduler)\n";
                    break;
            }
            
            std::cout << "\nNote: Service management features require gateway binary path\n";
            std::cout << "Full implementation pending. Use --help for more information.\n\n";
            
        } catch (const std::exception& e) {
            std::cerr << "\nError: " << e.what() << "\n\n";
            return 1;
        }
        
        return 0;
    }
    
    // Unknown command
    std::cerr << "\nError: Unknown command '" << command << "'\n";
    std::cerr << "Run 'hunterclaw-daemon --help' for usage information\n\n";
    return 1;
}
