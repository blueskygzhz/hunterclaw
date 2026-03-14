#include "hunterclaw/cli/agent_commands.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/json.h"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace hunterclaw {
namespace cli {

// ============================================================================
// AgentListCommand
// ============================================================================

AgentListCommand::AgentListCommand(std::shared_ptr<agents::AgentManager> manager)
    : manager_(manager) {}

void AgentListCommand::configure(CLI::App& app) {
    app.add_flag("--json", show_json_, "Output as JSON");
}

core::Result<int> AgentListCommand::execute(const CommandContext& ctx) {
    auto agents_result = manager_->list_agents();
    if (!agents_result.success) {
        std::cerr << "Error: " << agents_result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    auto& agents = agents_result.value;
    
    if (show_json_) {
        // JSON output
        core::JsonArray json_agents;
        for (const auto& agent : agents) {
            core::JsonObject obj;
            obj["name"] = core::JsonValue(agent.name);
            obj["type"] = core::JsonValue(agent.type);
            obj["model"] = core::JsonValue(agent.model);
            obj["enabled"] = core::JsonValue(agent.enabled);
            obj["status"] = core::JsonValue(agent.status);
            json_agents.push_back(core::JsonValue(obj));
        }
        std::cout << core::JsonWriter::write(core::JsonValue(json_agents)) << std::endl;
    } else {
        // Table output
        if (agents.empty()) {
            std::cout << "No agents found." << std::endl;
            return core::Result<int>::ok(0);
        }
        
        std::cout << std::left
                  << std::setw(20) << "NAME"
                  << std::setw(15) << "TYPE"
                  << std::setw(20) << "MODEL"
                  << std::setw(10) << "ENABLED"
                  << "STATUS"
                  << std::endl;
        std::cout << std::string(80, '-') << std::endl;
        
        for (const auto& agent : agents) {
            std::cout << std::left
                      << std::setw(20) << agent.name
                      << std::setw(15) << agent.type
                      << std::setw(20) << agent.model
                      << std::setw(10) << (agent.enabled ? "yes" : "no")
                      << agent.status
                      << std::endl;
        }
    }
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// AgentCreateCommand
// ============================================================================

AgentCreateCommand::AgentCreateCommand(std::shared_ptr<agents::AgentManager> manager)
    : manager_(manager) {}

void AgentCreateCommand::configure(CLI::App& app) {
    app.add_option("name", agent_name_, "Agent name")
        ->required();
    app.add_option("--type,-t", agent_type_, "Agent type (react/acp)")
        ->default_val("react");
    app.add_option("--model,-m", model_, "Model to use")
        ->default_val("claude-3-5-sonnet");
    app.add_option("--description,-d", description_, "Agent description");
}

core::Result<int> AgentCreateCommand::execute(const CommandContext& ctx) {
    agents::AgentConfig config;
    config.name = agent_name_;
    config.type = agent_type_;
    config.model = model_;
    config.description = description_;
    config.enabled = true;
    
    auto result = manager_->create_agent(config);
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    std::cout << "✓ Agent '" << agent_name_ << "' created successfully" << std::endl;
    std::cout << "  Type: " << agent_type_ << std::endl;
    std::cout << "  Model: " << model_ << std::endl;
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// AgentDeleteCommand
// ============================================================================

AgentDeleteCommand::AgentDeleteCommand(std::shared_ptr<agents::AgentManager> manager)
    : manager_(manager) {}

void AgentDeleteCommand::configure(CLI::App& app) {
    app.add_option("name", agent_name_, "Agent name")
        ->required();
    app.add_flag("--force,-f", force_, "Force deletion without confirmation");
}

core::Result<int> AgentDeleteCommand::execute(const CommandContext& ctx) {
    // Confirm deletion
    if (!force_) {
        std::cout << "Delete agent '" << agent_name_ << "'? [y/N] ";
        std::string response;
        std::getline(std::cin, response);
        
        if (response != "y" && response != "Y") {
            std::cout << "Cancelled." << std::endl;
            return core::Result<int>::ok(0);
        }
    }
    
    auto result = manager_->delete_agent(agent_name_);
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    std::cout << "✓ Agent '" << agent_name_ << "' deleted" << std::endl;
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// AgentInfoCommand
// ============================================================================

AgentInfoCommand::AgentInfoCommand(std::shared_ptr<agents::AgentManager> manager)
    : manager_(manager) {}

void AgentInfoCommand::configure(CLI::App& app) {
    app.add_option("name", agent_name_, "Agent name")
        ->required();
    app.add_flag("--json", show_json_, "Output as JSON");
}

core::Result<int> AgentInfoCommand::execute(const CommandContext& ctx) {
    auto agent_result = manager_->get_agent(agent_name_);
    if (!agent_result.success) {
        std::cerr << "Error: " << agent_result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    auto& agent = agent_result.value;
    
    if (show_json_) {
        // JSON output
        core::JsonObject obj;
        obj["name"] = core::JsonValue(agent.name);
        obj["type"] = core::JsonValue(agent.type);
        obj["model"] = core::JsonValue(agent.model);
        obj["description"] = core::JsonValue(agent.description);
        obj["enabled"] = core::JsonValue(agent.enabled);
        obj["status"] = core::JsonValue(agent.status);
        obj["created_at"] = core::JsonValue(agent.created_at);
        obj["updated_at"] = core::JsonValue(agent.updated_at);
        
        std::cout << core::JsonWriter::write(core::JsonValue(obj), true) << std::endl;
    } else {
        // Human-readable output
        std::cout << "Agent: " << agent.name << std::endl;
        std::cout << "  Type:        " << agent.type << std::endl;
        std::cout << "  Model:       " << agent.model << std::endl;
        std::cout << "  Description: " << agent.description << std::endl;
        std::cout << "  Enabled:     " << (agent.enabled ? "yes" : "no") << std::endl;
        std::cout << "  Status:      " << agent.status << std::endl;
        std::cout << "  Created:     " << agent.created_at << std::endl;
        std::cout << "  Updated:     " << agent.updated_at << std::endl;
        
        if (!agent.tools.empty()) {
            std::cout << "  Tools:       ";
            for (size_t i = 0; i < agent.tools.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << agent.tools[i];
            }
            std::cout << std::endl;
        }
        
        if (!agent.skills.empty()) {
            std::cout << "  Skills:      ";
            for (size_t i = 0; i < agent.skills.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << agent.skills[i];
            }
            std::cout << std::endl;
        }
    }
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// AgentEnableCommand
// ============================================================================

AgentEnableCommand::AgentEnableCommand(std::shared_ptr<agents::AgentManager> manager)
    : manager_(manager) {}

void AgentEnableCommand::configure(CLI::App& app) {
    app.add_option("name", agent_name_, "Agent name")
        ->required();
}

core::Result<int> AgentEnableCommand::execute(const CommandContext& ctx) {
    auto result = manager_->enable_agent(agent_name_);
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    std::cout << "✓ Agent '" << agent_name_ << "' enabled" << std::endl;
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// AgentDisableCommand
// ============================================================================

AgentDisableCommand::AgentDisableCommand(std::shared_ptr<agents::AgentManager> manager)
    : manager_(manager) {}

void AgentDisableCommand::configure(CLI::App& app) {
    app.add_option("name", agent_name_, "Agent name")
        ->required();
}

core::Result<int> AgentDisableCommand::execute(const CommandContext& ctx) {
    auto result = manager_->disable_agent(agent_name_);
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    std::cout << "✓ Agent '" << agent_name_ << "' disabled" << std::endl;
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// AgentChatCommand
// ============================================================================

AgentChatCommand::AgentChatCommand(std::shared_ptr<agents::AgentManager> manager)
    : manager_(manager) {}

void AgentChatCommand::configure(CLI::App& app) {
    app.add_option("name", agent_name_, "Agent name")
        ->required();
    app.add_option("message", message_, "Message to send")
        ->required();
    app.add_flag("--no-streaming", streaming_, "Disable streaming output")
        ->default_val(false);
}

core::Result<int> AgentChatCommand::execute(const CommandContext& ctx) {
    std::cout << "→ " << message_ << std::endl;
    std::cout << std::endl;
    
    if (streaming_) {
        // Streaming mode
        std::cout << agent_name_ << ": " << std::flush;
        
        auto result = manager_->chat_streaming(
            agent_name_,
            message_,
            [](const std::string& chunk) {
                std::cout << chunk << std::flush;
            }
        );
        
        std::cout << std::endl;
        
        if (!result.success) {
            std::cerr << "Error: " << result.error_message << std::endl;
            return core::Result<int>::ok(1);
        }
    } else {
        // Non-streaming mode
        auto result = manager_->chat(agent_name_, message_);
        if (!result.success) {
            std::cerr << "Error: " << result.error_message << std::endl;
            return core::Result<int>::ok(1);
        }
        
        std::cout << agent_name_ << ": " << result.value << std::endl;
    }
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// Registration
// ============================================================================

void register_agent_commands(
    CLI::App& app,
    std::shared_ptr<agents::AgentManager> manager
) {
    // Register all agent commands
    auto agent_list = std::make_shared<AgentListCommand>(manager);
    auto agent_create = std::make_shared<AgentCreateCommand>(manager);
    auto agent_delete = std::make_shared<AgentDeleteCommand>(manager);
    auto agent_info = std::make_shared<AgentInfoCommand>(manager);
    auto agent_enable = std::make_shared<AgentEnableCommand>(manager);
    auto agent_disable = std::make_shared<AgentDisableCommand>(manager);
    auto agent_chat = std::make_shared<AgentChatCommand>(manager);
    
    // Configure subcommands
    auto list_cmd = app.add_subcommand("agent:list", agent_list->description());
    agent_list->configure(*list_cmd);
    list_cmd->callback([agent_list]() {
        CommandContext ctx;
        agent_list->execute(ctx);
    });
    
    auto create_cmd = app.add_subcommand("agent:create", agent_create->description());
    agent_create->configure(*create_cmd);
    create_cmd->callback([agent_create]() {
        CommandContext ctx;
        agent_create->execute(ctx);
    });
    
    auto delete_cmd = app.add_subcommand("agent:delete", agent_delete->description());
    agent_delete->configure(*delete_cmd);
    delete_cmd->callback([agent_delete]() {
        CommandContext ctx;
        agent_delete->execute(ctx);
    });
    
    auto info_cmd = app.add_subcommand("agent:info", agent_info->description());
    agent_info->configure(*info_cmd);
    info_cmd->callback([agent_info]() {
        CommandContext ctx;
        agent_info->execute(ctx);
    });
    
    auto enable_cmd = app.add_subcommand("agent:enable", agent_enable->description());
    agent_enable->configure(*enable_cmd);
    enable_cmd->callback([agent_enable]() {
        CommandContext ctx;
        agent_enable->execute(ctx);
    });
    
    auto disable_cmd = app.add_subcommand("agent:disable", agent_disable->description());
    agent_disable->configure(*disable_cmd);
    disable_cmd->callback([agent_disable]() {
        CommandContext ctx;
        agent_disable->execute(ctx);
    });
    
    auto chat_cmd = app.add_subcommand("agent:chat", agent_chat->description());
    agent_chat->configure(*chat_cmd);
    chat_cmd->callback([agent_chat]() {
        CommandContext ctx;
        agent_chat->execute(ctx);
    });
}

} // namespace cli
} // namespace hunterclaw
