#pragma once

// 严格对齐 OpenClaw: tasks/task-flow-registry.ts (696行)
// + tasks/task-flow-registry.store.ts (79行)
// + tasks/task-flow-owner-access.ts (50行)
// + tasks/task-registry.audit.ts (188行) + task-registry.audit.shared.ts (42行)
// Flow orchestration engine + audit

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
// TaskFlowRegistry (对齐 OC: task-flow-registry.ts)
// ============================================================================

struct TaskFlowUpdateResult {
    bool applied = false;
    std::optional<std::string> reason; // "not_found" | "revision_conflict"
    std::optional<TaskFlowRecord> flow;
    std::optional<TaskFlowRecord> current;
};

class TaskFlowRegistry {
public:
    TaskFlowRegistry();
    ~TaskFlowRegistry();

    /// Create flow record (对齐 OC: createFlowRecord)
    TaskFlowRecord create_flow_record(const TaskFlowRecord& params);

    /// Create managed task flow (对齐 OC: createManagedTaskFlow)
    TaskFlowRecord create_managed_task_flow(const TaskFlowRecord& params);

    /// Update flow by id with expected revision (对齐 OC: updateFlowRecordByIdExpectedRevision)
    TaskFlowUpdateResult update_flow_record_by_id_expected_revision(
        const std::string& flow_id, int expected_revision, const TaskFlowRecord& patch);

    /// Set flow waiting (对齐 OC: setFlowWaiting)
    TaskFlowUpdateResult set_flow_waiting(
        const std::string& flow_id, int expected_revision,
        const std::optional<std::string>& current_step = std::nullopt,
        const std::optional<nlohmann::json>& wait_json = std::nullopt);

    /// Resume flow (对齐 OC: resumeFlow)
    TaskFlowUpdateResult resume_flow(
        const std::string& flow_id, int expected_revision,
        TaskFlowStatus status = TaskFlowStatus::QUEUED);

    /// Finish flow (对齐 OC: finishFlow)
    TaskFlowUpdateResult finish_flow(
        const std::string& flow_id, int expected_revision);

    /// Fail flow (对齐 OC: failFlow)
    TaskFlowUpdateResult fail_flow(
        const std::string& flow_id, int expected_revision);

    /// Request flow cancel (对齐 OC: requestFlowCancel)
    TaskFlowUpdateResult request_flow_cancel(
        const std::string& flow_id, int expected_revision);

    /// Get by id (对齐 OC: getTaskFlowById)
    std::optional<TaskFlowRecord> get_task_flow_by_id(const std::string& flow_id) const;

    /// List all flow records (对齐 OC: listTaskFlowRecords)
    std::vector<TaskFlowRecord> list_task_flow_records() const;

    /// List flows for owner key (对齐 OC: listTaskFlowsForOwnerKey)
    std::vector<TaskFlowRecord> list_task_flows_for_owner_key(const std::string& owner_key) const;

    /// Delete flow by id (对齐 OC: deleteTaskFlowRecordById)
    bool delete_task_flow_record_by_id(const std::string& flow_id);

    /// Sync flow from task (对齐 OC: syncFlowFromTask)
    std::optional<TaskFlowRecord> sync_flow_from_task(const TaskRecord& task);

private:
    mutable std::mutex mutex_;
    std::map<std::string, TaskFlowRecord> flows_;
};

// ============================================================================
// Task Audit (对齐 OC: task-registry.audit.ts + task-registry.audit.shared.ts)
// ============================================================================

enum class TaskAuditSeverity { WARN, ERROR };

enum class TaskAuditCode {
    STALE_QUEUED, STALE_RUNNING, LOST, DELIVERY_FAILED, MISSING_CLEANUP, INCONSISTENT_TIMESTAMPS
};

struct TaskAuditFinding {
    TaskAuditSeverity severity;
    TaskAuditCode code;
    TaskRecord task;
    std::optional<int64_t> age_ms;
    std::string detail;
};

struct TaskAuditSummary {
    int total = 0;
    int warnings = 0;
    int errors = 0;
    std::map<std::string, int> by_code;
};

/// List audit findings (对齐 OC: listTaskAuditFindings)
std::vector<TaskAuditFinding> list_task_audit_findings(
    const std::vector<TaskRecord>& tasks,
    int64_t now_ms = 0,
    int64_t stale_queued_ms = 10 * 60 * 1000,
    int64_t stale_running_ms = 30 * 60 * 1000);

/// Summarize findings (对齐 OC: summarizeTaskAuditFindings)
TaskAuditSummary summarize_task_audit_findings(const std::vector<TaskAuditFinding>& findings);

// ============================================================================
// Task Flow Audit (对齐 OC: task-flow-registry.audit.ts)
// ============================================================================

enum class TaskFlowAuditCode {
    RESTORE_FAILED, STALE_RUNNING, STALE_WAITING, STALE_BLOCKED,
    CANCEL_STUCK, MISSING_LINKED_TASKS, BLOCKED_TASK_MISSING, INCONSISTENT_TIMESTAMPS
};

struct TaskFlowAuditFinding {
    TaskAuditSeverity severity;
    TaskFlowAuditCode code;
    std::optional<TaskFlowRecord> flow;
    std::optional<int64_t> age_ms;
    std::string detail;
};

std::vector<TaskFlowAuditFinding> list_task_flow_audit_findings(
    const std::vector<TaskFlowRecord>& flows,
    int64_t now_ms = 0);

} // namespace tasks
} // namespace hunterclaw
