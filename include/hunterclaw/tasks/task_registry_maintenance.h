#pragma once

// 严格对齐 OpenClaw: tasks/task-registry.maintenance.ts (392行)
// + tasks/task-flow-registry.maintenance.ts (132行)
// + tasks/task-owner-access.ts (90行)
// + tasks/task-status-access.ts (11行)
// + agents/session-async-task-status.ts (63行)
// Maintenance sweep engine + owner-scoped access + session async task status

#include "hunterclaw/tasks/task_registry.h"
#include "hunterclaw/tasks/task_executor.h"
#include "hunterclaw/tasks/task_flow_registry.h"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hunterclaw {
namespace tasks {

// ============================================================================
// Constants (对齐 OC task-registry.maintenance.ts)
// ============================================================================

constexpr int64_t TASK_RECONCILE_GRACE_MS = 5 * 60 * 1000;   // 5 min grace before marking lost
constexpr int64_t TASK_RETENTION_MS = 7LL * 24 * 60 * 60 * 1000;  // 7 day retention
constexpr int64_t TASK_SWEEP_INTERVAL_MS = 60 * 1000;        // 60s sweep interval
constexpr int SWEEP_YIELD_BATCH_SIZE = 25;                    // yield every 25 tasks
constexpr int64_t TASK_FLOW_RETENTION_MS = 7LL * 24 * 60 * 60 * 1000;  // 7 day flow retention

// ============================================================================
// TaskRegistryMaintenanceSummary (对齐 OC)
// ============================================================================

struct TaskRegistryMaintenanceSummary {
    int reconciled = 0;     // tasks marked as lost
    int recovered = 0;      // tasks recovered before being marked lost (对齐 OC)
    int cleanup_stamped = 0; // terminal tasks given cleanupAfter timestamp
    int pruned = 0;          // expired tasks deleted
};

struct TaskFlowRegistryMaintenanceSummary {
    int reconciled = 0;     // cancelled flows finalized
    int pruned = 0;          // expired flows deleted
};

// ============================================================================
// TaskRegistryMaintenance (对齐 OC: startTaskRegistryMaintenance / stopTaskRegistryMaintenance)
// ============================================================================

class TaskRegistryMaintenance {
public:
    TaskRegistryMaintenance(
        std::shared_ptr<TaskRegistry> registry,
        std::shared_ptr<TaskFlowRegistry> flow_registry = nullptr);
    ~TaskRegistryMaintenance();

    /// Start periodic sweep (对齐 OC: startTaskRegistryMaintenance)
    void start();

    /// Stop periodic sweep (对齐 OC: stopTaskRegistryMaintenance)
    void stop();

    /// Run one maintenance pass (对齐 OC: runTaskRegistryMaintenance)
    TaskRegistryMaintenanceSummary run_task_maintenance();

    /// Preview maintenance without applying (对齐 OC: previewTaskRegistryMaintenance)
    TaskRegistryMaintenanceSummary preview_task_maintenance() const;

    /// Run flow maintenance pass (对齐 OC: runTaskFlowRegistryMaintenance)
    TaskFlowRegistryMaintenanceSummary run_flow_maintenance();

    /// Preview flow maintenance (对齐 OC: previewTaskFlowRegistryMaintenance)
    TaskFlowRegistryMaintenanceSummary preview_flow_maintenance() const;

    /// Get reconciled summary (对齐 OC: getInspectableTaskRegistrySummary)
    TaskRegistrySummary get_inspectable_summary() const;

    /// Reconcile single task for operator inspection (对齐 OC: reconcileTaskRecordForOperatorInspection)
    TaskRecord reconcile_task_for_inspection(const TaskRecord& task) const;

    /// List all tasks with reconciliation (对齐 OC: reconcileInspectableTasks)
    std::vector<TaskRecord> reconcile_inspectable_tasks() const;

    /// Resolve task by lookup token with reconciliation (对齐 OC: reconcileTaskLookupToken)
    std::optional<TaskRecord> reconcile_task_lookup_token(const std::string& token) const;

    /// Inject backing-session checker (for runtime-specific session liveness detection)
    using BackingSessionChecker = std::function<bool(const TaskRecord& task)>;
    void set_backing_session_checker(BackingSessionChecker checker);

private:
    std::shared_ptr<TaskRegistry> registry_;
    std::shared_ptr<TaskFlowRegistry> flow_registry_;
    std::atomic<bool> running_{false};
    std::thread sweep_thread_;
    BackingSessionChecker backing_session_checker_;

    // Internal helpers (对齐 OC)
    bool should_mark_lost(const TaskRecord& task, int64_t now) const;
    bool should_prune_terminal(const TaskRecord& task, int64_t now) const;
    bool should_stamp_cleanup_after(const TaskRecord& task) const;
    int64_t resolve_cleanup_after(const TaskRecord& task) const;
    bool has_backing_session(const TaskRecord& task) const;

    // Flow helpers
    bool is_terminal_flow(const TaskFlowRecord& flow) const;
    bool should_prune_flow(const TaskFlowRecord& flow, int64_t now) const;
    bool should_finalize_cancelled_flow(const TaskFlowRecord& flow) const;
};

// ============================================================================
// Task Owner Access (对齐 OC: task-owner-access.ts)
// ============================================================================

/// Check if caller can access task (对齐 OC: canOwnerAccessTask)
bool can_owner_access_task(const TaskRecord& task, const std::string& caller_owner_key);

/// Get task by id for owner (对齐 OC: getTaskByIdForOwner)
std::optional<TaskRecord> get_task_by_id_for_owner(
    TaskRegistry& registry,
    const std::string& task_id,
    const std::string& caller_owner_key);

/// List tasks for related session key for owner (对齐 OC: listTasksForRelatedSessionKeyForOwner)
std::vector<TaskRecord> list_tasks_for_related_session_key_for_owner(
    TaskRegistry& registry,
    const std::string& related_session_key,
    const std::string& caller_owner_key);

/// Build task status snapshot for related session key for owner
/// (对齐 OC: buildTaskStatusSnapshotForRelatedSessionKeyForOwner)
TaskStatusSnapshot build_task_status_snapshot_for_owner(
    TaskRegistry& registry,
    const std::string& related_session_key,
    const std::string& caller_owner_key);

/// Find latest task for related session key for owner
/// (对齐 OC: findLatestTaskForRelatedSessionKeyForOwner)
std::optional<TaskRecord> find_latest_task_for_owner(
    TaskRegistry& registry,
    const std::string& related_session_key,
    const std::string& caller_owner_key);

/// Resolve task for lookup token for owner (对齐 OC: resolveTaskForLookupTokenForOwner)
std::optional<TaskRecord> resolve_task_for_lookup_token_for_owner(
    TaskRegistry& registry,
    const std::string& token,
    const std::string& caller_owner_key);

// ============================================================================
// Task Flow Owner Access (对齐 OC: task-flow-owner-access.ts)
// ============================================================================

/// Get flow by id for owner (对齐 OC: getTaskFlowByIdForOwner)
std::optional<TaskFlowRecord> get_task_flow_by_id_for_owner(
    TaskFlowRegistry& registry,
    const std::string& flow_id,
    const std::string& caller_owner_key);

/// List flows for owner (对齐 OC: listTaskFlowsForOwner)
std::vector<TaskFlowRecord> list_task_flows_for_owner(
    TaskFlowRegistry& registry,
    const std::string& caller_owner_key);

// ============================================================================
// Session Async Task Status (对齐 OC: agents/session-async-task-status.ts)
// ============================================================================

/// Find active session task (对齐 OC: findActiveSessionTask)
std::optional<TaskRecord> find_active_session_task(
    TaskRegistry& registry,
    const std::string& session_key,
    std::optional<TaskRuntime> runtime = std::nullopt,
    const std::optional<std::string>& task_kind = std::nullopt,
    const std::optional<std::string>& source_id_prefix = std::nullopt);

/// Build session async task status details (对齐 OC: buildSessionAsyncTaskStatusDetails)
nlohmann::json build_session_async_task_status_details(const TaskRecord& task);

} // namespace tasks
} // namespace hunterclaw
