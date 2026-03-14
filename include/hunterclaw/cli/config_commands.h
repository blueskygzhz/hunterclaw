#pragma once

#include "hunterclaw/cli/command.h"

namespace hunterclaw {
namespace cli {

// ============================================================================
// Config CLI 命令 (对齐 OpenClaw)
// ============================================================================

class ConfigGetCommand : public Command {
public:
    std::string name() const override { return "config:get"; }
    std::string description() const override { 
        return "Get a configuration value";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class ConfigSetCommand : public Command {
public:
    std::string name() const override { return "config:set"; }
    std::string description() const override { 
        return "Set a configuration value";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class ConfigListCommand : public Command {
public:
    std::string name() const override { return "config:list"; }
    std::string description() const override { 
        return "List all configuration values";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class ConfigUnsetCommand : public Command {
public:
    std::string name() const override { return "config:unset"; }
    std::string description() const override { 
        return "Unset a configuration value";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class ConfigResetCommand : public Command {
public:
    std::string name() const override { return "config:reset"; }
    std::string description() const override { 
        return "Reset configuration to defaults";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class ConfigPathCommand : public Command {
public:
    std::string name() const override { return "config:path"; }
    std::string description() const override { 
        return "Show configuration file path";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class ConfigEditCommand : public Command {
public:
    std::string name() const override { return "config:edit"; }
    std::string description() const override { 
        return "Open configuration file in editor";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

} // namespace cli
} // namespace hunterclaw
