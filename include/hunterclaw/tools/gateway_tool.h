#pragma once

// gateway_tool.h
// Strictly aligned with OpenClaw: src/agents/tools/gateway-tool.ts
//
// Tool: gateway
// Actions: restart | config.get | config.schema.lookup | config.apply | config.patch | update.run
// ownerOnly: true

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <string>

namespace hunterclaw {
namespace tools {

/**
 * @brief Options for gateway tool (aligned with OpenClaw createGatewayTool opts)
 */
struct GatewayToolOptions {
    // Current agent session key
    std::string agent_session_key;

    // Config reference
    const core::Config* config = nullptr;
};

/**
 * @brief Create gateway tool
 *
 * Aligned with OpenClaw: createGatewayTool(opts?)
 *
 * Provides: restart, config.get, config.schema.lookup, config.apply,
 *           config.patch, update.run
 * ownerOnly: true
 */
std::shared_ptr<Tool> create_gateway_tool(const GatewayToolOptions& opts = {});

} // namespace tools
} // namespace hunterclaw
