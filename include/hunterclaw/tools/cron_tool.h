// 🎯 严格对齐 OpenClaw cron-tool.ts (src/agents/tools/cron-tool.ts)
// Single-file agent tool — mirrors OpenClaw's single cron-tool.ts.
//
// OpenClaw: createCronTool(opts?: CronToolOptions, deps?: CronToolDeps)
// HunterClaw: create_cron_tool(integration, agent_session_key)
//
// Supported actions: status | list | add | update | remove | run | runs | wake
// ownerOnly: true
// Flat-params recovery for add/update (non-frontier model compat, issue #11310)
// jobId canonical; id accepted for backward compatibility
// runMode: "due" | "force" (default: "force")
// wake mode: "now" | "next-heartbeat" (default: "next-heartbeat")
// contextMessages: 0-10 (recent chat context injection for systemEvent jobs)

#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/cron/gateway_integration.h"
#include <memory>
#include <string>

namespace hunterclaw {
namespace tools {

/**
 * Create the Cron agent tool.
 *
 * 🎯 Mirrors OpenClaw: createCronTool(opts?, deps?)
 *
 * @param integration     GatewayCronIntegration instance (required)
 * @param agent_session_key  Current agent session key (optional; used for
 *                           agentId/sessionKey injection, delivery inference,
 *                           and contextMessages chat history fetch)
 */
std::shared_ptr<Tool> create_cron_tool(
    std::shared_ptr<cron::GatewayCronIntegration> integration,
    const std::string& agent_session_key = ""
);

} // namespace tools
} // namespace hunterclaw
