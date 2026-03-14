#include "hunterclaw/agents/types.h"
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

core::Result<AgentConfig> resolve_agent_config(
    const core::Config& config,
    const std::string& agent_id
) {
    for (const auto& agent_data : config.agents) {
        if (agent_data.id == agent_id) {
            // ========== Convert AgentConfigData to AgentConfig ==========
            AgentConfig cfg;
            
            // 基础字段
            cfg.id = agent_data.id;
            cfg.is_default = agent_data.is_default;
            cfg.name = agent_data.name;
            cfg.workspace = agent_data.workspace;
            cfg.agent_dir = agent_data.agent_dir;
            cfg.skills = agent_data.skills;
            
            // Model 配置 (从 JsonValue 解析)
            if (agent_data.extra.contains("model") && agent_data.extra["model"].is_object()) {
                const auto& model_json = agent_data.extra["model"];
                
                if (model_json.contains("provider") && model_json["provider"].is_string()) {
                    cfg.model.provider = model_json["provider"].get<std::string>();
                }
                if (model_json.contains("model") && model_json["model"].is_string()) {
                    cfg.model.model = model_json["model"].get<std::string>();
                }
                if (model_json.contains("api_key") && model_json["api_key"].is_string()) {
                    cfg.model.api_key = model_json["api_key"].get<std::string>();
                }
                if (model_json.contains("endpoint") && model_json["endpoint"].is_string()) {
                    cfg.model.endpoint = model_json["endpoint"].get<std::string>();
                }
                if (model_json.contains("max_tokens") && model_json["max_tokens"].is_number()) {
                    cfg.model.max_tokens = model_json["max_tokens"].get<int>();
                }
                if (model_json.contains("temperature") && model_json["temperature"].is_number()) {
                    cfg.model.temperature = model_json["temperature"].get<double>();
                }
                if (model_json.contains("context_tokens") && model_json["context_tokens"].is_number()) {
                    cfg.model.context_tokens = model_json["context_tokens"].get<int>();
                }
            }
            
            // Thinking level
            if (agent_data.extra.contains("thinking_level") && agent_data.extra["thinking_level"].is_string()) {
                cfg.thinking_level = agent_data.extra["thinking_level"].get<std::string>();
            }
            
            // Timeout
            if (agent_data.extra.contains("timeout_seconds") && agent_data.extra["timeout_seconds"].is_number()) {
                cfg.timeout_seconds = agent_data.extra["timeout_seconds"].get<int>();
            }
            
            // Identity/Persona (从 JsonValue 解析)
            if (agent_data.extra.contains("identity") && agent_data.extra["identity"].is_object()) {
                const auto& identity_json = agent_data.extra["identity"];
                
                if (identity_json.contains("name") && identity_json["name"].is_string()) {
                    cfg.identity.name = identity_json["name"].as_string();
                }
                if (identity_json.contains("role") && identity_json["role"].is_string()) {
                    cfg.identity.role = identity_json["role"].as_string();
                }
                if (identity_json.contains("persona") && identity_json["persona"].is_string()) {
                    cfg.identity.persona = identity_json["persona"].as_string();
                }
                // Note: rules array processing skipped - requires proper JsonValue array API
            }
            
            // Subagents (从 JsonValue 解析)
            if (agent_data.extra.contains("subagents") && agent_data.extra["subagents"].is_object()) {
                const auto& subagents_json = agent_data.extra["subagents"];
                
                if (subagents_json.contains("enabled") && subagents_json["enabled"].is_bool()) {
                    cfg.subagents.enabled = subagents_json["enabled"].as_bool();
                }
                if (subagents_json.contains("max_concurrent") && subagents_json["max_concurrent"].is_number()) {
                    cfg.subagents.max_concurrent = static_cast<int>(subagents_json["max_concurrent"].as_number());
                }
                if (subagents_json.contains("timeout_seconds") && subagents_json["timeout_seconds"].is_number()) {
                    cfg.subagents.timeout_seconds = static_cast<int>(subagents_json["timeout_seconds"].as_number());
                }
                if (subagents_json.contains("max_depth") && subagents_json["max_depth"].is_number()) {
                    cfg.subagents.max_depth = static_cast<int>(subagents_json["max_depth"].as_number());
                }
            }
            
            // Extra (原样保留 - JsonValue复制操作可能有问题,暂时跳过)
            // cfg.extra = agent_data.extra;
            
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
core::Result<AgentResolution> resolve_agent(
    const core::Config& config,
    const std::string& session_key,
    const std::string& explicit_agent_id
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
