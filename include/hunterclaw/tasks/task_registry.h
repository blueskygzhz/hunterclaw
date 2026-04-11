#pragma once

// 严格对齐 OpenClaw: tasks/task-registry.ts (1979行)
// + tasks/task-registry.store.ts (95行) + tasks/task-registry.summary.ts (57行)
// + tasks/task-registry.paths.ts (22行) + tasks/runtime-internal.ts (35行)
// Core task registry — in-memory map + indexed lookups + persistence interface

#include "hunterclaw/tasks/task_registry_types.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tasks {

// ============================================================================
// TaskRegistryStore interface (对齐 OC: task-registry.store.ts)
// ============================================================================

struct TaskRegistryStore {
    std::function<TaskRegistrySnapshot()> load_snapshot;
    std::function<void(const TaskRegistrySnapshot&)> save_snapshot;
    std::function<void(const TaskRecord&)> upsert_task;
    std::function<void(const std::string&)> delete_task;
    std::function<void(const TaskDeliveryState&)> upsert_delivery_state;
    std::function<void(const std::string&)> delete_delivery_state;
    std::function<void()> close;
};

// ============================================================================
// TaskRegistry (对齐 OC: task-registry.ts 核心注册表)
// ============================================================================

class TaskRegistry {
public:
    TaskRegistry();
    ~TaskRegistry();

    /// Configure persistence store (对齐 OC: configureTaskRegistryRuntime)
    void configure_store(std::shared_ptr<TaskRegistryStore> store);

    /// Ensure registry is loaded from store (对齐 OC: ensureTaskRegistryReady)
    void ensure_ready();

    // ──── CRUD ────

    /// Create a new task record (对齐 OC: createTaskRecord)
    TaskRecord create_task(const TaskRecord& params);

    /// Update task by id (对齐 OC: updateTask)
    std::optional<TaskRecord> update_task(const std::string& task_id, const TaskRecord& patch);

    /// Delete task by id (对齐 OC: deleteTaskRecordById)
    bool delete_task(const std::string& task_id);

    // ──── Query ────

    /// Get by task id (对齐 OC: getTaskById)
    std::optional<TaskRecord> get_task_by_id(const std::string& task_id) const;

    /// Find by run id (对齐 OC: findTaskByRunId)
    std::optional<TaskRecord> find_task_by_run_id(const std::string& run_id) const;

    /// List all tasks (newest first) (对齐 OC: listTaskRecords)
    std::vector<TaskRecord> list_task_records() const;

    /// List tasks for session key (对齐 OC: listTasksForSessionKey)
    std::vector<TaskRecord> list_tasks_for_session_key(const std::string& session_key) const;

    /// List tasks for owner key (对齐 OC: listTasksForOwnerKey)
    std::vector<TaskRecord> list_tasks_for_owner_key(const std::string& owner_key) const;

    /// List tasks for flow id (对齐 OC: listTasksForFlowId)
    std::vector<TaskRecord> list_tasks_for_flow_id(const std::string& flow_id) const;

    /// List tasks for agent id (对齐 OC: listTasksForAgentId)
    std::vector<TaskRecord> list_tasks_for_agent_id(const std::string& agent_id) const;

    /// Resolve task from lookup token (对齐 OC: resolveTaskForLookupToken)
    std::optional<TaskRecord> resolve_task_for_lookup_token(const std::string& token) const;

    // ──── Status mutations ────

    /// Mark task terminal by id (对齐 OC: markTaskTerminalById)
    std::optional<TaskRecord> mark_task_terminal_by_id(
        const std::string& task_id, TaskStatus status, int64_t ended_at,
        const std::optional<std::string>& error = std::nullopt,
        const std::optional<std::string>& terminal_summary = std::nullopt,
        const std::optional<TaskTerminalOutcome>& terminal_outcome = std::nullopt);

    /// Mark task lost by id (对齐 OC: markTaskLostById)
    std::optional<TaskRecord> mark_task_lost_by_id(
        const std::string& task_id, int64_t ended_at,
        const std::optional<std::string>& error = std::nullopt);

    /// Set progress by id (对齐 OC: setTaskProgressById)
    std::optional<TaskRecord> set_task_progress_by_id(
        const std::string& task_id,
        const std::optional<std::string>& progress_summary = std::nullopt);

    /// Mark running by run id (对齐 OC: markTaskRunningByRunId)
    std::vector<TaskRecord> mark_task_running_by_run_id(const std::string& run_id);

    /// Mark terminal by run id (对齐 OC: markTaskTerminalByRunId)
    std::vector<TaskRecord> mark_task_terminal_by_run_id(
        const std::string& run_id, TaskStatus status, int64_t ended_at,
        const std::optional<std::string>& error = std::nullopt);

    // ──── Snapshot ────

    TaskRegistrySnapshot get_snapshot() const;
    TaskRegistrySummary get_summary() const;

    /// Cancel task by id (对齐 OC: cancelTaskById)
    struct CancelResult {
        bool found = false;
        bool cancelled = false;
        std::optional<std::string> reason;
        std::optional<TaskRecord> task;
    };
    CancelResult cancel_task_by_id(const std::string& task_id);

    // ──── Link to flow ────

    std::optional<TaskRecord> link_task_to_flow_by_id(
        const std::string& task_id, const std::string& flow_id);

private:
    mutable std::mutex mutex_;
    std::map<std::string, TaskRecord> tasks_;
    std::map<std::string, TaskDeliveryState> delivery_states_;

    // Indexes (对齐 OC: taskIdsByRunId, taskIdsByOwnerKey, taskIdsByParentFlowId, taskIdsByRelatedSessionKey)
    std::map<std::string, std::set<std::string>> by_run_id_;
    std::map<std::string, std::set<std::string>> by_owner_key_;
    std::map<std::string, std::set<std::string>> by_parent_flow_id_;
    std::map<std::string, std::set<std::string>> by_related_session_key_;

    std::shared_ptr<TaskRegistryStore> store_;
    bool restored_ = false;

    // Index management
    void rebuild_all_indexes();
    void add_to_indexes(const TaskRecord& task);
    void remove_from_indexes(const TaskRecord& task);

    // Persistence
    void persist_upsert(const TaskRecord& task);
    void persist_delete(const std::string& task_id);

    // Internal query
    std::vector<TaskRecord> get_tasks_by_run_id_unlocked(const std::string& run_id) const;
    std::vector<TaskRecord> list_from_index(
        const std::map<std::string, std::set<std::string>>& index,
        const std::string& key) const;

    static constexpr int64_t DEFAULT_TASK_RETENTION_MS = 7LL * 24 * 60 * 60 * 1000;
};

} // namespace tasks
} // namespace hunterclaw
