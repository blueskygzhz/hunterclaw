#pragma once

// agents_list_tool.h
// Strictly aligned with OpenClaw: src/agents/tools/agents-list-tool.ts
//
// Tool: agents_list
// Description: List OpenClaw agent ids you can target with sessions_spawn
//              when runtime="subagent" (based on subagent allowlists).

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <string>

namespace hunterclaw {
namespace tools {

/**
 * @brief Options for agents_list tool (aligned with OpenClaw createAgentsListTool opts)
 */
struct AgentsListToolOptions {
    // Current agent session key (agentSessionKey in OpenClaw)
    std::string agent_session_key;

    // Explicit agent ID override for cron/hook sessions
    // Aligned with OpenClaw: requesterAgentIdOverride
    std::string requester_agent_id_override;

    // Config reference for resolving agent list and allowlists
    // Uses core::Config.data (nlohmann::json) for raw config access
    const core::Config* config = nullptr;
};

/**
 * @brief Create agents_list tool
 *
 * Aligned with OpenClaw: createAgentsListTool(opts?)
 *
 * Returns a tool that lists agent IDs the requester can target with
 * sessions_spawn (based on subagent allowlists from config).
 */
std::shared_ptr<Tool> create_agents_list_tool(const AgentsListToolOptions& opts = {});

} // namespace tools
} // namespace hunterclaw
