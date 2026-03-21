#pragma once
// ============================================================================
// exec.h - Exec Tool (Strictly aligned with OpenClaw bash-tools.exec.ts)
// ============================================================================

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/tools/tool_hooks.h"
#include "hunterclaw/tools/approval.h"
#include <memory>

namespace hunterclaw {

namespace gateway {
class GatewayServer;
}

namespace tools {

/**
 * Create the exec tool, fully aligned with OpenClaw createExecTool().
 *
 * Alignment:
 *   - Parameter schema: command/workdir/env/yieldMs/background/timeout/pty/elevated/host/security/ask/node
 *   - Host routing: sandbox -> gateway -> node
 *   - Security/ask policy: minSecurity() / maxAsk()
 *   - Gateway allowlist + approval flow (exec.approval.requested broadcast)
 *   - before_tool_call / after_tool_call plugin hooks via ToolHookManager
 *   - runExecProcess: spawn, PTY, yield/background
 *   - validateScriptFileForShellBleed preflight
 *   - notifyOnExit / emitExecSystemEvent
 */
std::shared_ptr<Tool> create_exec_tool(
    const ToolContext& context,
    std::shared_ptr<ApprovalManager> approval_mgr = nullptr,
    std::shared_ptr<gateway::GatewayServer> gateway_server = nullptr,
    std::shared_ptr<ToolHookManager> hook_manager = nullptr
);

} // namespace tools
} // namespace hunterclaw
