#pragma once

#include "hunterclaw/cli/command.h"
#include "hunterclaw/agents/agent_manager.h"
#include <memory>

namespace hunterclaw {
namespace cli {

// ============================================================================
// Agent CLI Commands (对齐 OpenClaw)
// ============================================================================

// hunterclaw agent:list - 列出所有 agent
class AgentListCommand : public Command {
public:
    explicit AgentListCommand(std::shared_ptr<agents::AgentManager> manager);
    
    std::string name() const override { return "agent:list"; }
    std::string description() const override { return "List all agents"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<agents::AgentManager> manager_;
    bool show_json_ = false;
};

// hunterclaw agent:create <name> - 创建新 agent
class AgentCreateCommand : public Command {
public:
    explicit AgentCreateCommand(std::shared_ptr<agents::AgentManager> manager);
    
    std::string name() const override { return "agent:create"; }
    std::string description() const override { return "Create a new agent"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<agents::AgentManager> manager_;
    std::string agent_name_;
    std::string agent_type_;
    std::string model_;
    std::string description_;
};

// hunterclaw agent:delete <name> - 删除 agent
class AgentDeleteCommand : public Command {
public:
    explicit AgentDeleteCommand(std::shared_ptr<agents::AgentManager> manager);
    
    std::string name() const override { return "agent:delete"; }
    std::string description() const override { return "Delete an agent"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<agents::AgentManager> manager_;
    std::string agent_name_;
    bool force_ = false;
};

// hunterclaw agent:info <name> - 显示 agent 详情
class AgentInfoCommand : public Command {
public:
    explicit AgentInfoCommand(std::shared_ptr<agents::AgentManager> manager);
    
    std::string name() const override { return "agent:info"; }
    std::string description() const override { return "Show agent details"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<agents::AgentManager> manager_;
    std::string agent_name_;
    bool show_json_ = false;
};

// hunterclaw agent:enable <name> - 启用 agent
class AgentEnableCommand : public Command {
public:
    explicit AgentEnableCommand(std::shared_ptr<agents::AgentManager> manager);
    
    std::string name() const override { return "agent:enable"; }
    std::string description() const override { return "Enable an agent"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<agents::AgentManager> manager_;
    std::string agent_name_;
};

// hunterclaw agent:disable <name> - 禁用 agent
class AgentDisableCommand : public Command {
public:
    explicit AgentDisableCommand(std::shared_ptr<agents::AgentManager> manager);
    
    std::string name() const override { return "agent:disable"; }
    std::string description() const override { return "Disable an agent"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<agents::AgentManager> manager_;
    std::string agent_name_;
};

// hunterclaw agent:chat <name> - 与 agent 交互
class AgentChatCommand : public Command {
public:
    explicit AgentChatCommand(std::shared_ptr<agents::AgentManager> manager);
    
    std::string name() const override { return "agent:chat"; }
    std::string description() const override { return "Chat with an agent"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<agents::AgentManager> manager_;
    std::string agent_name_;
    std::string message_;
    bool streaming_ = true;
};

// ============================================================================
// Register all agent commands
// ============================================================================

void register_agent_commands(
    CLI::App& app,
    std::shared_ptr<agents::AgentManager> manager
);

} // namespace cli
} // namespace hunterclaw
