// 🎯 严格对齐 OpenClaw cron-tool.ts
// Reference: openclaw-analysis/src/agents/tools/cron-tool.ts
//
// This file is the tools-layer entry point for the Cron tool.
// The full implementation lives in src/cron/cron_tool.cc.
// This file wires the cron tool into the tools subsystem.
//
// OpenClaw alignment:
// - action: status | list | add | update | remove | run | runs | wake
// - ownerOnly: true
// - flat-params recovery for add/update (non-frontier model compat)
// - jobId canonical; id accepted for backward compatibility
// - runMode: "due" | "force" (default: "force")
// - wake mode: "now" | "next-heartbeat" (default: "next-heartbeat")
// - contextMessages: 0-10 (recent chat context injection for systemEvent jobs)

#include "hunterclaw/cron/cron_tool.h"
#include "hunterclaw/cron/gateway_integration.h"
#include "hunterclaw/tools/tool.h"
#include <memory>

namespace hunterclaw {
namespace tools {

// ============================================================================
// Factory: create the Cron tool for the tools subsystem
// ============================================================================

std::shared_ptr<Tool> create_cron_tool_for_tools(
    std::shared_ptr<cron::GatewayCronIntegration> integration
) {
    return cron::create_cron_tool(std::move(integration));
}

} // namespace tools
} // namespace hunterclaw
