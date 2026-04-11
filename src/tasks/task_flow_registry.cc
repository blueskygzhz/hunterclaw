#include "hunterclaw/tasks/task_flow_registry.h"
#include <algorithm>
#include <chrono>
#include <random>

namespace hunterclaw {
namespace tasks {

namespace {
    std::string generate_uuid() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<uint64_t> dist;
        uint64_t a = dist(gen), b = dist(gen);
        char buf[37];
        snprintf(buf, sizeof(buf),
            "%08x-%04x-%04x-%04x-%012llx",
            (uint32_t)(a >> 32), (uint16_t)(a >> 16),
            (uint16_t)((a & 0x0FFF) | 0x4000),
            (uint16_t)((b >> 48) | 0x8000),
            (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
        return buf;
    }

    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        return s.substr(start, s.find_last_not_of(" \t\n\r") - start + 1);
    }

    TaskFlowStatus derive_task_flow_status_from_task(const TaskRecord& task) {
        if (task.status == TaskStatus::QUEUED) return TaskFlowStatus::QUEUED;
        if (task.status == TaskStatus::RUNNING) return TaskFlowStatus::RUNNING;
        if (task.status == TaskStatus::SUCCEEDED) {
            return task.terminal_outcome == TaskTerminalOutcome::BLOCKED ? TaskFlowStatus::BLOCKED : TaskFlowStatus::SUCCEEDED;
        }
        if (task.status == TaskStatus::CANCELLED) return TaskFlowStatus::CANCELLED;
        if (task.status == TaskStatus::LOST) return TaskFlowStatus::LOST;
        return TaskFlowStatus::FAILED;
    }
} // namespace

// ============================================================================
// TaskFlowRegistry
// ============================================================================

TaskFlowRegistry::TaskFlowRegistry() = default;
TaskFlowRegistry::~TaskFlowRegistry() = default;

TaskFlowRecord TaskFlowRegistry::create_flow_record(const TaskFlowRecord& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    TaskFlowRecord record = params;
    record.flow_id = generate_uuid();
    if (record.created_at == 0) record.created_at = now_ms();
    if (record.updated_at == 0) record.updated_at = record.created_at;
    flows_[record.flow_id] = record;
    return record;
}

TaskFlowRecord TaskFlowRegistry::create_managed_task_flow(const TaskFlowRecord& params) {
    TaskFlowRecord record = params;
    record.sync_mode = TaskFlowSyncMode::MANAGED;
    return create_flow_record(record);
}

TaskFlowUpdateResult TaskFlowRegistry::update_flow_record_by_id_expected_revision(
    const std::string& flow_id, int expected_revision, const TaskFlowRecord& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = flows_.find(flow_id);
    if (it == flows_.end()) return {false, "not_found"};
    if (it->second.revision != expected_revision) {
        return {false, "revision_conflict", std::nullopt, it->second};
    }
    auto& current = it->second;
    if (patch.status != TaskFlowStatus::QUEUED) current.status = patch.status;
    if (patch.current_step) current.current_step = patch.current_step;
    if (patch.wait_json) current.wait_json = patch.wait_json;
    if (patch.state_json) current.state_json = patch.state_json;
    if (patch.blocked_task_id) current.blocked_task_id = patch.blocked_task_id;
    if (patch.blocked_summary) current.blocked_summary = patch.blocked_summary;
    if (patch.cancel_requested_at) current.cancel_requested_at = patch.cancel_requested_at;
    if (patch.ended_at) current.ended_at = patch.ended_at;
    current.revision++;
    current.updated_at = now_ms();
    return {true, std::nullopt, current};
}

TaskFlowUpdateResult TaskFlowRegistry::set_flow_waiting(
    const std::string& flow_id, int expected_revision,
    const std::optional<std::string>& current_step,
    const std::optional<nlohmann::json>& wait_json) {
    TaskFlowRecord patch;
    patch.status = TaskFlowStatus::WAITING;
    patch.current_step = current_step;
    patch.wait_json = wait_json;
    return update_flow_record_by_id_expected_revision(flow_id, expected_revision, patch);
}

TaskFlowUpdateResult TaskFlowRegistry::resume_flow(
    const std::string& flow_id, int expected_revision, TaskFlowStatus status) {
    TaskFlowRecord patch;
    patch.status = status;
    return update_flow_record_by_id_expected_revision(flow_id, expected_revision, patch);
}

TaskFlowUpdateResult TaskFlowRegistry::finish_flow(
    const std::string& flow_id, int expected_revision) {
    TaskFlowRecord patch;
    patch.status = TaskFlowStatus::SUCCEEDED;
    patch.ended_at = now_ms();
    return update_flow_record_by_id_expected_revision(flow_id, expected_revision, patch);
}

TaskFlowUpdateResult TaskFlowRegistry::fail_flow(
    const std::string& flow_id, int expected_revision) {
    TaskFlowRecord patch;
    patch.status = TaskFlowStatus::FAILED;
    patch.ended_at = now_ms();
    return update_flow_record_by_id_expected_revision(flow_id, expected_revision, patch);
}

TaskFlowUpdateResult TaskFlowRegistry::request_flow_cancel(
    const std::string& flow_id, int expected_revision) {
    TaskFlowRecord patch;
    patch.cancel_requested_at = now_ms();
    return update_flow_record_by_id_expected_revision(flow_id, expected_revision, patch);
}

std::optional<TaskFlowRecord> TaskFlowRegistry::get_task_flow_by_id(const std::string& flow_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = flows_.find(flow_id);
    return it != flows_.end() ? std::make_optional(it->second) : std::nullopt;
}

std::vector<TaskFlowRecord> TaskFlowRegistry::list_task_flow_records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TaskFlowRecord> result;
    for (const auto& [id, flow] : flows_) result.push_back(flow);
    std::sort(result.begin(), result.end(), [](const TaskFlowRecord& a, const TaskFlowRecord& b) {
        return a.created_at > b.created_at;
    });
    return result;
}

std::vector<TaskFlowRecord> TaskFlowRegistry::list_task_flows_for_owner_key(const std::string& owner_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = trim(owner_key);
    std::vector<TaskFlowRecord> result;
    for (const auto& [id, flow] : flows_) {
        if (trim(flow.owner_key) == key) result.push_back(flow);
    }
    std::sort(result.begin(), result.end(), [](const TaskFlowRecord& a, const TaskFlowRecord& b) {
        return a.created_at > b.created_at;
    });
    return result;
}

bool TaskFlowRegistry::delete_task_flow_record_by_id(const std::string& flow_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return flows_.erase(flow_id) > 0;
}

std::optional<TaskFlowRecord> TaskFlowRegistry::sync_flow_from_task(const TaskRecord& task) {
    if (!task.parent_flow_id || task.parent_flow_id->empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = flows_.find(*task.parent_flow_id);
    if (it == flows_.end()) return std::nullopt;
    auto& flow = it->second;
    if (flow.sync_mode != TaskFlowSyncMode::TASK_MIRRORED) return flow;
    flow.status = derive_task_flow_status_from_task(task);
    flow.goal = trim(task.label.value_or(trim(task.task)));
    if (flow.goal.empty()) flow.goal = "Background task";
    flow.updated_at = task.last_event_at.value_or(now_ms());
    flow.revision++;
    if (is_terminal_flow_status(flow.status)) {
        flow.ended_at = task.ended_at.value_or(flow.updated_at);
    }
    return flow;
}

// ============================================================================
// Task Audit
// ============================================================================

std::vector<TaskAuditFinding> list_task_audit_findings(
    const std::vector<TaskRecord>& tasks,
    int64_t now, int64_t stale_queued_ms, int64_t stale_running_ms) {
    if (now == 0) now = now_ms();
    std::vector<TaskAuditFinding> findings;
    for (const auto& task : tasks) {
        int64_t ref_at = task.last_event_at.value_or(task.started_at.value_or(task.created_at));
        int64_t age = std::max(int64_t(0), now - ref_at);

        if (task.status == TaskStatus::QUEUED && age >= stale_queued_ms) {
            findings.push_back({TaskAuditSeverity::WARN, TaskAuditCode::STALE_QUEUED, task, age, "queued task has not advanced recently"});
        }
        if (task.status == TaskStatus::RUNNING && age >= stale_running_ms) {
            findings.push_back({TaskAuditSeverity::ERROR, TaskAuditCode::STALE_RUNNING, task, age, "running task appears stuck"});
        }
        if (task.status == TaskStatus::LOST) {
            findings.push_back({TaskAuditSeverity::ERROR, TaskAuditCode::LOST, task, age, task.error.value_or("task lost its backing session")});
        }
        if (task.delivery_status == TaskDeliveryStatus::FAILED && task.notify_policy != TaskNotifyPolicy::SILENT) {
            findings.push_back({TaskAuditSeverity::WARN, TaskAuditCode::DELIVERY_FAILED, task, age, "terminal update delivery failed"});
        }
        if (is_terminal_task_status(task.status) && !task.cleanup_after) {
            findings.push_back({TaskAuditSeverity::WARN, TaskAuditCode::MISSING_CLEANUP, task, age, "terminal task is missing cleanupAfter"});
        }
    }
    return findings;
}

TaskAuditSummary summarize_task_audit_findings(const std::vector<TaskAuditFinding>& findings) {
    TaskAuditSummary summary;
    for (const auto& f : findings) {
        summary.total++;
        if (f.severity == TaskAuditSeverity::ERROR) summary.errors++;
        else summary.warnings++;
    }
    return summary;
}

// ============================================================================
// Task Flow Audit
// ============================================================================

std::vector<TaskFlowAuditFinding> list_task_flow_audit_findings(
    const std::vector<TaskFlowRecord>& flows, int64_t now) {
    if (now == 0) now = now_ms();
    std::vector<TaskFlowAuditFinding> findings;
    constexpr int64_t STALE_MS = 30 * 60 * 1000;
    for (const auto& flow : flows) {
        int64_t ref_at = flow.updated_at > 0 ? flow.updated_at : flow.created_at;
        int64_t age = std::max(int64_t(0), now - ref_at);
        if (flow.status == TaskFlowStatus::RUNNING && age >= STALE_MS) {
            findings.push_back({TaskAuditSeverity::ERROR, TaskFlowAuditCode::STALE_RUNNING, flow, age, "running TaskFlow has not advanced recently"});
        }
        if (flow.status == TaskFlowStatus::WAITING && age >= STALE_MS) {
            findings.push_back({TaskAuditSeverity::WARN, TaskFlowAuditCode::STALE_WAITING, flow, age, "waiting TaskFlow has not advanced recently"});
        }
        if (flow.status == TaskFlowStatus::BLOCKED && age >= STALE_MS) {
            findings.push_back({TaskAuditSeverity::WARN, TaskFlowAuditCode::STALE_BLOCKED, flow, age, "blocked TaskFlow has not advanced recently"});
        }
    }
    return findings;
}

} // namespace tasks
} // namespace hunterclaw
