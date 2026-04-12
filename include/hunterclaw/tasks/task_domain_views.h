#pragma once

// 严格对齐 OpenClaw: tasks/task-domain-views.ts (96行)
// + plugins/runtime/task-domain-types.ts (84行)
// DTO mapping layer: internal TaskRecord/TaskFlowRecord → external views

#include "hunterclaw/tasks/task_registry_types.h"
#include "hunterclaw/tasks/task_registry.h"
#include "hunterclaw/tasks/task_flow_registry.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tasks {

// ============================================================================
// Task Run View/Detail DTO (对齐 OC: task-domain-types.ts TaskRunView/TaskRunDetail)
// ============================================================================

struct TaskRunView {
    std::string id;
    TaskRuntime runtime;
    std::optional<std::string> source_id;
    std::string owner_key;
    TaskScopeKind scope;
    std::optional<std::string> child_session_key;
    std::optional<std::string> flow_id;
    std::optional<std::string> parent_task_id;
    std::optional<std::string> agent_id;
    std::optional<std::string> run_id;
    std::optional<std::string> label;
    std::string title;
    TaskStatus status;
    TaskDeliveryStatus delivery_status;
    TaskNotifyPolicy notify_policy;
    int64_t created_at;
    std::optional<int64_t> started_at;
    std::optional<int64_t> ended_at;
    std::optional<int64_t> last_event_at;
    std::optional<int64_t> cleanup_after;
    std::optional<std::string> error;
    std::optional<std::string> progress_summary;
    std::optional<std::string> terminal_summary;
    std::optional<TaskTerminalOutcome> terminal_outcome;
};

using TaskRunDetail = TaskRunView;

struct TaskRunCancelResult {
    bool found = false;
    bool cancelled = false;
    std::optional<std::string> reason;
    std::optional<TaskRunDetail> task;
};

// ============================================================================
// Task Run Aggregate Summary DTO (对齐 OC: task-domain-types.ts TaskRunAggregateSummary)
// ============================================================================

struct TaskRunAggregateSummary {
    int total = 0;
    int active = 0;
    int terminal = 0;
    int failures = 0;
    std::map<std::string, int> by_status;
    std::map<std::string, int> by_runtime;
};

// ============================================================================
// Task Flow View/Detail DTO (对齐 OC: task-domain-types.ts TaskFlowView/TaskFlowDetail)
// ============================================================================

struct TaskFlowView {
    std::string id;
    std::string owner_key;
    TaskFlowStatus status;
    TaskNotifyPolicy notify_policy;
    std::string goal;
    std::optional<std::string> current_step;
    std::optional<int64_t> cancel_requested_at;
    int64_t created_at;
    int64_t updated_at;
    std::optional<int64_t> ended_at;
};

struct TaskFlowDetail : TaskFlowView {
    std::optional<nlohmann::json> state;
    std::optional<nlohmann::json> wait;
    std::optional<std::string> blocked_task_id;
    std::optional<std::string> blocked_summary;
    std::vector<TaskRunView> tasks;
    TaskRunAggregateSummary task_summary;
};

// ============================================================================
// Mapping functions (对齐 OC: task-domain-views.ts)
// ============================================================================

/// Map TaskRecord → TaskRunView (对齐 OC: mapTaskRunView)
TaskRunView map_task_run_view(const TaskRecord& task);

/// Map TaskRecord → TaskRunDetail (对齐 OC: mapTaskRunDetail)
TaskRunDetail map_task_run_detail(const TaskRecord& task);

/// Map TaskFlowRecord → TaskFlowView (对齐 OC: mapTaskFlowView)
TaskFlowView map_task_flow_view(const TaskFlowRecord& flow);

/// Map TaskFlowRecord + tasks → TaskFlowDetail (对齐 OC: mapTaskFlowDetail)
TaskFlowDetail map_task_flow_detail(
    const TaskFlowRecord& flow,
    const std::vector<TaskRecord>& tasks,
    const std::optional<TaskRegistrySummary>& summary = std::nullopt);

/// Map TaskRegistrySummary → TaskRunAggregateSummary (对齐 OC: mapTaskRunAggregateSummary)
TaskRunAggregateSummary map_task_run_aggregate_summary(const TaskRegistrySummary& summary);

// ============================================================================
// JSON serialization (对齐 OC: DTO → JSON)
// ============================================================================

nlohmann::json task_run_view_to_json(const TaskRunView& view);
nlohmann::json task_flow_view_to_json(const TaskFlowView& view);

} // namespace tasks
} // namespace hunterclaw
