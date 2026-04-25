#pragma once

// 严格对齐 OpenClaw: tasks/task-executor.ts (739行) + task-executor-policy.ts (122行)
// Task executor — creates/starts/completes/fails/cancels task runs + policy

#include "hunterclaw/tasks/task_registry.h"
#include <memory>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tasks {

// ============================================================================
// Task Executor Policy (对齐 OC: task-executor-policy.ts)
// ============================================================================

/// Format terminal message for completed task (对齐 OC: formatTaskTerminalMessage)
std::string format_task_terminal_message(const TaskRecord& task);

/// Format blocked followup message (对齐 OC: formatTaskBlockedFollowupMessage)
std::optional<std::string> format_task_blocked_followup_message(const TaskRecord& task);

/// Format state change message (对齐 OC: formatTaskStateChangeMessage)
std::optional<std::string> format_task_state_change_message(
    const TaskRecord& task, const TaskEventRecord& event);

/// Should auto-deliver terminal update (对齐 OC: shouldAutoDeliverTaskTerminalUpdate)
bool should_auto_deliver_task_terminal_update(const TaskRecord& task);

/// Should auto-deliver state change (对齐 OC: shouldAutoDeliverTaskStateChange)
bool should_auto_deliver_task_state_change(const TaskRecord& task);

/// Should suppress duplicate delivery (对齐 OC: shouldSuppressDuplicateTerminalDelivery)
bool should_suppress_duplicate_terminal_delivery(
    const TaskRecord& task, const std::optional<std::string>& preferred_task_id);

// ============================================================================
// Task Executor (对齐 OC: task-executor.ts)
// ============================================================================

class TaskExecutor {
public:
    explicit TaskExecutor(std::shared_ptr<TaskRegistry> registry);
    ~TaskExecutor();

    /// Create queued task run (对齐 OC: createQueuedTaskRun)
    TaskRecord create_queued_task_run(const TaskRecord& params);

    /// Create running task run (对齐 OC: createRunningTaskRun)
    TaskRecord create_running_task_run(const TaskRecord& params);

    /// Start task run by run id (对齐 OC: startTaskRunByRunId)
    std::vector<TaskRecord> start_task_run_by_run_id(const std::string& run_id);

    /// Complete task run by run id (对齐 OC: completeTaskRunByRunId)
    std::vector<TaskRecord> complete_task_run_by_run_id(
        const std::string& run_id, int64_t ended_at,
        const std::optional<std::string>& terminal_summary = std::nullopt,
        const std::optional<TaskTerminalOutcome>& terminal_outcome = std::nullopt);

    /// Fail task run by run id (对齐 OC: failTaskRunByRunId)
    std::vector<TaskRecord> fail_task_run_by_run_id(
        const std::string& run_id, int64_t ended_at,
        const std::optional<std::string>& error = std::nullopt,
        TaskStatus status = TaskStatus::FAILED);

    /// Mark task run lost by id (对齐 OC: markTaskRunLostById)
    std::optional<TaskRecord> mark_task_run_lost_by_id(
        const std::string& task_id, int64_t ended_at,
        const std::optional<std::string>& error = std::nullopt);

    /// Cancel task by id (对齐 OC: cancelDetachedTaskRunById)
    TaskRegistry::CancelResult cancel_task_by_id(const std::string& task_id);

    /// Record progress by run id (对齐 OC: recordTaskRunProgressByRunId)
    std::vector<TaskRecord> record_task_progress_by_run_id(
        const std::string& run_id,
        const std::optional<std::string>& progress_summary = std::nullopt);

private:
    std::shared_ptr<TaskRegistry> registry_;
};

// ============================================================================
// Task Status Snapshot (对齐 OC: task-status.ts)
// ============================================================================

struct TaskStatusSnapshot {
    std::optional<TaskRecord> latest;
    std::optional<TaskRecord> focus;
    std::vector<TaskRecord> visible;
    std::vector<TaskRecord> active;
    std::vector<TaskRecord> recent_terminal;
    int active_count = 0;
    int total_count = 0;
    int recent_failure_count = 0;
};

/// Build status snapshot (对齐 OC: buildTaskStatusSnapshot)
TaskStatusSnapshot build_task_status_snapshot(
    const std::vector<TaskRecord>& tasks,
    int64_t now_ms = 0);

/// Format task status title (对齐 OC: formatTaskStatusTitle)
std::string format_task_status_title(const TaskRecord& task);

/// Format task status detail (对齐 OC: formatTaskStatusDetail)
std::optional<std::string> format_task_status_detail(const TaskRecord& task);

/// Sanitize task status text (对齐 OC: sanitizeTaskStatusText)
std::string sanitize_task_status_text(const std::string& value, int max_chars = 0);

// ============================================================================
// Global TaskExecutor singleton accessor
// Aligned with openclaw: getTaskExecutor() — returns the process-wide TaskExecutor
// Must be initialized via set_task_executor() before use.
// ============================================================================

/// Set the global TaskExecutor instance (called during gateway initialization)
void set_task_executor(std::shared_ptr<TaskExecutor> executor);

/// Get the global TaskExecutor instance
/// Throws std::runtime_error if not initialized.
TaskExecutor& get_task_executor();

} // namespace tasks
} // namespace hunterclaw
