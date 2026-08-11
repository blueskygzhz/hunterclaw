#pragma once

// sessions_tool.h
// Strictly aligned with OpenClaw:
//   src/agents/tools/sessions-list-tool.ts
//   src/agents/tools/sessions-send-tool.ts
//   src/agents/tools/sessions-spawn-tool.ts
//   src/agents/tools/sessions-history-tool.ts
//   src/agents/tools/session-status-tool.ts
//   src/agents/tools/sessions-yield-tool.ts
//   src/agents/tools/subagents-tool.ts

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/agents/subagent.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

// Forward declaration to avoid pulling in heavy ixwebsocket dependency
namespace hunterclaw {
namespace gateway {
class GatewayClient;
} // namespace gateway
} // namespace hunterclaw

namespace hunterclaw {
namespace tools {

/**
 * @brief Context for sessions tools (aligned with OpenClaw tool opts)
 *
 * Passed to all sessions tool constructors, mirrors the opts parameter
 * in OpenClaw's createSessionsListTool / createSessionsSendTool / etc.
 */
struct SessionsToolContext {
    // Requester session key (agentSessionKey in OpenClaw)
    std::string agent_session_key;

    // Requester channel (agentChannel in OpenClaw)
    std::string agent_channel;

    // Requester account ID
    std::string agent_account_id;

    // Requester delivery target
    std::string agent_to;

    // Requester thread ID
    std::string agent_thread_id;

    // Whether this session is sandboxed
    bool sandboxed = false;

    // Session scope: "per-sender" | "global"
    // Aligned with OpenClaw: cfg.session?.scope
    std::string session_scope = "per-sender";

    // Main session key (normalized)
    // Aligned with OpenClaw: normalizeMainKey(cfg.session?.mainKey)
    std::string main_key = "main";

    // Session tools visibility: "self" | "tree" | "agent" | "all"
    // Aligned with OpenClaw: cfg.tools?.sessions?.visibility (default "tree")
    std::string sessions_visibility = "tree";

    // Sandbox session tools visibility: "spawned" | "all"
    // Aligned with OpenClaw: cfg.agents?.defaults?.sandbox?.sessionToolsVisibility
    std::string sandbox_session_visibility = "spawned";

    // Agent-to-agent policy
    // Aligned with OpenClaw: cfg.tools?.agentToAgent
    bool a2a_enabled = false;
    std::vector<std::string> a2a_allow;

    // Subagent spawn limits
    // Aligned with OpenClaw: cfg.agents?.defaults?.subagents
    int max_spawn_depth = 5;
    int max_children_per_agent = 5;
    int active_children_count = 0;

    // Allowed cross-agent spawn targets
    // Aligned with OpenClaw: resolveAgentConfig(cfg, requesterAgentId)?.subagents?.allowAgents
    std::vector<std::string> subagent_allow_agents;

    // Workspace directory for subagent inheritance
    std::string workspace_dir;

    // Default model (for session_status display)
    std::string default_model;

    // Session ID (for sessions_yield)
    std::string session_id;

    // Requester agent ID override (for cron/hook sessions)
    std::string requester_agent_id_override;

    // ✅ 对齐 OpenClaw: SubagentRegistry 引用
    // sessions_spawn 成功后调用 registry->register_subagent() 注册 subagent run
    // 对应 OpenClaw: registerSubagentRun() in subagent-registry.ts
    std::shared_ptr<agents::SubagentRegistry> registry;

    // ✅ 对齐 OpenClaw: ACP 配置
    // Aligned with openclaw: cfg.acp.enabled / cfg.acp.defaultAgent / cfg.acp.allowedAgents
    bool acp_enabled = true;                          // DispatchConfig.enable_acp
    std::string acp_default_agent;                    // acp.defaultAgent
    std::vector<std::string> acp_allowed_agents;      // acp.allowedAgents (empty = all allowed)
    int acp_max_concurrent_sessions = 10;             // acp.maxConcurrentSessions
};

/**
 * @brief Factory: create all sessions tools
 *
 * Creates and returns all 6 sessions tools:
 *   - sessions_list
 *   - sessions_send
 *   - sessions_spawn
 *   - sessions_history
 *   - session_status
 *   - sessions_yield (only if on_yield is provided)
 *   - agents_wait  ✓ 对齐 OpenClaw agents-wait-tool.ts
 *   - agents_list  ✓ 对齐 OpenClaw agents-list-tool.ts
 *   - sessions_search  ✓ 对齐 OpenClaw sessions-search-tool.ts
 *   - sessions_patch/reset/delete  ✓ 对齐 OpenClaw gateway 操作
 *
 * Aligned with OpenClaw: createSessionsListTool, createSessionsSendTool,
 * createSessionsSpawnTool, createSessionsHistoryTool, createSessionStatusTool,
 * createSessionsYieldTool, createAgentsWaitTool, createAgentsListTool,
 * createSessionsSearchTool
 *
 * @param gw         Gateway client for RPC calls
 * @param ctx        Tool context (session key, visibility, A2A policy, etc.)
 * @param on_yield   Yield callback for sessions_yield (optional)
 * @return           Vector of tool instances
 */
std::vector<std::shared_ptr<Tool>> create_sessions_tools(
    std::shared_ptr<gateway::GatewayClient> gw,
    const SessionsToolContext& ctx,
    std::function<void(const std::string&)> on_yield = nullptr
);

} // namespace tools
} // namespace hunterclaw
