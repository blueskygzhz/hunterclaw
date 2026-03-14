#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/cron/gateway_integration.h"
#include <memory>

namespace hunterclaw {
namespace cron {

/**
 * Create Cron Agent Tool
 * 
 * Allows AI agents to manage cron jobs through natural language.
 * Supports: list, add, update, remove, run, status
 * 
 * @param integration GatewayCronIntegration instance
 * @return Shared pointer to Cron Tool
 */
std::shared_ptr<tools::Tool> create_cron_tool(
    std::shared_ptr<GatewayCronIntegration> integration
);

} // namespace cron
} // namespace hunterclaw
