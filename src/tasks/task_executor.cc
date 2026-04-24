#include "hunterclaw/tasks/task_executor.h"
#include <algorithm>
#include <chrono>

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

    std::string truncate(const std::string& s, int max_chars) {
        if (max_chars <= 0 || (int)s.size() <= max_chars) return s;
        return s.substr(0, max_chars - 1) + "\xE2\x80\xA6"; // "…"
    }

    constexpr int64_t TASK_STATUS_RECENT_WINDOW_MS = 5 * 60 * 1000;
    constexpr int TASK_STATUS_TITLE_MAX_CHARS = 80;
    constexpr int TASK_STATUS_DETAIL_MAX_CHARS = 120;
} // namespace

// ============================================================================
// Task Executor Policy
// ============================================================================

std::string format_task_terminal_message(const TaskRecord& task) {
    std::string title = truncate(trim(task.label.value_or(task.task)), TASK_STATUS_TITLE_MAX_CHARS);
    if (title.empty()) title = "Background task";
    std::string run_label = task.run_id ? " (run " + task.run_id->substr(0, 8) + ")" : "";

    if (task.status == TaskStatus::SUCCEEDED) {
        if (task.terminal_outcome == TaskTerminalOutcome::BLOCKED) {
            return "Background task blocked: " + title + run_label + ".";
        }
        return "Background task done: " + title + run_label + ".";
    }
    if (task.status == TaskStatus::TIMED_OUT) {
        return "Background task timed out: " + title + run_label + ".";
    }
    if (task.status == TaskStatus::LOST) {
        return "Background task lost: " + title + run_label + ". " +
               (task.error ? *task.error : "Backing session disappeared.");
    }
    if (task.status == TaskStatus::CANCELLED) {
        return "Background task cancelled: " + title + run_label + ".";
    }
    // FAILED
    return "Background task failed: " + title + run_label + "." +
           (task.error ? " " + *task.error : "");
}

std::optional<std::string> format_task_blocked_followup_message(const TaskRecord& task) {
    if (task.status != TaskStatus::SUCCEEDED || task.terminal_outcome != TaskTerminalOutcome::BLOCKED) {
        return std::nullopt;
    }
    std::string title = truncate(trim(task.label.value_or(task.task)), TASK_STATUS_TITLE_MAX_CHARS);
    std::string summary = task.terminal_summary.value_or("Task is blocked and needs follow-up.");
    return "Task needs follow-up: " + title + ". " + summary;
}

std::optional<std::string> format_task_state_change_message(
    const TaskRecord& task, const TaskEventRecord& event) {
    std::string title = truncate(trim(task.label.value_or(task.task)), TASK_STATUS_TITLE_MAX_CHARS);
    if (event.kind == "running") {
        return "Background task started: " + title + ".";
    }
    if (event.kind == "progress" && event.summary) {
        return "Background task update: " + title + ". " + *event.summary;
    }
    return std::nullopt;
}

bool should_auto_deliver_task_terminal_update(const TaskRecord& task) {
    if (task.notify_policy == TaskNotifyPolicy::SILENT) return false;
    if (task.runtime == TaskRuntime::SUBAGENT && task.status != TaskStatus::CANCELLED) return false;
    if (!is_terminal_task_status(task.status)) return false;
    return task.delivery_status == TaskDeliveryStatus::PENDING;
}

bool should_auto_deliver_task_state_change(const TaskRecord& task) {
    return task.notify_policy == TaskNotifyPolicy::STATE_CHANGES &&
           task.delivery_status == TaskDeliveryStatus::PENDING &&
           !is_terminal_task_status(task.status);
}

bool should_suppress_duplicate_terminal_delivery(
    const TaskRecord& task, const std::optional<std::string>& preferred_task_id) {
    if (task.runtime != TaskRuntime::ACP || !task.run_id || task.run_id->empty()) return false;
    return preferred_task_id && *preferred_task_id != task.task_id;
}

// ============================================================================
// TaskExecutor
// ============================================================================

TaskExecutor::TaskExecutor(std::shared_ptr<TaskRegistry> registry)
    : registry_(std::move(registry)) {}

TaskExecutor::~TaskExecutor() = default;

TaskRecord TaskExecutor::create_queued_task_run(const TaskRecord& params) {
    TaskRecord record = params;
    record.status = TaskStatus::QUEUED;
    return registry_->create_task(record);
}

TaskRecord TaskExecutor::create_running_task_run(const TaskRecord& params) {
    TaskRecord record = params;
    record.status = TaskStatus::RUNNING;
    if (!record.started_at) record.started_at = now_ms();
    return registry_->create_task(record);
}

std::vector<TaskRecord> TaskExecutor::start_task_run_by_run_id(const std::string& run_id) {
    return registry_->mark_task_running_by_run_id(run_id);
}

std::vector<TaskRecord> TaskExecutor::complete_task_run_by_run_id(
    const std::string& run_id, int64_t ended_at,
    const std::optional<std::string>& terminal_summary,
    const std::optional<TaskTerminalOutcome>& terminal_outcome) {
    // 对齐 OC: markTaskTerminalByRunId — pass terminal_summary and terminal_outcome through
    return registry_->mark_task_terminal_by_run_id(
        run_id, TaskStatus::SUCCEEDED, ended_at,
        std::nullopt, terminal_summary, terminal_outcome);
}

std::vector<TaskRecord> TaskExecutor::fail_task_run_by_run_id(
    const std::string& run_id, int64_t ended_at,
    const std::optional<std::string>& error,
    TaskStatus status) {
    return registry_->mark_task_terminal_by_run_id(run_id, status, ended_at, error);
}

std::optional<TaskRecord> TaskExecutor::mark_task_run_lost_by_id(
    const std::string& task_id, int64_t ended_at,
    const std::optional<std::string>& error) {
    return registry_->mark_task_lost_by_id(task_id, ended_at, error);
}

TaskRegistry::CancelResult TaskExecutor::cancel_task_by_id(const std::string& task_id) {
    return registry_->cancel_task_by_id(task_id);
}

std::vector<TaskRecord> TaskExecutor::record_task_progress_by_run_id(
    const std::string& run_id,
    const std::optional<std::string>& progress_summary) {
    // 对齐 OC: recordTaskProgressByRunId — only update active (non-terminal) tasks
    // Use find_task_by_run_id to locate the task, then update progress
    auto task = registry_->find_task_by_run_id(run_id);
    if (!task || is_active_task_status(task->status) == false) return {};
    auto result = registry_->set_task_progress_by_id(task->task_id, progress_summary);
    if (result) return {*result};
    return {};
}
// ============================================================================
// Task Status Snapshot
// ============================================================================

std::string sanitize_task_status_text(const std::string& value, int max_chars) {
    auto trimmed = trim(value);
    if (max_chars > 0) return truncate(trimmed, max_chars);
    return trimmed;
}

std::string format_task_status_title(const TaskRecord& task) {
    std::string raw = trim(task.label.value_or(trim(task.task)));
    return sanitize_task_status_text(raw.empty() ? "Background task" : raw, TASK_STATUS_TITLE_MAX_CHARS);
}

std::optional<std::string> format_task_status_detail(const TaskRecord& task) {
    if (task.status == TaskStatus::RUNNING || task.status == TaskStatus::QUEUED) {
        auto s = sanitize_task_status_text(task.progress_summary.value_or(""), TASK_STATUS_DETAIL_MAX_CHARS);
        return s.empty() ? std::nullopt : std::make_optional(s);
    }
    auto err = sanitize_task_status_text(task.error.value_or(""), TASK_STATUS_DETAIL_MAX_CHARS);
    if (!err.empty()) return err;
    auto ts = sanitize_task_status_text(task.terminal_summary.value_or(""), TASK_STATUS_DETAIL_MAX_CHARS);
    return ts.empty() ? std::nullopt : std::make_optional(ts);
}

TaskStatusSnapshot build_task_status_snapshot(
    const std::vector<TaskRecord>& tasks,
    int64_t now) {
    if (now == 0) now = now_ms();

    TaskStatusSnapshot snap;
    std::vector<TaskRecord> visible_candidates;

    for (const auto& task : tasks) {
        if (task.cleanup_after && *task.cleanup_after <= now) continue;
        visible_candidates.push_back(task);
    }

    for (const auto& task : visible_candidates) {
        if (is_active_task_status(task.status)) {
            snap.active.push_back(task);
        } else {
            int64_t ref_at = task.ended_at.value_or(task.last_event_at.value_or(task.created_at));
            if (now - ref_at <= TASK_STATUS_RECENT_WINDOW_MS) {
                snap.recent_terminal.push_back(task);
            }
        }
    }

    snap.visible = snap.active;
    snap.visible.insert(snap.visible.end(), snap.recent_terminal.begin(), snap.recent_terminal.end());

    snap.active_count = (int)snap.active.size();
    snap.total_count = (int)snap.visible.size();

    for (const auto& task : snap.recent_terminal) {
        if (task.status == TaskStatus::FAILED || task.status == TaskStatus::TIMED_OUT || task.status == TaskStatus::LOST) {
            snap.recent_failure_count++;
        }
    }

    if (!snap.active.empty()) snap.latest = snap.active[0];
    else if (!snap.recent_terminal.empty()) snap.latest = snap.recent_terminal[0];

    if (!snap.active.empty()) {
        snap.focus = snap.active[0];
    } else {
        for (const auto& task : snap.recent_terminal) {
            if (task.status == TaskStatus::FAILED || task.status == TaskStatus::TIMED_OUT || task.status == TaskStatus::LOST) {
                snap.focus = task;
                break;
            }
        }
        if (!snap.focus && !snap.recent_terminal.empty()) {
            snap.focus = snap.recent_terminal[0];
        }
    }

    return snap;
}

} // namespace tasks
} // namespace hunterclaw
