// 🎯 严格对齐 OpenClaw cron-tool.ts
// Reference: openclaw-analysis/src/agents/tools/cron-tool.ts
//
// Tools-layer header for the Cron tool.
// The full implementation lives in src/cron/cron_tool.cc (via cron::create_cron_tool).
//
// OpenClaw alignment:
// - action: status | list | add | update | remove | run | runs | wake
// - ownerOnly: true
// - flat-params recovery for add/update (non-frontier model compat)
// - jobId canonical; id accepted for backward compatibility
// - runMode: "due" | "force" (default: "force")
// - wake mode: "now" | "next-heartbeat" (default: "next-heartbeat")
// - contextMessages: 0-10 (recent chat context injection for systemEvent jobs)

#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/cron/gateway_integration.h"
#include <memory>

namespace hunterclaw {
namespace tools {

/**
 * Create the Cron tool for the tools subsystem.
 *
 * Delegates to hunterclaw::cron::create_cron_tool().
 *
 * @param integration GatewayCronIntegration instance
 * @return Shared pointer to the Cron Tool
 */
std::shared_ptr<Tool> create_cron_tool_for_tools(
    std::shared_ptr<cron::GatewayCronIntegration> integration
);

} // namespace tools
} // namespace hunterclaw
