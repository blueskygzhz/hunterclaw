// 严格对齐 OpenClaw: tasks/task-domain-views.ts (96行)
// DTO mapping: internal records → external views

#include "hunterclaw/tasks/task_domain_views.h"

namespace hunterclaw {
namespace tasks {

// ============================================================================
// mapTaskRunView (对齐 OC task-domain-views.ts:23-51)
// ============================================================================

TaskRunView map_task_run_view(const TaskRecord& task) {
    TaskRunView v;
    v.id = task.task_id;
    v.runtime = task.runtime;
    v.source_id = task.source_id;
    v.owner_key = task.owner_key;
    v.scope = task.scope_kind;
    v.child_session_key = task.child_session_key;
    v.flow_id = task.parent_flow_id;
    v.parent_task_id = task.parent_task_id;
    v.agent_id = task.agent_id;
    v.run_id = task.run_id;
    v.label = task.label;
    v.title = task.task;
    v.status = task.status;
    v.delivery_status = task.delivery_status;
    v.notify_policy = task.notify_policy;
    v.created_at = task.created_at;
    v.started_at = task.started_at;
    v.ended_at = task.ended_at;
    v.last_event_at = task.last_event_at;
    v.cleanup_after = task.cleanup_after;
    v.error = task.error;
    v.progress_summary = task.progress_summary;
    v.terminal_summary = task.terminal_summary;
    v.terminal_outcome = task.terminal_outcome;
    return v;
}

TaskRunDetail map_task_run_detail(const TaskRecord& task) {
    return map_task_run_view(task);
}

// ============================================================================
// mapTaskFlowView (对齐 OC task-domain-views.ts:57-71)
// ============================================================================

TaskFlowView map_task_flow_view(const TaskFlowRecord& flow) {
    TaskFlowView v;
    v.id = flow.flow_id;
    v.owner_key = flow.owner_key;
    v.status = flow.status;
    v.notify_policy = flow.notify_policy;
    v.goal = flow.goal;
    v.current_step = flow.current_step;
    v.cancel_requested_at = flow.cancel_requested_at;
    v.created_at = flow.created_at;
    v.updated_at = flow.updated_at;
    v.ended_at = flow.ended_at;
    return v;
}

// ============================================================================
// mapTaskFlowDetail (对齐 OC task-domain-views.ts:73-95)
// ============================================================================

TaskFlowDetail map_task_flow_detail(
    const TaskFlowRecord& flow,
    const std::vector<TaskRecord>& tasks,
    const std::optional<TaskRegistrySummary>& summary_opt) {

    TaskRegistrySummary summary = summary_opt
        ? *summary_opt
        : summarize_task_records(tasks);

    TaskFlowDetail d;
    // Copy base TaskFlowView
    static_cast<TaskFlowView&>(d) = map_task_flow_view(flow);
    d.state = flow.state_json;
    d.wait = flow.wait_json;
    d.blocked_task_id = flow.blocked_task_id;
    d.blocked_summary = flow.blocked_summary;
    for (const auto& task : tasks) {
        d.tasks.push_back(map_task_run_view(task));
    }
    d.task_summary = map_task_run_aggregate_summary(summary);
    return d;
}

// ============================================================================
// mapTaskRunAggregateSummary (对齐 OC task-domain-views.ts:12-21)
// ============================================================================

TaskRunAggregateSummary map_task_run_aggregate_summary(const TaskRegistrySummary& summary) {
    TaskRunAggregateSummary agg;
    agg.total = summary.total;
    agg.active = summary.active;
    agg.terminal = summary.terminal;
    agg.failures = summary.failures;
    agg.by_status = summary.by_status;
    agg.by_runtime = summary.by_runtime;
    return agg;
}

// ============================================================================
// JSON serialization
// ============================================================================

nlohmann::json task_run_view_to_json(const TaskRunView& v) {
    nlohmann::json j;
    j["id"] = v.id;
    j["runtime"] = task_runtime_string(v.runtime);
    if (v.source_id) j["sourceId"] = *v.source_id;
    j["ownerKey"] = v.owner_key;
    j["scope"] = task_scope_kind_string(v.scope);
    if (v.child_session_key) j["childSessionKey"] = *v.child_session_key;
    if (v.flow_id) j["flowId"] = *v.flow_id;
    if (v.parent_task_id) j["parentTaskId"] = *v.parent_task_id;
    if (v.agent_id) j["agentId"] = *v.agent_id;
    if (v.run_id) j["runId"] = *v.run_id;
    if (v.label) j["label"] = *v.label;
    j["title"] = v.title;
    j["status"] = task_status_string(v.status);
    j["deliveryStatus"] = task_delivery_status_string(v.delivery_status);
    j["notifyPolicy"] = task_notify_policy_string(v.notify_policy);
    j["createdAt"] = v.created_at;
    if (v.started_at) j["startedAt"] = *v.started_at;
    if (v.ended_at) j["endedAt"] = *v.ended_at;
    if (v.last_event_at) j["lastEventAt"] = *v.last_event_at;
    if (v.cleanup_after) j["cleanupAfter"] = *v.cleanup_after;
    if (v.error) j["error"] = *v.error;
    if (v.progress_summary) j["progressSummary"] = *v.progress_summary;
    if (v.terminal_summary) j["terminalSummary"] = *v.terminal_summary;
    if (v.terminal_outcome) j["terminalOutcome"] = task_terminal_outcome_string(*v.terminal_outcome);
    return j;
}

nlohmann::json task_flow_view_to_json(const TaskFlowView& v) {
    nlohmann::json j;
    j["id"] = v.id;
    j["ownerKey"] = v.owner_key;
    j["status"] = task_flow_status_string(v.status);
    j["notifyPolicy"] = task_notify_policy_string(v.notify_policy);
    j["goal"] = v.goal;
    if (v.current_step) j["currentStep"] = *v.current_step;
    if (v.cancel_requested_at) j["cancelRequestedAt"] = *v.cancel_requested_at;
    j["createdAt"] = v.created_at;
    j["updatedAt"] = v.updated_at;
    if (v.ended_at) j["endedAt"] = *v.ended_at;
    return j;
}

} // namespace tasks
} // namespace hunterclaw
