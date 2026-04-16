#pragma once

// 严格对齐 OpenClaw: src/cron/isolated-agent/run.ts (715行)
// 隔离 agent 运行入口 — 完整的 cron 隔离 agent 运行流程

#include "hunterclaw/cron/service.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace hunterclaw {
namespace cron {

/**
 * RunCronAgentTurnResult (对齐 OpenClaw RunCronAgentTurnResult)
 */
struct RunCronAgentTurnResult {
  CronRunStatus status = CronRunStatus::OK;
  std::optional<std::string> error;
  std::optional<std::string> summary;
  std::optional<std::string> output_text;
  std::optional<std::string> session_id;
  std::optional<std::string> session_key;
  std::optional<bool> delivered;
  std::optional<bool> delivery_attempted;
  std::optional<std::string> error_kind;  // "delivery-target"
  // CronRunTelemetry
  std::optional<std::string> model;
  std::optional<std::string> provider;
};

/**
 * IsolatedDeliveryContract (对齐 OpenClaw IsolatedDeliveryContract)
 */
enum class IsolatedDeliveryContract {
  CRON_OWNED,  // Cron-owned runs route delivery through the runner
  SHARED       // Shared callers keep previous behavior
};

/**
 * RunCronAgentTurnParams (对齐 OpenClaw RunCronAgentTurnParams)
 */
struct RunCronAgentTurnParams {
  // Config
  std::string config_path;
  // Job
  CronJob job;
  std::string message;
  // Session
  std::string session_key;
  std::optional<std::string> agent_id;
  std::optional<std::string> lane;
  IsolatedDeliveryContract delivery_contract = IsolatedDeliveryContract::CRON_OWNED;
  // Abort
  std::function<bool()> is_aborted;
  std::function<std::string()> abort_reason;
};

/**
 * IsolatedAgentRunDeps (对齐 OpenClaw cli deps + runtime deps)
 */
struct IsolatedAgentRunDeps {
  // Agent execution callback
  std::function<RunCronAgentTurnResult(
      const CronJob& job,
      const std::string& message,
      const std::string& session_key,
      const std::optional<std::string>& agent_id,
      const std::optional<std::string>& lane,
      const std::function<bool()>& is_aborted)> execute_agent;

  // Delivery resolution
  std::function<bool(const CronJob& job)> resolve_delivery_requested;

  // Session management
  std::function<std::string(const std::string& session_key,
                             const std::string& agent_id)> resolve_agent_session_key;

  // Model selection
  std::function<std::pair<std::string, std::string>(
      const CronJob& job,
      const std::optional<std::string>& agent_id)> resolve_model_selection;

  // Workspace
  std::function<std::string(const std::optional<std::string>& agent_id)> resolve_workspace_dir;

  // Skills
  std::function<void(const std::string& workspace_dir,
                      const std::optional<std::string>& agent_id)> resolve_skills_snapshot;
};

/**
 * 执行隔离 agent 运行
 * 对齐 OpenClaw: runCronIsolatedAgentTurn(params)
 *
 * Full flow:
 * 1. Resolve agent config and defaults
 * 2. Resolve session (new or existing based on sessionTarget)
 * 3. Resolve model selection
 * 4. Resolve delivery context
 * 5. Build command body (with cron metadata and delivery instruction)
 * 6. Execute agent run (with model fallback and timeout)
 * 7. Finalize: extract payloads, dispatch delivery, update session
 *
 * @param params  Run parameters
 * @param deps    Runtime dependencies
 * @return execution result
 */
RunCronAgentTurnResult run_cron_isolated_agent_turn(
    const RunCronAgentTurnParams& params,
    const IsolatedAgentRunDeps& deps);

/**
 * 解析 cron 工具策略
 * 对齐 OpenClaw: resolveCronToolPolicy(params)
 */
struct CronToolPolicy {
  bool require_explicit_message_target = false;
  bool disable_message_tool = false;
};

CronToolPolicy resolve_cron_tool_policy(
    bool delivery_requested,
    bool delivery_resolved_ok,
    IsolatedDeliveryContract contract);

/**
 * 构建 cron 命令体
 * 对齐 OpenClaw: appendCronDeliveryInstruction
 */
std::string build_cron_command_body(
    const CronJob& job,
    const std::string& message,
    bool delivery_requested,
    int64_t now_ms = 0);

}  // namespace cron
}  // namespace hunterclaw
