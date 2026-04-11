#pragma once

// 严格对齐 OpenClaw: tasks/task-registry.types.ts (86行)
// + tasks/task-flow-registry.types.ts (44行)
// Core types for the background task registry and flow orchestration

#include "hunterclaw/core/json.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tasks {

// ============================================================================
// TaskRuntime (对齐 OC: TaskRuntime)
// ============================================================================

enum class TaskRuntime { SUBAGENT, ACP, CLI, CRON };

inline std::string task_runtime_string(TaskRuntime r) {
    switch (r) {
        case TaskRuntime::SUBAGENT: return "subagent";
        case TaskRuntime::ACP: return "acp";
        case TaskRuntime::CLI: return "cli";
        case TaskRuntime::CRON: return "cron";
    }
    return "subagent";
}

// ============================================================================
// TaskStatus (对齐 OC: TaskStatus)
// ============================================================================

enum class TaskStatus {
    QUEUED, RUNNING, SUCCEEDED, FAILED, TIMED_OUT, CANCELLED, LOST
};

inline std::string task_status_string(TaskStatus s) {
    switch (s) {
        case TaskStatus::QUEUED: return "queued";
        case TaskStatus::RUNNING: return "running";
        case TaskStatus::SUCCEEDED: return "succeeded";
        case TaskStatus::FAILED: return "failed";
        case TaskStatus::TIMED_OUT: return "timed_out";
        case TaskStatus::CANCELLED: return "cancelled";
        case TaskStatus::LOST: return "lost";
    }
    return "queued";
}

inline bool is_terminal_task_status(TaskStatus s) {
    return s == TaskStatus::SUCCEEDED || s == TaskStatus::FAILED ||
           s == TaskStatus::TIMED_OUT || s == TaskStatus::CANCELLED ||
           s == TaskStatus::LOST;
}

inline bool is_active_task_status(TaskStatus s) {
    return s == TaskStatus::QUEUED || s == TaskStatus::RUNNING;
}

// ============================================================================
// TaskDeliveryStatus (对齐 OC: TaskDeliveryStatus)
// ============================================================================

enum class TaskDeliveryStatus {
    PENDING, DELIVERED, SESSION_QUEUED, FAILED, PARENT_MISSING, NOT_APPLICABLE
};

inline std::string task_delivery_status_string(TaskDeliveryStatus s) {
    switch (s) {
        case TaskDeliveryStatus::PENDING: return "pending";
        case TaskDeliveryStatus::DELIVERED: return "delivered";
        case TaskDeliveryStatus::SESSION_QUEUED: return "session_queued";
        case TaskDeliveryStatus::FAILED: return "failed";
        case TaskDeliveryStatus::PARENT_MISSING: return "parent_missing";
        case TaskDeliveryStatus::NOT_APPLICABLE: return "not_applicable";
    }
    return "pending";
}

// ============================================================================
// TaskNotifyPolicy (对齐 OC: TaskNotifyPolicy)
// ============================================================================

enum class TaskNotifyPolicy { DONE_ONLY, STATE_CHANGES, SILENT };

inline std::string task_notify_policy_string(TaskNotifyPolicy p) {
    switch (p) {
        case TaskNotifyPolicy::DONE_ONLY: return "done_only";
        case TaskNotifyPolicy::STATE_CHANGES: return "state_changes";
        case TaskNotifyPolicy::SILENT: return "silent";
    }
    return "done_only";
}

// ============================================================================
// TaskTerminalOutcome / TaskScopeKind (对齐 OC)
// ============================================================================

enum class TaskTerminalOutcome { SUCCEEDED, BLOCKED };
enum class TaskScopeKind { SESSION, SYSTEM };

// ============================================================================
// TaskEventRecord (对齐 OC: TaskEventRecord)
// ============================================================================

struct TaskEventRecord {
    int64_t at = 0;
    std::string kind;  // TaskStatus string or "progress"
    std::optional<std::string> summary;
};

// ============================================================================
// TaskDeliveryState (对齐 OC: TaskDeliveryState)
// ============================================================================

struct TaskDeliveryState {
    std::string task_id;
    std::optional<nlohmann::json> requester_origin;  // DeliveryContext
    std::optional<int64_t> last_notified_event_at;
};

// ============================================================================
// TaskRecord (对齐 OC: TaskRecord)
// ============================================================================

struct TaskRecord {
    std::string task_id;
    TaskRuntime runtime = TaskRuntime::SUBAGENT;
    std::optional<std::string> task_kind;
    std::optional<std::string> source_id;
    std::string requester_session_key;
    std::string owner_key;
    TaskScopeKind scope_kind = TaskScopeKind::SESSION;
    std::optional<std::string> child_session_key;
    std::optional<std::string> parent_flow_id;
    std::optional<std::string> parent_task_id;
    std::optional<std::string> agent_id;
    std::optional<std::string> run_id;
    std::optional<std::string> label;
    std::string task;
    TaskStatus status = TaskStatus::QUEUED;
    TaskDeliveryStatus delivery_status = TaskDeliveryStatus::PENDING;
    TaskNotifyPolicy notify_policy = TaskNotifyPolicy::DONE_ONLY;
    int64_t created_at = 0;
    std::optional<int64_t> started_at;
    std::optional<int64_t> ended_at;
    std::optional<int64_t> last_event_at;
    std::optional<int64_t> cleanup_after;
    std::optional<std::string> error;
    std::optional<std::string> progress_summary;
    std::optional<std::string> terminal_summary;
    std::optional<TaskTerminalOutcome> terminal_outcome;
};

// ============================================================================
// TaskRegistrySnapshot (对齐 OC)
// ============================================================================

struct TaskRegistrySnapshot {
    std::vector<TaskRecord> tasks;
    std::vector<TaskDeliveryState> delivery_states;
};

// ============================================================================
// TaskRegistrySummary (对齐 OC: TaskRegistrySummary)
// ============================================================================

struct TaskRegistrySummary {
    int total = 0;
    int active = 0;
    int terminal = 0;
    int failures = 0;
    std::map<std::string, int> by_status;
    std::map<std::string, int> by_runtime;
};

TaskRegistrySummary create_empty_task_registry_summary();
TaskRegistrySummary summarize_task_records(const std::vector<TaskRecord>& records);

// ============================================================================
// TaskFlowSyncMode / TaskFlowStatus (对齐 OC)
// ============================================================================

enum class TaskFlowSyncMode { TASK_MIRRORED, MANAGED };

enum class TaskFlowStatus {
    QUEUED, RUNNING, WAITING, BLOCKED, SUCCEEDED, FAILED, CANCELLED, LOST
};

inline bool is_terminal_flow_status(TaskFlowStatus s) {
    return s == TaskFlowStatus::SUCCEEDED || s == TaskFlowStatus::FAILED ||
           s == TaskFlowStatus::CANCELLED || s == TaskFlowStatus::LOST;
}

// ============================================================================
// TaskFlowRecord (对齐 OC: TaskFlowRecord)
// ============================================================================

struct TaskFlowRecord {
    std::string flow_id;
    TaskFlowSyncMode sync_mode = TaskFlowSyncMode::MANAGED;
    std::string owner_key;
    std::optional<nlohmann::json> requester_origin;
    std::optional<std::string> controller_id;
    int revision = 0;
    TaskFlowStatus status = TaskFlowStatus::QUEUED;
    TaskNotifyPolicy notify_policy = TaskNotifyPolicy::DONE_ONLY;
    std::string goal;
    std::optional<std::string> current_step;
    std::optional<std::string> blocked_task_id;
    std::optional<std::string> blocked_summary;
    std::optional<nlohmann::json> state_json;
    std::optional<nlohmann::json> wait_json;
    std::optional<int64_t> cancel_requested_at;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::optional<int64_t> ended_at;
};

} // namespace tasks
} // namespace hunterclaw
