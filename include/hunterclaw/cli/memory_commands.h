#pragma once

#include "hunterclaw/cli/command.h"

namespace hunterclaw {
namespace cli {

// ============================================================================
// Memory CLI 命令 (对齐 OpenClaw)
// ============================================================================

class MemorySearchCommand : public Command {
public:
    std::string name() const override { return "memory:search"; }
    std::string description() const override { 
        return "Search memory files semantically";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class MemoryGetCommand : public Command {
public:
    std::string name() const override { return "memory:get"; }
    std::string description() const override { 
        return "Get memory file content";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class MemoryListCommand : public Command {
public:
    std::string name() const override { return "memory:list"; }
    std::string description() const override { 
        return "List all memory files";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class MemoryStatsCommand : public Command {
public:
    std::string name() const override { return "memory:stats"; }
    std::string description() const override { 
        return "Show memory system statistics";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class MemoryCompactCommand : public Command {
public:
    std::string name() const override { return "memory:compact"; }
    std::string description() const override { 
        return "Compact memory files";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class MemoryReindexCommand : public Command {
public:
    std::string name() const override { return "memory:reindex"; }
    std::string description() const override { 
        return "Reindex memory embeddings";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

class MemoryConfigCommand : public Command {
public:
    std::string name() const override { return "memory:config"; }
    std::string description() const override { 
        return "Show memory configuration";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

} // namespace cli
} // namespace hunterclaw
