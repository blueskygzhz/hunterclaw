#include "hunterclaw/agents/agent.h"
#include "hunterclaw/core/utils.h"
#include <algorithm>

namespace hunterclaw {
namespace agents {

// ============================================================================
// Agent Scope Helper Functions
// ============================================================================

/// List all agent IDs from configuration
std::vector<std::string> list_agent_ids(const core::Config& config) {
    std::vector<std::string> ids;
    for (const auto& agent_cfg : config.agents) {
        ids.push_back(agent_cfg.id);
    }
    return ids;
}

/// Resolve default agent ID
std::string resolve_default_agent_id(const core::Config& config) {
    for (const auto& agent_cfg : config.agents) {
        if (agent_cfg.is_default) {
            return agent_cfg.id;
        }
    }
    
    // If no default set, return first agent
    if (!config.agents.empty()) {
        return config.agents[0].id;
    }
    
    return "default";
}

/// Resolve agent ID from session key
/// Session key format: "agent_id:session_name" or just "session_name"
std::string resolve_session_agent_id(
    const std::string& session_key,
    const core::Config& config
) {
    // Check if session key contains agent ID hint
    auto colon_pos = session_key.find(':');
    if (colon_pos != std::string::npos) {
        std::string potential_agent_id = session_key.substr(0, colon_pos);
        
        // Verify agent exists
        for (const auto& agent_cfg : config.agents) {
            if (agent_cfg.id == potential_agent_id) {
                return potential_agent_id;
            }
        }
    }
    
    // Fall back to default
    return resolve_default_agent_id(config);
}

/// Resolve agent configuration by ID
core::Result<AgentConfig> resolve_agent_config(
    const core::Config& config,
    const std::string& agent_id
) {
    for (const auto& agent_data : config.agents) {
        if (agent_data.id == agent_id) {
            // Convert AgentConfigData to AgentConfig
            AgentConfig cfg;
            cfg.id = agent_data.id;
            cfg.is_default = agent_data.is_default;
            cfg.name = agent_data.name;
            cfg.workspace = agent_data.workspace;
            cfg.agent_dir = agent_data.agent_dir;
            cfg.skills = agent_data.skills;
            // TODO: Convert JsonValue to proper config structs
            return core::Result<AgentConfig>::Ok(cfg);
        }
    }
    
    return core::Result<AgentConfig>::Err("Agent not found: " + agent_id);
}

/// Resolve agent workspace directory
std::string resolve_agent_workspace_dir(
    const core::Config& config,
    const std::string& agent_id
) {
    auto cfg_result = resolve_agent_config(config, agent_id);
    if (cfg_result.is_ok()) {
        const auto& cfg = cfg_result.value;
        if (!cfg.workspace.empty()) {
            return core::utils::expand_path(cfg.workspace);
        }
    }
    
    // Default workspace
    return core::utils::expand_path("~/hunterclaw-workspace");
}

/// Resolve agent directory (for agent-specific files)
std::string resolve_agent_dir(
    const core::Config& config,
    const std::string& agent_id
) {
    auto cfg_result = resolve_agent_config(config, agent_id);
    if (cfg_result.is_ok()) {
        const auto& cfg = cfg_result.value;
        if (!cfg.agent_dir.empty()) {
            return core::utils::expand_path(cfg.agent_dir);
        }
    }
    
    // Default: ~/.hunterclaw/agents/<agent_id>
    std::string base_dir = core::utils::expand_path("~/.hunterclaw/agents");
    return base_dir + "/" + agent_id;
}

/// Resolve skill filter for agent
std::vector<std::string> resolve_agent_skills_filter(
    const core::Config& config,
    const std::string& agent_id
) {
    auto cfg_result = resolve_agent_config(config, agent_id);
    if (cfg_result.is_ok()) {
        return cfg_result.value.skills;
    }
    
    // Default: allow all skills
    return {"*"};
}

/// Check if agent has specific skill enabled
bool is_skill_enabled_for_agent(
    const core::Config& config,
    const std::string& agent_id,
    const std::string& skill_id
) {
    auto filter = resolve_agent_skills_filter(config, agent_id);
    
    // Empty filter means all allowed
    if (filter.empty()) {
        return true;
    }
    
    for (const auto& pattern : filter) {
        if (pattern == "*") {
            return true;
        }
        
        if (pattern == skill_id) {
            return true;
        }
        
        // Wildcard matching
        if (pattern.back() == '*') {
            std::string prefix = pattern.substr(0, pattern.size() - 1);
            if (skill_id.substr(0, prefix.size()) == prefix) {
                return true;
            }
        }
        
        // Negation
        if (pattern.front() == '!') {
            std::string neg_pattern = pattern.substr(1);
            if (neg_pattern == skill_id) {
                return false;
            }
        }
    }
    
    return false;
}

/// Build agent context (workspace, agent dir, etc.)
struct AgentContext {
    std::string agent_id;
    std::string workspace_dir;
    std::string agent_dir;
    std::string skills_dir;
    std::string memory_dir;
    AgentConfig config;
};

core::Result<AgentContext> build_agent_context(
    const core::Config& config,
    const std::string& agent_id
) {
    auto cfg_result = resolve_agent_config(config, agent_id);
    if (cfg_result.is_err()) {
        return core::Result<AgentContext>::Err(cfg_result.error);
    }
    
    AgentContext ctx;
    ctx.agent_id = agent_id;
    ctx.config = cfg_result.value;
    ctx.workspace_dir = resolve_agent_workspace_dir(config, agent_id);
    ctx.agent_dir = resolve_agent_dir(config, agent_id);
    ctx.skills_dir = ctx.workspace_dir + "/skills";
    ctx.memory_dir = ctx.agent_dir + "/memory";
    
    // Ensure directories exist
    core::utils::mkdir_p(ctx.workspace_dir);
    core::utils::mkdir_p(ctx.agent_dir);
    core::utils::mkdir_p(ctx.skills_dir);
    core::utils::mkdir_p(ctx.memory_dir);
    
    return core::Result<AgentContext>::Ok(ctx);
}

/// Resolve agent from multiple sources (session, explicit ID, default)
struct AgentResolution {
    std::string agent_id;
    std::string session_key;
    AgentConfig config;
    AgentContext context;
};

core::Result<AgentResolution> resolve_agent(
    const core::Config& config,
    const std::string& session_key,
    const std::string& explicit_agent_id = ""
) {
    // Priority: explicit ID > session hint > default
    std::string agent_id;
    
    if (!explicit_agent_id.empty()) {
        agent_id = explicit_agent_id;
    } else {
        agent_id = resolve_session_agent_id(session_key, config);
    }
    
    auto cfg_result = resolve_agent_config(config, agent_id);
    if (cfg_result.is_err()) {
        return core::Result<AgentResolution>::Err(cfg_result.error);
    }
    
    auto ctx_result = build_agent_context(config, agent_id);
    if (ctx_result.is_err()) {
        return core::Result<AgentResolution>::Err(ctx_result.error);
    }
    
    AgentResolution resolution;
    resolution.agent_id = agent_id;
    resolution.session_key = session_key;
    resolution.config = cfg_result.value;
    resolution.context = ctx_result.value;
    
    return core::Result<AgentResolution>::Ok(resolution);
}

} // namespace agents
} // namespace hunterclaw
