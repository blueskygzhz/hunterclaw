#pragma once

// 严格对齐 OpenClaw: src/acp/control-plane/manager.background-task.ts (221行)
// Mirrors child ACP turns into detached-task status for requester-facing progress.
//
// 背景：ACP 子会话不进 SubagentRegistry（对齐 OC 注释
// "ACP children bypass the subagent registry"），因此父会话无法通过
// agents_wait 拉取结果。真源的做法是把子会话 turn 的生命周期镜像进
// TaskRegistry（detached task），父侧通过 tasks 视图 / 终态投递感知结果。

#include "hunterclaw/acp/runtime_errors.h"
#include "hunterclaw/tasks/task_registry_types.h"

#include <optional>
#include <string>

namespace hunterclaw {
namespace acp {

// 对齐 OC: ACP_BACKGROUND_TASK_TEXT_MAX_LENGTH = 160
constexpr size_t ACP_BACKGROUND_TASK_TEXT_MAX_LENGTH = 160;
// 对齐 OC: ACP_BACKGROUND_TASK_PROGRESS_MAX_LENGTH = 240
constexpr size_t ACP_BACKGROUND_TASK_PROGRESS_MAX_LENGTH = 240;

/// Context needed to mirror a child ACP turn into the requester task registry.
/// 对齐 OC: type BackgroundTaskContext
struct AcpBackgroundTaskContext {
    std::string requester_session_key;
    std::optional<std::string> requester_channel;   // deliveryContextFromSession
    std::optional<std::string> requester_to;
    std::optional<std::string> requester_account_id;
    std::optional<std::string> requester_thread_id;
    std::string child_session_key;
    std::string run_id;
    std::optional<std::string> label;
    std::string task;
};

/// Terminal result inferred from final progress text.
/// 对齐 OC: resolveBackgroundTaskTerminalResult 返回值
struct AcpBackgroundTaskTerminalResult {
    std::optional<tasks::TaskTerminalOutcome> terminal_outcome;
    std::optional<std::string> terminal_summary;
};

/// Produces the bounded task label shown for a child ACP background run.
/// 对齐 OC: summarizeBackgroundTaskText
std::string summarize_background_task_text(const std::string& text);

/// Appends bounded progress text while preserving a single-line task summary.
/// 对齐 OC: appendBackgroundTaskProgressSummary
std::string append_background_task_progress_summary(const std::string& current,
                                                    const std::string& chunk);

/// Maps ACP runtime failures to detached-task terminal states.
/// 对齐 OC: resolveBackgroundTaskFailureStatus —— 仅 timeout 映射 TIMED_OUT，其余 FAILED
tasks::TaskStatus resolve_background_task_failure_status(const AcpRuntimeError& error);

/// Infers blocked terminal outcomes from final progress text.
/// 对齐 OC: resolveBackgroundTaskTerminalResult
AcpBackgroundTaskTerminalResult resolve_background_task_terminal_result(
    const std::string& progress_summary);

/// Creates the running task record mirroring a child ACP turn.
/// 对齐 OC: createBackgroundTaskRecord —— 失败仅 logVerbose，不抛
void create_background_task_record(const AcpBackgroundTaskContext& context, int64_t started_at);

/// Updates progress on the mirrored task.
/// 对齐 OC: markBackgroundTaskRunning
void mark_background_task_running(const std::string& run_id,
                                  const std::optional<std::string>& progress_summary);

/// Parameters for terminal task write. 对齐 OC: markBackgroundTaskTerminal params
struct MarkBackgroundTaskTerminalParams {
    tasks::TaskStatus status = tasks::TaskStatus::SUCCEEDED;
    int64_t ended_at = 0;
    std::optional<std::string> error;
    std::optional<std::string> progress_summary;
    std::optional<std::string> terminal_summary;
    std::optional<tasks::TaskTerminalOutcome> terminal_outcome;
};

/// Writes the terminal state of the mirrored task.
/// 对齐 OC: markBackgroundTaskTerminal
void mark_background_task_terminal(const std::string& run_id,
                                   const MarkBackgroundTaskTerminalParams& params);

} // namespace acp
} // namespace hunterclaw
