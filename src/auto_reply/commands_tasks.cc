// 严格对齐 OpenClaw:
//   auto-reply/reply/commands-tasks.ts (150行)
//   auto-reply/reply/commands-status.ts (formatSessionTaskLine + formatAgentTaskCountsLine)
//   tasks/task-status-access.ts (11行)
//   agents/media-generation-task-status-shared.ts (102行)

#include "hunterclaw/auto_reply/commands_tasks.h"
#include "hunterclaw/tasks/task_registry_maintenance.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>

namespace hunterclaw {
namespace auto_reply {

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ============================================================================
// Task Status Access (对齐 OC: task-status-access.ts)
// ============================================================================

std::vector<tasks::TaskRecord> list_tasks_for_session_key_for_status(
    tasks::TaskRegistry& registry,
    const std::string& session_key) {
    // 对齐 OC: listTasksForSessionKeyForStatus → listTasksForSessionKey
    return registry.list_tasks_for_session_key(session_key);
}

std::vector<tasks::TaskRecord> list_tasks_for_agent_id_for_status(
    tasks::TaskRegistry& registry,
    const std::string& agent_id) {
    // 对齐 OC: listTasksForAgentIdForStatus → listTasksForAgentId
    return registry.list_tasks_for_agent_id(agent_id);
}

// ============================================================================
// /tasks command constants (对齐 OC commands-tasks.ts:18-35)
// ============================================================================

static constexpr int MAX_VISIBLE_TASKS = 5;

static const char* task_status_icon(tasks::TaskStatus status) {
    switch (status) {
        case tasks::TaskStatus::QUEUED:    return "🟡";
        case tasks::TaskStatus::RUNNING:   return "🟢";
        case tasks::TaskStatus::SUCCEEDED: return "✅";
        case tasks::TaskStatus::FAILED:    return "🔴";
        case tasks::TaskStatus::TIMED_OUT: return "⏱️";
        case tasks::TaskStatus::CANCELLED: return "⚪️";
        case tasks::TaskStatus::LOST:      return "⚠️";
    }
    return "❓";
}

static const char* task_runtime_label(tasks::TaskRuntime runtime) {
    switch (runtime) {
        case tasks::TaskRuntime::SUBAGENT: return "Subagent";
        case tasks::TaskRuntime::ACP:      return "ACP";
        case tasks::TaskRuntime::CLI:      return "CLI";
        case tasks::TaskRuntime::CRON:     return "Cron";
    }
    return "Unknown";
}

// ============================================================================
// format helpers (对齐 OC commands-tasks.ts:37-78)
// ============================================================================

static std::string format_duration_compact(int64_t ms) {
    if (ms < 1000) return std::to_string(ms) + "ms";
    int64_t seconds = ms / 1000;
    if (seconds < 60) return std::to_string(seconds) + "s";
    int64_t minutes = seconds / 60;
    if (minutes < 60) return std::to_string(minutes) + "m";
    int64_t hours = minutes / 60;
    return std::to_string(hours) + "h " + std::to_string(minutes % 60) + "m";
}

static std::string format_time_ago(int64_t ms) {
    if (ms < 60000) return "just now";
    int64_t minutes = ms / 60000;
    if (minutes < 60) return std::to_string(minutes) + "m ago";
    int64_t hours = minutes / 60;
    if (hours < 24) return std::to_string(hours) + "h ago";
    int64_t days = hours / 24;
    return std::to_string(days) + "d ago";
}

static std::string format_task_timing(const tasks::TaskRecord& task) {
    int64_t now = now_ms();
    if (task.status == tasks::TaskStatus::RUNNING) {
        int64_t started_at = task.started_at.value_or(task.created_at);
        return "elapsed " + format_duration_compact(now - started_at);
    }
    if (task.status == tasks::TaskStatus::QUEUED) {
        return "queued " + format_time_ago(now - task.created_at);
    }
    int64_t ended_at = task.ended_at.value_or(
        task.last_event_at.value_or(task.created_at));
    return "finished " + format_time_ago(now - ended_at);
}

static std::string format_visible_task(const tasks::TaskRecord& task, int index) {
    std::string title = tasks::format_task_status_title(task);
    std::string status_str = tasks::task_status_string(task.status);
    // Replace underscores with spaces
    std::replace(status_str.begin(), status_str.end(), '_', ' ');
    std::string timing = format_task_timing(task);
    auto detail_opt = tasks::format_task_status_detail(task);

    std::string meta = std::string(task_runtime_label(task.runtime)) +
                       " · " + status_str + " · " + timing;

    std::ostringstream oss;
    oss << (index + 1) << ". " << task_status_icon(task.status) << " " << title << "\n";
    oss << "   " << meta;
    if (detail_opt) {
        oss << "\n   " << *detail_opt;
    }
    return oss.str();
}

// ============================================================================
// buildTasksText (对齐 OC commands-tasks.ts:81-108)
// ============================================================================

std::string build_tasks_text(
    tasks::TaskRegistry& registry,
    const std::string& session_key,
    const std::string& agent_id) {

    auto session_tasks = list_tasks_for_session_key_for_status(registry, session_key);
    auto session_snapshot = tasks::build_task_status_snapshot(session_tasks);

    std::ostringstream oss;
    oss << "📋 Tasks\n";

    if (session_snapshot.total_count == 0) {
        oss << "All clear - nothing linked to this session right now.";
        // Check agent-level fallback (对齐 OC: formatAgentFallbackLine)
        auto agent_tasks = list_tasks_for_agent_id_for_status(registry, agent_id);
        auto agent_snapshot = tasks::build_task_status_snapshot(agent_tasks);
        if (agent_snapshot.total_count > 0) {
            oss << "\nAgent-local: " << agent_snapshot.active_count
                << " active · " << agent_snapshot.total_count << " total";
        }
        return oss.str();
    }

    oss << "Current session: " << session_snapshot.active_count
        << " active · " << session_snapshot.total_count << " total";

    // Show visible tasks (对齐 OC: visible.slice(0, MAX_VISIBLE_TASKS))
    int visible_count = std::min((int)session_snapshot.visible.size(), MAX_VISIBLE_TASKS);
    if (visible_count > 0) {
        oss << "\n";
        for (int i = 0; i < visible_count; ++i) {
            oss << "\n" << format_visible_task(session_snapshot.visible[i], i);
            if (i < visible_count - 1) oss << "\n";
        }
        int hidden = (int)session_snapshot.visible.size() - visible_count;
        if (hidden > 0) {
            oss << "\n\n+" << hidden << " more recent task"
                << (hidden == 1 ? "" : "s");
        }
    }
    return oss.str();
}

// ============================================================================
// formatSessionTaskLine (对齐 OC commands-status.ts:68-84)
// ============================================================================

std::optional<std::string> format_session_task_line(
    tasks::TaskRegistry& registry,
    const std::string& session_key) {

    auto session_tasks = list_tasks_for_session_key_for_status(registry, session_key);
    auto snapshot = tasks::build_task_status_snapshot(session_tasks);
    if (!snapshot.focus) return std::nullopt;

    std::string headline;
    if (snapshot.active_count > 0) {
        headline = std::to_string(snapshot.active_count) + " active · " +
                   std::to_string(snapshot.total_count) + " total";
    } else if (snapshot.recent_failure_count > 0) {
        headline = std::to_string(snapshot.recent_failure_count) + " recent failure" +
                   (snapshot.recent_failure_count == 1 ? "" : "s");
    } else {
        headline = "recently finished";
    }

    std::string title = tasks::format_task_status_title(*snapshot.focus);
    auto detail = tasks::format_task_status_detail(*snapshot.focus);
    std::string runtime_str = task_runtime_label(snapshot.focus->runtime);

    std::ostringstream parts;
    parts << headline << " · " << runtime_str << " · " << title;
    if (detail) parts << " · " << *detail;

    return "📌 Tasks: " + parts.str();
}

// ============================================================================
// formatAgentTaskCountsLine (对齐 OC commands-status.ts:86-92)
// ============================================================================

std::optional<std::string> format_agent_task_counts_line(
    tasks::TaskRegistry& registry,
    const std::string& agent_id) {

    auto agent_tasks = list_tasks_for_agent_id_for_status(registry, agent_id);
    auto snapshot = tasks::build_task_status_snapshot(agent_tasks);
    if (snapshot.total_count == 0) return std::nullopt;

    return "📌 Tasks: " + std::to_string(snapshot.active_count) +
           " active · " + std::to_string(snapshot.total_count) + " total · agent-local";
}

// ============================================================================
// Media Generation Task Status (对齐 OC: agents/media-generation-task-status-shared.ts)
// ============================================================================

bool is_active_media_generation_task(
    const tasks::TaskRecord& task,
    const std::string& task_kind) {
    return task.runtime == tasks::TaskRuntime::CLI &&
           task.scope_kind == tasks::TaskScopeKind::SESSION &&
           task.task_kind.value_or("") == task_kind &&
           (task.status == tasks::TaskStatus::QUEUED || task.status == tasks::TaskStatus::RUNNING);
}

std::optional<std::string> get_media_generation_task_provider_id(
    const tasks::TaskRecord& task,
    const std::string& source_prefix) {
    std::string sid = task.source_id.value_or("");
    std::string prefix = source_prefix + ":";
    if (sid.rfind(prefix, 0) != 0) return std::nullopt;
    std::string provider_id = sid.substr(prefix.size());
    // Trim
    size_t s = provider_id.find_first_not_of(" \t");
    if (s == std::string::npos) return std::nullopt;
    provider_id = provider_id.substr(s);
    return provider_id.empty() ? std::nullopt : std::make_optional(provider_id);
}

std::optional<tasks::TaskRecord> find_active_media_generation_task_for_session(
    tasks::TaskRegistry& registry,
    const std::string& session_key,
    const std::string& task_kind,
    const std::string& source_prefix) {
    // Delegate to the existing find_active_session_task in task_registry_maintenance
    return tasks::find_active_session_task(
        registry, session_key,
        tasks::TaskRuntime::CLI,
        task_kind,
        source_prefix);
}

std::string build_media_generation_task_status_text(
    const tasks::TaskRecord& task,
    const std::string& source_prefix,
    const std::string& noun_label,
    const std::string& tool_name,
    const std::string& completion_label,
    bool duplicate_guard) {

    auto provider = get_media_generation_task_provider_id(task, source_prefix);
    std::ostringstream oss;
    oss << noun_label << " task " << task.task_id << " is already "
        << tasks::task_status_string(task.status);
    if (provider) oss << " with " << *provider;
    oss << ".";
    if (task.progress_summary && !task.progress_summary->empty()) {
        oss << "\nProgress: " << *task.progress_summary << ".";
    }
    if (duplicate_guard) {
        oss << "\nDo not call " << tool_name << " again for this request. "
            << "Wait for the completion event; I will post the finished "
            << completion_label << " here.";
    } else {
        oss << "\nWait for the completion event; I will post the finished "
            << completion_label << " here when it's ready.";
    }
    return oss.str();
}

std::optional<std::string> build_active_media_generation_task_prompt_context_for_session(
    tasks::TaskRegistry& registry,
    const std::string& session_key,
    const std::string& task_kind,
    const std::string& source_prefix,
    const std::string& noun_label,
    const std::string& tool_name,
    const std::string& completion_label) {

    auto task = find_active_media_generation_task_for_session(
        registry, session_key, task_kind, source_prefix);
    if (!task) return std::nullopt;

    auto provider = get_media_generation_task_provider_id(*task, source_prefix);

    std::ostringstream oss;
    oss << "An active " << noun_label << " background task already exists for this session.\n";
    oss << "Task " << task->task_id << " is currently "
        << tasks::task_status_string(task->status);
    if (provider) oss << " via " << *provider;
    oss << ".\n";
    if (task->progress_summary && !task->progress_summary->empty()) {
        oss << "Current progress: " << *task->progress_summary << ".\n";
    }
    oss << "Do not call `" << tool_name
        << "` again for the same request while that task is queued or running.\n";
    oss << "If the user asks for progress or whether the work is async, explain the active task state or call `"
        << tool_name << "` with `action:\"status\"` instead of starting a new generation.\n";
    oss << "Only start a new `" << tool_name << "` call if the user clearly asks for different/new "
        << completion_label << ".";
    return oss.str();
}

} // namespace auto_reply
} // namespace hunterclaw
