#pragma once

// ============================================================================
// acp_spawn.h
// Aligned with openclaw:
//   src/agents/acp-spawn.ts          (spawnAcpDirect)
//   src/agents/acp-spawn-parent-stream.ts (startAcpSpawnParentStreamRelay)
//
// Provides:
//   - SpawnAcpParams          (aligned with SpawnAcpParams)
//   - SpawnAcpContext         (aligned with SpawnAcpContext)
//   - SpawnAcpResult          (aligned with SpawnAcpResult)
//   - AcpSpawnParentRelayHandle (aligned with AcpSpawnParentRelayHandle)
//   - spawn_acp_direct        (aligned with spawnAcpDirect)
//   - start_acp_spawn_parent_stream_relay (aligned with startAcpSpawnParentStreamRelay)
//   - resolve_acp_spawn_runtime_policy_error (aligned with resolveAcpSpawnRuntimePolicyError)
// ============================================================================

#include <string>
#include <optional>
#include <functional>
#include <memory>
#include <vector>

// Forward declaration to avoid pulling in ixwebsocket headers
namespace hunterclaw {
namespace gateway {
class GatewayClient;
} // namespace gateway
} // namespace hunterclaw

namespace hunterclaw {
namespace agents {

// ============================================================================
// AcpSpawnConfig
// ACP-specific configuration extracted from SessionsToolContext / DispatchConfig
// ============================================================================

struct AcpSpawnConfig {
    bool enable_acp = true;                     // DispatchConfig.enable_acp
    std::string acp_default_agent;              // acp.defaultAgent
    std::vector<std::string> acp_allowed_agents; // acp.allowedAgents
    int acp_max_concurrent_sessions = 10;       // acp.maxConcurrentSessions
};

// ============================================================================
// SpawnAcpParams
// Aligned with openclaw: SpawnAcpParams
// ============================================================================

struct SpawnAcpParams {
    std::string task;                           // required
    std::optional<std::string> label;
    std::optional<std::string> agent_id;        // agentId
    std::optional<std::string> resume_session_id; // resumeSessionId
    std::optional<std::string> cwd;
    std::optional<std::string> mode;            // "run" | "session"
    bool thread = false;
    std::optional<std::string> sandbox;         // "inherit" | "require"
    std::optional<std::string> stream_to;       // "parent"
};

// ============================================================================
// SpawnAcpContext
// Aligned with openclaw: SpawnAcpContext
// ============================================================================

struct SpawnAcpContext {
    std::optional<std::string> agent_session_key;   // agentSessionKey
    std::optional<std::string> agent_channel;       // agentChannel
    std::optional<std::string> agent_account_id;    // agentAccountId
    std::optional<std::string> agent_to;            // agentTo
    std::optional<std::string> agent_thread_id;     // agentThreadId
    /// ✅ 对齐 OC SpawnAcpContext.agentGroupId
    /// 用于 Telegram 等区分 group vs topic 的 channel
    std::optional<std::string> agent_group_id;      // agentGroupId
    bool sandboxed = false;
};

// ============================================================================
// SpawnAcpResult
// Aligned with openclaw: SpawnAcpResult
// ============================================================================

struct SpawnAcpResult {
    std::string status;                         // "accepted" | "forbidden" | "error"
    std::optional<std::string> child_session_key; // childSessionKey
    std::optional<std::string> run_id;          // runId
    std::optional<std::string> mode;            // "run" | "session"
    std::optional<std::string> stream_log_path; // streamLogPath
    std::optional<std::string> note;
    std::optional<std::string> error;
    /// ✅ 对齐 OC SpawnAcpFailure.errorCode
    /// 结构化错误码: "acp_disabled" | "requester_session_required" | "runtime_policy" |
    ///   "thread_required" | "target_agent_required" | "agent_forbidden" |
    ///   "thread_binding_invalid" | "cwd_resolution_failed" | "spawn_failed"
    std::optional<std::string> error_code;
};

// ============================================================================
// AcpSpawnParentRelayHandle
// Aligned with openclaw: AcpSpawnParentRelayHandle
// ============================================================================

struct AcpSpawnParentRelayHandle {
    std::function<void()> dispose;
    std::function<void()> notify_started;
};

// ============================================================================
// resolve_acp_spawn_runtime_policy_error
// Aligned with openclaw: resolveAcpSpawnRuntimePolicyError
// ============================================================================

std::optional<std::string> resolve_acp_spawn_runtime_policy_error(
    const AcpSpawnConfig& cfg,
    const std::optional<std::string>& requester_session_key,
    bool requester_sandboxed,
    const std::optional<std::string>& sandbox_mode
);

// ============================================================================
// start_acp_spawn_parent_stream_relay
// Aligned with openclaw: startAcpSpawnParentStreamRelay
//
// Subscribes to agent events for the given runId and relays text deltas
// and lifecycle events to the parent session via system events + heartbeat wake.
// ============================================================================

AcpSpawnParentRelayHandle start_acp_spawn_parent_stream_relay(
    const std::string& run_id,
    const std::string& parent_session_key,
    const std::string& child_session_key,
    const std::string& agent_id,
    const std::optional<std::string>& log_path = std::nullopt,
    int stream_flush_ms = 2500,
    int no_output_notice_ms = 60000,
    int no_output_poll_ms = 15000,
    int max_relay_lifetime_ms = 6 * 60 * 60 * 1000,
    bool emit_start_notice = false
);

// ============================================================================
// spawn_acp_direct
// Aligned with openclaw: spawnAcpDirect
//
// Spawns an ACP session:
//   1. Policy checks (acp.enabled, sandbox, allowedAgents)
//   2. Resolve target agentId
//   3. sessions.patch (create child session)
//   4. Optionally start parent stream relay
//   5. Dispatch agent turn via gateway "agent" method
//   6. Return accepted result
// ============================================================================

SpawnAcpResult spawn_acp_direct(
    const SpawnAcpParams& params,
    const SpawnAcpContext& ctx,
    std::shared_ptr<gateway::GatewayClient> gw,
    const AcpSpawnConfig& cfg
);

// ============================================================================
// Notes (aligned with openclaw)
// ============================================================================

// Aligned with openclaw: ACP_SPAWN_ACCEPTED_NOTE
inline const std::string ACP_SPAWN_ACCEPTED_NOTE =
    "initial ACP task queued in isolated session; follow-ups continue in the bound thread.";

// Aligned with openclaw: ACP_SPAWN_SESSION_ACCEPTED_NOTE
inline const std::string ACP_SPAWN_SESSION_ACCEPTED_NOTE =
    "thread-bound ACP session stays active after this task; continue in-thread for follow-ups.";

} // namespace agents
} // namespace hunterclaw
