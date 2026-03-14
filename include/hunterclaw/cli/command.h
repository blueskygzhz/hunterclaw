#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>

namespace hunterclaw {
namespace cli {

/**
 * Base command interface
 * Aligned with OpenClaw's command pattern
 */
class Command {
public:
    virtual ~Command() = default;
    
    virtual core::Result<void> execute(const std::vector<std::string>& args) = 0;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    virtual void print_help() const = 0;
};

/**
 * Command option
 */
struct CommandOption {
    std::string name;           // e.g. "port"
    std::string flag_short;     // e.g. "-p"
    std::string flag_long;      // e.g. "--port"
    std::string description;
    std::string default_value;
    bool required = false;
};

/**
 * Parsed command line arguments
 */
struct CommandArgs {
    std::map<std::string, std::string> options;
    std::vector<std::string> positional;
    
    bool has_option(const std::string& name) const {
        return options.find(name) != options.end();
    }
    
    std::string get_option(const std::string& name, const std::string& default_value = "") const {
        auto it = options.find(name);
        return it != options.end() ? it->second : default_value;
    }
    
    bool get_bool(const std::string& name, bool default_value = false) const {
        auto it = options.find(name);
        if (it == options.end()) return default_value;
        const std::string& val = it->second;
        return val == "true" || val == "1" || val == "yes";
    }
    
    int get_int(const std::string& name, int default_value = 0) const {
        auto it = options.find(name);
        if (it == options.end()) return default_value;
        try {
            return std::stoi(it->second);
        } catch (...) {
            return default_value;
        }
    }
};

/**
 * Command parser
 */
class CommandParser {
public:
    static core::Result<CommandArgs> parse(
        const std::vector<std::string>& args,
        const std::vector<CommandOption>& options
    );
    
    static std::string format_help(
        const std::string& command_name,
        const std::string& description,
        const std::vector<CommandOption>& options
    );
};

/**
 * Command registry
 * Manages all CLI commands
 */
class CommandRegistry {
public:
    static CommandRegistry& instance();
    
    void register_command(std::shared_ptr<Command> command);
    core::Result<void> execute(const std::string& command_name, const std::vector<std::string>& args);
    
    std::vector<std::string> list_commands() const;
    std::shared_ptr<Command> get_command(const std::string& name) const;
    
    void print_help() const;
    
private:
    CommandRegistry() = default;
    std::map<std::string, std::shared_ptr<Command>> commands_;
};

/**
 * JSON output helper
 */
struct JsonOutput {
    bool ok = true;
    std::string result;
    std::string message;
    std::vector<std::string> warnings;
    std::map<std::string, std::string> data;
    
    std::string to_json() const;
    void print() const;
};

} // namespace cli
} // namespace hunterclaw
