#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/cron/gateway_integration.h"
#include <memory>

namespace hunterclaw {
namespace cron {

/**
 * Create Cron Agent Tool (🎯 严格对齐 OpenClaw cron-tool.ts)
 *
 * Allows AI agents to manage cron jobs through natural language.
 *
 * Supported actions (对齐 OpenClaw CRON_ACTIONS):
 * - status:  Check cron scheduler status
 * - list:    List jobs (includeDisabled:true to include disabled)
 * - add:     Create job (requires job object; flat-params recovery for non-frontier models)
 * - update:  Modify job (requires jobId + patch; flat-params recovery)
 * - remove:  Delete job (requires jobId)
 * - run:     Trigger job immediately (requires jobId; runMode: "due"|"force", default "force")
 * - runs:    Get job run history (requires jobId)
 * - wake:    Send wake event (requires text; mode: "now"|"next-heartbeat", default "next-heartbeat")
 *
 * Key alignment points:
 * - ownerOnly: true
 * - jobId is canonical; id is accepted for backward compatibility
 * - Flat-params recovery: non-frontier models (e.g. Grok) may flatten job/patch fields
 *   to the top level; the tool auto-reconstructs the nested object
 * - contextMessages (0-10): inject recent chat context into systemEvent job text
 *
 * @param integration GatewayCronIntegration instance
 * @return Shared pointer to Cron Tool
 */
std::shared_ptr<tools::Tool> create_cron_tool(
    std::shared_ptr<GatewayCronIntegration> integration
);

} // namespace cron
} // namespace hunterclaw
