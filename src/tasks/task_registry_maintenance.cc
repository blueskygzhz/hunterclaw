// 严格对齐 OpenClaw: tasks/task-registry.maintenance.ts (392行)
// + tasks/task-flow-registry.maintenance.ts (132行)
// + tasks/task-owner-access.ts (90行)
// + tasks/task-status-access.ts (11行)
// + agents/session-async-task-status.ts (63行)

#include "hunterclaw/tasks/task_registry_maintenance.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace hunterclaw {
namespace tasks {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, s.find_last_not_of(" \t\n\r") - start + 1);
}

} // namespace

// ============================================================================
// TaskRegistryMaintenance
// ============================================================================

TaskRegistryMaintenance::TaskRegistryMaintenance(
    std::shared_ptr<TaskRegistry> registry,
    std::shared_ptr<TaskFlowRegistry> flow_registry)
    : registry_(std::move(registry))
    , flow_registry_(std::move(flow_registry)) {}

TaskRegistryMaintenance::~TaskRegistryMaintenance() {
    stop();
}

void TaskRegistryMaintenance::set_backing_session_checker(BackingSessionChecker checker) {
    backing_session_checker_ = std::move(checker);
}

// ── Start/Stop (对齐 OC: startTaskRegistryMaintenance / stopTaskRegistryMaintenance) ──

void TaskRegistryMaintenance::start() {
    if (running_.load()) return;
    running_.store(true);
    registry_->ensure_ready();

    sweep_thread_ = std::thread([this]() {
        // 对齐 OC: initial deferred sweep after 5s
        std::this_thread::sleep_for(std::chrono::milliseconds(5000));
        while (running_.load()) {
            run_task_maintenance();
            if (flow_registry_) run_flow_maintenance();
            // 对齐 OC: TASK_SWEEP_INTERVAL_MS = 60s
            for (int i = 0; i < 60 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
    });
    sweep_thread_.detach();
}

void TaskRegistryMaintenance::stop() {
    running_.store(false);
}

// ── Internal helpers ──

bool TaskRegistryMaintenance::has_backing_session(const TaskRecord& task) const {
    // 对齐 OC: hasBackingSession — delegate to injected checker
    if (backing_session_checker_) {
        return backing_session_checker_(task);
    }
    // Default: assume session exists (conservative)
    return true;
}

bool TaskRegistryMaintenance::should_mark_lost(const TaskRecord& task, int64_t now) const {
    // 对齐 OC: shouldMarkLost
    if (!is_active_task_status(task.status)) return false;
    int64_t ref = task.last_event_at.value_or(task.started_at.value_or(task.created_at));
    if (now - ref < TASK_RECONCILE_GRACE_MS) return false;
    return !has_backing_session(task);
}

bool TaskRegistryMaintenance::should_prune_terminal(const TaskRecord& task, int64_t now) const {
    // 对齐 OC: shouldPruneTerminalTask
    if (is_active_task_status(task.status)) return false;
    if (task.cleanup_after) return now >= *task.cleanup_after;
    int64_t terminal_at = task.ended_at.value_or(task.last_event_at.value_or(task.created_at));
    return now - terminal_at >= TASK_RETENTION_MS;
}

bool TaskRegistryMaintenance::should_stamp_cleanup_after(const TaskRecord& task) const {
    // 对齐 OC: shouldStampCleanupAfter
    return !is_active_task_status(task.status) && !task.cleanup_after.has_value();
}

int64_t TaskRegistryMaintenance::resolve_cleanup_after(const TaskRecord& task) const {
    // 对齐 OC: resolveCleanupAfter
    int64_t terminal_at = task.ended_at.value_or(task.last_event_at.value_or(task.created_at));
    return terminal_at + TASK_RETENTION_MS;
}

// ── Task Maintenance (对齐 OC: runTaskRegistryMaintenance) ──

TaskRegistryMaintenanceSummary TaskRegistryMaintenance::run_task_maintenance() {
    registry_->ensure_ready();
    int64_t now = now_ms();
    TaskRegistryMaintenanceSummary summary;
    auto all_tasks = registry_->list_task_records();
    int processed = 0;

    for (const auto& task : all_tasks) {
        auto current = registry_->get_task_by_id(task.task_id);
        if (!current) continue;

        if (should_mark_lost(*current, now)) {
            // 对齐 OC: markTaskLost — pass cleanupAfter explicitly
            int64_t cleanup = current->cleanup_after.value_or(resolve_cleanup_after(*current));
            auto updated = registry_->mark_task_lost_by_id(
                current->task_id,
                current->ended_at.value_or(now),
                current->error.value_or("backing session missing"),
                cleanup);
            // 对齐 OC: maybeDeliverTaskTerminalUpdate after marking lost
            if (updated && updated->status == TaskStatus::LOST) {
                summary.reconciled++;
            }
            processed++;
            continue;
        }
        if (should_prune_terminal(*current, now)) {
            if (registry_->delete_task(current->task_id)) {
                summary.pruned++;
            }
            processed++;
            continue;
        }
        if (should_stamp_cleanup_after(*current)) {
            // 对齐 OC: setTaskCleanupAfterById
            TaskRecord patch;
            patch.cleanup_after = resolve_cleanup_after(*current);
            registry_->update_task(current->task_id, patch);
            summary.cleanup_stamped++;
        }
        processed++;
    }
    return summary;
}

TaskRegistryMaintenanceSummary TaskRegistryMaintenance::preview_task_maintenance() const {
    registry_->ensure_ready();
    int64_t now = now_ms();
    TaskRegistryMaintenanceSummary summary;
    for (const auto& task : registry_->list_task_records()) {
        if (should_mark_lost(task, now)) { summary.reconciled++; continue; }
        if (should_prune_terminal(task, now)) { summary.pruned++; continue; }
        if (should_stamp_cleanup_after(task)) { summary.cleanup_stamped++; }
    }
    return summary;
}

// ── Task record reconciliation (对齐 OC: reconcileTaskRecordForOperatorInspection) ──

TaskRecord TaskRegistryMaintenance::reconcile_task_for_inspection(const TaskRecord& task) const {
    int64_t now = now_ms();
    if (!should_mark_lost(task, now)) return task;
    // Project as lost (对齐 OC: projectTaskLost)
    TaskRecord projected = task;
    projected.status = TaskStatus::LOST;
    projected.ended_at = task.ended_at.value_or(now);
    projected.last_event_at = now;
    projected.error = task.error.value_or("backing session missing");
    if (!projected.cleanup_after) {
        projected.cleanup_after = resolve_cleanup_after(projected);
    }
    return projected;
}

std::vector<TaskRecord> TaskRegistryMaintenance::reconcile_inspectable_tasks() const {
    registry_->ensure_ready();
    auto all = registry_->list_task_records();
    std::vector<TaskRecord> result;
    result.reserve(all.size());
    for (const auto& task : all) {
        result.push_back(reconcile_task_for_inspection(task));
    }
    return result;
}

std::optional<TaskRecord> TaskRegistryMaintenance::reconcile_task_lookup_token(
    const std::string& token) const {
    registry_->ensure_ready();
    auto task = registry_->resolve_task_for_lookup_token(token);
    return task ? std::make_optional(reconcile_task_for_inspection(*task)) : std::nullopt;
}

TaskRegistrySummary TaskRegistryMaintenance::get_inspectable_summary() const {
    return summarize_task_records(reconcile_inspectable_tasks());
}

// ── Flow Maintenance (对齐 OC: task-flow-registry.maintenance.ts) ──

bool TaskRegistryMaintenance::is_terminal_flow(const TaskFlowRecord& flow) const {
    return is_terminal_flow_status(flow.status);
}

bool TaskRegistryMaintenance::should_prune_flow(const TaskFlowRecord& flow, int64_t now) const {
    if (!is_terminal_flow(flow)) return false;
    // 对齐 OC: hasActiveLinkedTasks
    if (registry_) {
        auto linked = registry_->list_tasks_for_flow_id(flow.flow_id);
        for (const auto& t : linked) {
            if (is_active_task_status(t.status)) return false;
        }
    }
    int64_t terminal_at = flow.ended_at.value_or(flow.updated_at);
    return now - terminal_at >= TASK_FLOW_RETENTION_MS;
}

bool TaskRegistryMaintenance::should_finalize_cancelled_flow(const TaskFlowRecord& flow) const {
    // 对齐 OC: shouldFinalizeCancelledFlow
    if (flow.sync_mode != TaskFlowSyncMode::MANAGED) return false;
    if (!flow.cancel_requested_at || is_terminal_flow(flow)) return false;
    // Check no active linked tasks
    if (registry_) {
        auto linked = registry_->list_tasks_for_flow_id(flow.flow_id);
        for (const auto& t : linked) {
            if (is_active_task_status(t.status)) return false;
        }
    }
    return true;
}

TaskFlowRegistryMaintenanceSummary TaskRegistryMaintenance::run_flow_maintenance() {
    if (!flow_registry_) return {};
    int64_t now = now_ms();
    TaskFlowRegistryMaintenanceSummary summary;
    auto all_flows = flow_registry_->list_task_flow_records();

    for (const auto& flow : all_flows) {
        auto current = flow_registry_->get_task_flow_by_id(flow.flow_id);
        if (!current) continue;

        if (should_finalize_cancelled_flow(*current)) {
            // 对齐 OC: finalizeCancelledFlow — clear blocked/wait fields, set cancelled + endedAt
            int64_t ended_at = std::max(now, current->updated_at);
            if (current->cancel_requested_at) ended_at = std::max(ended_at, *current->cancel_requested_at);
            // Build a complete patch that also clears blocked/wait fields (对齐 OC: blockedTaskId:null, blockedSummary:null, waitJson:null)
            TaskFlowRecord patch = *current;
            patch.status = TaskFlowStatus::CANCELLED;
            patch.ended_at = ended_at;
            patch.updated_at = ended_at;
            patch.blocked_task_id = std::nullopt;
            patch.blocked_summary = std::nullopt;
            patch.wait_json = std::nullopt;
            flow_registry_->update_flow_record_by_id_expected_revision(
                current->flow_id, current->revision, patch);
            summary.reconciled++;
            continue;
        }
        if (should_prune_flow(*current, now)) {
            flow_registry_->delete_task_flow_record_by_id(current->flow_id);
            summary.pruned++;
        }
    }
    return summary;
}

TaskFlowRegistryMaintenanceSummary TaskRegistryMaintenance::preview_flow_maintenance() const {
    if (!flow_registry_) return {};
    int64_t now = now_ms();
    TaskFlowRegistryMaintenanceSummary summary;
    for (const auto& flow : flow_registry_->list_task_flow_records()) {
        if (should_finalize_cancelled_flow(flow)) { summary.reconciled++; continue; }
        if (should_prune_flow(flow, now)) { summary.pruned++; }
    }
    return summary;
}

// ============================================================================
// Task Owner Access (对齐 OC: task-owner-access.ts)
// ============================================================================

bool can_owner_access_task(const TaskRecord& task, const std::string& caller_owner_key) {
    return task.scope_kind == TaskScopeKind::SESSION &&
           trim(task.owner_key) == trim(caller_owner_key);
}

std::optional<TaskRecord> get_task_by_id_for_owner(
    TaskRegistry& registry,
    const std::string& task_id,
    const std::string& caller_owner_key) {
    auto task = registry.get_task_by_id(task_id);
    if (task && can_owner_access_task(*task, caller_owner_key)) return task;
    return std::nullopt;
}

std::vector<TaskRecord> list_tasks_for_related_session_key_for_owner(
    TaskRegistry& registry,
    const std::string& related_session_key,
    const std::string& caller_owner_key) {
    auto tasks = registry.list_tasks_for_session_key(related_session_key);
    std::vector<TaskRecord> result;
    for (const auto& task : tasks) {
        if (can_owner_access_task(task, caller_owner_key)) {
            result.push_back(task);
        }
    }
    return result;
}

TaskStatusSnapshot build_task_status_snapshot_for_owner(
    TaskRegistry& registry,
    const std::string& related_session_key,
    const std::string& caller_owner_key) {
    auto tasks = list_tasks_for_related_session_key_for_owner(
        registry, related_session_key, caller_owner_key);
    return build_task_status_snapshot(tasks);
}

std::optional<TaskRecord> find_latest_task_for_owner(
    TaskRegistry& registry,
    const std::string& related_session_key,
    const std::string& caller_owner_key) {
    auto tasks = list_tasks_for_related_session_key_for_owner(
        registry, related_session_key, caller_owner_key);
    return tasks.empty() ? std::nullopt : std::make_optional(tasks[0]);
}

std::optional<TaskRecord> resolve_task_for_lookup_token_for_owner(
    TaskRegistry& registry,
    const std::string& token,
    const std::string& caller_owner_key) {
    // Try direct by ID
    auto direct = get_task_by_id_for_owner(registry, token, caller_owner_key);
    if (direct) return direct;
    // Try by run ID
    auto by_run = registry.find_task_by_run_id(token);
    if (by_run && can_owner_access_task(*by_run, caller_owner_key)) return by_run;
    // Try by session key
    auto by_session = find_latest_task_for_owner(registry, token, caller_owner_key);
    if (by_session) return by_session;
    // Try raw lookup
    auto raw = registry.resolve_task_for_lookup_token(token);
    if (raw && can_owner_access_task(*raw, caller_owner_key)) return raw;
    return std::nullopt;
}

// ============================================================================
// Task Flow Owner Access (对齐 OC: task-flow-owner-access.ts)
// ============================================================================

std::optional<TaskFlowRecord> get_task_flow_by_id_for_owner(
    TaskFlowRegistry& registry,
    const std::string& flow_id,
    const std::string& caller_owner_key) {
    auto flow = registry.get_task_flow_by_id(flow_id);
    if (flow && trim(flow->owner_key) == trim(caller_owner_key)) return flow;
    return std::nullopt;
}

std::vector<TaskFlowRecord> list_task_flows_for_owner(
    TaskFlowRegistry& registry,
    const std::string& caller_owner_key) {
    auto key = trim(caller_owner_key);
    if (key.empty()) return {};
    return registry.list_task_flows_for_owner_key(key);
}

// ============================================================================
// Session Async Task Status (对齐 OC: agents/session-async-task-status.ts)
// ============================================================================

std::optional<TaskRecord> find_active_session_task(
    TaskRegistry& registry,
    const std::string& session_key,
    std::optional<TaskRuntime> runtime,
    const std::optional<std::string>& task_kind,
    const std::optional<std::string>& source_id_prefix) {

    auto sk = trim(session_key);
    if (sk.empty()) return std::nullopt;

    auto tasks = registry.list_tasks_for_owner_key(sk);
    std::vector<TaskRecord> matches;

    for (const auto& task : tasks) {
        if (task.scope_kind != TaskScopeKind::SESSION) continue;
        if (runtime && task.runtime != *runtime) continue;
        if (!is_active_task_status(task.status)) continue;
        if (task_kind && trim(*task_kind) != trim(task.task_kind.value_or(""))) continue;
        if (source_id_prefix) {
            std::string sid = trim(task.source_id.value_or(""));
            std::string prefix = trim(*source_id_prefix);
            if (sid != prefix && sid.rfind(prefix + ":", 0) != 0) continue;
        }
        matches.push_back(task);
    }

    if (matches.empty()) return std::nullopt;
    // Prefer running over queued (对齐 OC)
    for (const auto& t : matches) {
        if (t.status == TaskStatus::RUNNING) return t;
    }
    return matches[0];
}

nlohmann::json build_session_async_task_status_details(const TaskRecord& task) {
    nlohmann::json result;
    result["async"] = true;
    result["active"] = true;
    result["existingTask"] = true;
    result["status"] = task_status_string(task.status);
    nlohmann::json task_info;
    task_info["taskId"] = task.task_id;
    if (task.run_id) task_info["runId"] = *task.run_id;
    result["task"] = task_info;
    if (task.task_kind) result["taskKind"] = *task.task_kind;
    if (task.progress_summary) result["progressSummary"] = *task.progress_summary;
    if (task.source_id) result["sourceId"] = *task.source_id;
    return result;
}

} // namespace tasks
} // namespace hunterclaw
