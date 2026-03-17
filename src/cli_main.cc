#include "hunterclaw/core/types.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/cli/command.h"
#include <iostream>
#include <vector>
#include <string>

using namespace hunterclaw;

void print_version() {
    std::cout << "HunterClaw CLI version 0.1.0" << std::endl;
    std::cout << "High-Performance AI Agent Platform (C++)" << std::endl;
    std::cout << "Compatible with OpenClaw" << std::endl;
}

void print_help() {
    std::cout << "Usage: hunterclaw-cli [command] [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Available Commands:" << std::endl;
    std::cout << "  version        Show version information" << std::endl;
    std::cout << "  help           Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --help, -h     Show this help message" << std::endl;
    std::cout << "  --version, -v  Show version information" << std::endl;
    std::cout << std::endl;
    std::cout << "For more information, visit: https://github.com/hunterclaw" << std::endl;
}

int main(int argc, char** argv) {
    // Parse command line arguments
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        args.push_back(argv[i]);
    }
    
    // Handle empty arguments
    if (args.empty()) {
        print_help();
        return 0;
    }
    
    // Handle commands
    std::string command = args[0];
    
    if (command == "--help" || command == "-h" || command == "help") {
        print_help();
        return 0;
    }
    
    if (command == "--version" || command == "-v" || command == "version") {
        print_version();
        return 0;
    }
    
    // Unknown command
    std::cerr << "Error: Unknown command '" << command << "'" << std::endl;
    std::cerr << "Run 'hunterclaw-cli --help' for usage information" << std::endl;
    return 1;
}
