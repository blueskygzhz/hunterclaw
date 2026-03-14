#include "hunterclaw/cli/command.h"
#include <iostream>
#include <sstream>
#include <iomanip>

namespace hunterclaw {
namespace cli {

// CommandParser implementation
core::Result<CommandArgs> CommandParser::parse(
    const std::vector<std::string>& args,
    const std::vector<CommandOption>& options
) {
    CommandArgs result;
    
    // Build option lookup maps
    std::map<std::string, const CommandOption*> long_flags;
    std::map<std::string, const CommandOption*> short_flags;
    
    for (const auto& opt : options) {
        if (!opt.flag_long.empty()) {
            long_flags[opt.flag_long] = &opt;
        }
        if (!opt.flag_short.empty()) {
            short_flags[opt.flag_short] = &opt;
        }
    }
    
    // Parse arguments
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];
        
        if (arg.substr(0, 2) == "--") {
            // Long flag
            auto it = long_flags.find(arg);
            if (it == long_flags.end()) {
                return core::Result<CommandArgs>::err("Unknown option: " + arg);
            }
            
            const CommandOption* opt = it->second;
            if (i + 1 >= args.size()) {
                return core::Result<CommandArgs>::err("Missing value for option: " + arg);
            }
            
            result.options[opt->name] = args[++i];
            
        } else if (arg.substr(0, 1) == "-" && arg != "-") {
            // Short flag
            auto it = short_flags.find(arg);
            if (it == short_flags.end()) {
                return core::Result<CommandArgs>::err("Unknown option: " + arg);
            }
            
            const CommandOption* opt = it->second;
            if (i + 1 >= args.size()) {
                return core::Result<CommandArgs>::err("Missing value for option: " + arg);
            }
            
            result.options[opt->name] = args[++i];
            
        } else {
            // Positional argument
            result.positional.push_back(arg);
        }
    }
    
    // Apply defaults
    for (const auto& opt : options) {
        if (result.options.find(opt.name) == result.options.end() && !opt.default_value.empty()) {
            result.options[opt.name] = opt.default_value;
        }
    }
    
    // Check required options
    for (const auto& opt : options) {
        if (opt.required && result.options.find(opt.name) == result.options.end()) {
            return core::Result<CommandArgs>::err("Required option missing: " + opt.flag_long);
        }
    }
    
    return core::Result<CommandArgs>::ok(result);
}

std::string CommandParser::format_help(
    const std::string& command_name,
    const std::string& description,
    const std::vector<CommandOption>& options
) {
    std::ostringstream oss;
    
    oss << "Usage: hunterclaw " << command_name << " [options]\n";
    oss << "\n";
    oss << description << "\n";
    oss << "\n";
    
    if (!options.empty()) {
        oss << "Options:\n";
        
        for (const auto& opt : options) {
            oss << "  ";
            
            if (!opt.flag_short.empty()) {
                oss << opt.flag_short << ", ";
            } else {
                oss << "    ";
            }
            
            oss << std::left << std::setw(20) << opt.flag_long;
            oss << opt.description;
            
            if (!opt.default_value.empty()) {
                oss << " (default: " << opt.default_value << ")";
            }
            if (opt.required) {
                oss << " [required]";
            }
            
            oss << "\n";
        }
    }
    
    return oss.str();
}

// CommandRegistry implementation
CommandRegistry& CommandRegistry::instance() {
    static CommandRegistry registry;
    return registry;
}

void CommandRegistry::register_command(std::shared_ptr<Command> command) {
    commands_[command->name()] = command;
}

core::Result<void> CommandRegistry::execute(const std::string& command_name, const std::vector<std::string>& args) {
    auto it = commands_.find(command_name);
    if (it == commands_.end()) {
        return core::Result<void>::err("Unknown command: " + command_name);
    }
    
    return it->second->execute(args);
}

std::vector<std::string> CommandRegistry::list_commands() const {
    std::vector<std::string> result;
    for (const auto& pair : commands_) {
        result.push_back(pair.first);
    }
    return result;
}

std::shared_ptr<Command> CommandRegistry::get_command(const std::string& name) const {
    auto it = commands_.find(name);
    return it != commands_.end() ? it->second : nullptr;
}

void CommandRegistry::print_help() const {
    std::cout << "HunterClaw CLI Commands:\n\n";
    
    for (const auto& pair : commands_) {
        std::cout << "  " << std::left << std::setw(20) << pair.first;
        std::cout << pair.second->description() << "\n";
    }
    
    std::cout << "\nRun 'hunterclaw <command> --help' for more information on a command.\n";
}

// JsonOutput implementation
std::string JsonOutput::to_json() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
    oss << "  \"result\": \"" << result << "\",\n";
    oss << "  \"message\": \"" << message << "\"";
    
    if (!warnings.empty()) {
        oss << ",\n  \"warnings\": [";
        for (size_t i = 0; i < warnings.size(); i++) {
            if (i > 0) oss << ", ";
            oss << "\"" << warnings[i] << "\"";
        }
        oss << "]";
    }
    
    if (!data.empty()) {
        oss << ",\n  \"data\": {\n";
        size_t idx = 0;
        for (const auto& pair : data) {
            if (idx++ > 0) oss << ",\n";
            oss << "    \"" << pair.first << "\": \"" << pair.second << "\"";
        }
        oss << "\n  }";
    }
    
    oss << "\n}";
    return oss.str();
}

void JsonOutput::print() const {
    std::cout << to_json() << std::endl;
}

} // namespace cli
} // namespace hunterclaw
