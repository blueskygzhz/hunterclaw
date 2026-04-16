// 严格对齐 OpenClaw: src/cron/isolated-agent/run.ts (715行)
// 隔离 agent 运行入口 — 完整的 cron 隔离 agent 运行流程
//
// 对齐 OpenClaw 架构:
//   run.ts → prepareCronRunContext → executeCronRun → finalizeCronRun
//   run-executor.ts → createCronPromptExecutor → executeCronRun (model fallback + retry)
//   delivery-dispatch.ts → dispatchCronDelivery (delivery routing)
//   model-selection.ts → resolveCronModelSelection
//   run-config.ts → buildCronAgentDefaultsConfig
//   session.ts → resolveCronSession
//   session-key.ts → resolveCronAgentSessionKey

#include "hunterclaw/cron/isolated_agent_run.h"
#include "hunterclaw/cron/schedule.h"

#include <chrono>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>

namespace hunterclaw {
namespace cron {

namespace {

int64_t system_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string format_cron_style_now(int64_t now_ms) {
  time_t secs = now_ms / 1000;
  std::tm tm = *std::gmtime(&secs);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

}  // namespace

// ============================================================================
// resolveCronToolPolicy (对齐 OpenClaw resolveCronToolPolicy)
// ============================================================================

CronToolPolicy resolve_cron_tool_policy(
    bool delivery_requested,
    bool delivery_resolved_ok,
    IsolatedDeliveryContract contract) {
  return {
    // requireExplicitMessageTarget:
    // Only enforce when delivery target was successfully resolved (#27898)
    delivery_requested && delivery_resolved_ok,
    // disableMessageTool:
    // Cron-owned runs always route delivery through the runner itself.
    // Shared callers keep previous behavior.
    contract == IsolatedDeliveryContract::CRON_OWNED ? true : delivery_requested
  };
}

// ============================================================================
// buildCronCommandBody (对齐 OpenClaw appendCronDeliveryInstruction)
// ============================================================================

std::string build_cron_command_body(
    const CronJob& job,
    const std::string& message,
    bool delivery_requested,
    int64_t now_ms) {

  if (now_ms == 0) now_ms = system_now_ms();

  std::string formatted_time = format_cron_style_now(now_ms);
  std::string time_line = "Current time: " + formatted_time;

  // 对齐 OpenClaw: `[cron:${job.id} ${job.name}] ${message}`
  std::string base = "[cron:" + job.id + " " + job.name + "] " + trim(message);
  std::string command_body = trim(base) + "\n" + time_line;

  // 对齐 OpenClaw: appendCronDeliveryInstruction
  if (delivery_requested) {
    command_body += "\n\nReturn your summary as plain text; it will be delivered automatically. "
                    "If the task explicitly calls for messaging a specific external recipient, "
                    "note who/where it should go instead of sending it yourself.";
  }

  return trim(command_body);
}

// ============================================================================
// runCronIsolatedAgentTurn (对齐 OpenClaw runCronIsolatedAgentTurn)
// ============================================================================

RunCronAgentTurnResult run_cron_isolated_agent_turn(
    const RunCronAgentTurnParams& params,
    const IsolatedAgentRunDeps& deps) {

  // ── Phase 1: Prepare context ──────────────────────────────────────────────
  // 对齐 OpenClaw prepareCronRunContext

  // Resolve agent ID
  std::string agent_id;
  if (params.agent_id && !params.agent_id->empty()) {
    agent_id = *params.agent_id;
  } else if (params.job.agent_id && !params.job.agent_id->empty()) {
    agent_id = *params.job.agent_id;
  }

  // Resolve session key
  std::string base_session_key = trim(params.session_key);
  if (base_session_key.empty()) {
    base_session_key = "cron:" + params.job.id;
  }

  std::string agent_session_key = base_session_key;
  if (deps.resolve_agent_session_key) {
    agent_session_key = deps.resolve_agent_session_key(base_session_key, agent_id);
  }

  // Resolve delivery
  bool delivery_requested = false;
  if (deps.resolve_delivery_requested) {
    delivery_requested = deps.resolve_delivery_requested(params.job);
  } else {
    // Default: isolated agentTurn with announce delivery
    delivery_requested = (params.job.delivery.mode == CronDeliveryMode::ANNOUNCE);
  }

  // Build command body
  std::string command_body = build_cron_command_body(
      params.job, params.message, delivery_requested);

  // ── Phase 2: Execute ──────────────────────────────────────────────────────
  // 对齐 OpenClaw executeCronRun

  // Check abort before execution
  if (params.is_aborted && params.is_aborted()) {
    std::string reason = params.abort_reason ? params.abort_reason() : "cron: job execution timed out";
    return {CronRunStatus::ERROR, reason};
  }

  RunCronAgentTurnResult result;

  if (deps.execute_agent) {
    result = deps.execute_agent(
        params.job,
        command_body,
        agent_session_key,
        params.agent_id,
        params.lane,
        params.is_aborted);
  } else {
    // No agent execution callback configured
    result.status = CronRunStatus::SKIPPED;
    result.error = "no agent executor configured";
    return result;
  }

  // Check abort after execution
  if (params.is_aborted && params.is_aborted()) {
    std::string reason = params.abort_reason ? params.abort_reason() : "cron: job execution timed out";
    return {CronRunStatus::ERROR, reason};
  }

  // ── Phase 3: Finalize ─────────────────────────────────────────────────────
  // 对齐 OpenClaw finalizeCronRun

  // Set session info
  if (!result.session_key) {
    result.session_key = agent_session_key;
  }

  return result;
}

}  // namespace cron
}  // namespace hunterclaw
