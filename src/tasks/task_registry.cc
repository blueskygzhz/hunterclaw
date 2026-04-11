#include "hunterclaw/tasks/task_registry.h"
#include "hunterclaw/core/utils.h"
#include <algorithm>
#include <chrono>
#include <random>

namespace hunterclaw {
namespace tasks {

// ============================================================================
// TaskRegistrySummary helpers
// ============================================================================

TaskRegistrySummary create_empty_task_registry_summary() {
    TaskRegistrySummary s;
    for (auto st : {"queued","running","succeeded","failed","timed_out","cancelled","lost"}) {
        s.by_status[st] = 0;
    }
    for (auto rt : {"subagent","acp","cli","cron"}) {
        s.by_runtime[rt] = 0;
    }
    return s;
}

TaskRegistrySummary summarize_task_records(const std::vector<TaskRecord>& records) {
    auto summary = create_empty_task_registry_summary();
    for (const auto& task : records) {
        summary.total++;
        summary.by_status[task_status_string(task.status)]++;
        summary.by_runtime[task_runtime_string(task.runtime)]++;
        if (is_active_task_status(task.status)) {
            summary.active++;
        } else {
            summary.terminal++;
        }
        if (task.status == TaskStatus::FAILED || task.status == TaskStatus::TIMED_OUT || task.status == TaskStatus::LOST) {
            summary.failures++;
        }
    }
    return summary;
}

// ============================================================================
// UUID generation
// ============================================================================

namespace {
    std::string generate_uuid() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<uint64_t> dist;
        uint64_t a = dist(gen), b = dist(gen);
        char buf[37];
        snprintf(buf, sizeof(buf),
            "%08x-%04x-%04x-%04x-%012llx",
            (uint32_t)(a >> 32),
            (uint16_t)(a >> 16),
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
} // namespace

// ============================================================================
// TaskRegistry
// ============================================================================

TaskRegistry::TaskRegistry() = default;
TaskRegistry::~TaskRegistry() = default;

void TaskRegistry::configure_store(std::shared_ptr<TaskRegistryStore> store) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_ = std::move(store);
}

void TaskRegistry::ensure_ready() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (restored_) return;
    restored_ = true;
    if (!store_ || !store_->load_snapshot) return;
    try {
        auto snapshot = store_->load_snapshot();
        for (const auto& task : snapshot.tasks) {
            tasks_[task.task_id] = task;
        }
        for (const auto& state : snapshot.delivery_states) {
            delivery_states_[state.task_id] = state;
        }
        rebuild_all_indexes();
    } catch (...) {
        // restore failure is non-fatal
    }
}

void TaskRegistry::rebuild_all_indexes() {
    by_run_id_.clear();
    by_owner_key_.clear();
    by_parent_flow_id_.clear();
    by_related_session_key_.clear();
    for (const auto& [id, task] : tasks_) {
        add_to_indexes(task);
    }
}

void TaskRegistry::add_to_indexes(const TaskRecord& task) {
    if (task.run_id && !task.run_id->empty()) {
        by_run_id_[*task.run_id].insert(task.task_id);
    }
    if (!task.owner_key.empty()) {
        by_owner_key_[task.owner_key].insert(task.task_id);
    }
    if (task.parent_flow_id && !task.parent_flow_id->empty()) {
        by_parent_flow_id_[*task.parent_flow_id].insert(task.task_id);
    }
    if (!task.owner_key.empty()) {
        by_related_session_key_[task.owner_key].insert(task.task_id);
    }
    if (task.child_session_key && !task.child_session_key->empty()) {
        by_related_session_key_[*task.child_session_key].insert(task.task_id);
    }
}

void TaskRegistry::remove_from_indexes(const TaskRecord& task) {
    if (task.run_id && !task.run_id->empty()) {
        by_run_id_[*task.run_id].erase(task.task_id);
    }
    if (!task.owner_key.empty()) {
        by_owner_key_[task.owner_key].erase(task.task_id);
    }
    if (task.parent_flow_id && !task.parent_flow_id->empty()) {
        by_parent_flow_id_[*task.parent_flow_id].erase(task.task_id);
    }
    if (!task.owner_key.empty()) {
        by_related_session_key_[task.owner_key].erase(task.task_id);
    }
    if (task.child_session_key && !task.child_session_key->empty()) {
        by_related_session_key_[*task.child_session_key].erase(task.task_id);
    }
}

void TaskRegistry::persist_upsert(const TaskRecord& task) {
    if (store_ && store_->upsert_task) {
        store_->upsert_task(task);
    }
}

void TaskRegistry::persist_delete(const std::string& task_id) {
    if (store_ && store_->delete_task) {
        store_->delete_task(task_id);
    }
}

// ──── CRUD ────

TaskRecord TaskRegistry::create_task(const TaskRecord& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    TaskRecord record = params;
    record.task_id = generate_uuid();
    if (record.created_at == 0) record.created_at = now_ms();
    if (!record.last_event_at) record.last_event_at = record.created_at;

    if (is_terminal_task_status(record.status) && !record.cleanup_after) {
        int64_t terminal_at = record.ended_at.value_or(record.last_event_at.value_or(record.created_at));
        record.cleanup_after = terminal_at + DEFAULT_TASK_RETENTION_MS;
    }

    tasks_[record.task_id] = record;
    add_to_indexes(record);
    persist_upsert(record);
    return record;
}

std::optional<TaskRecord> TaskRegistry::update_task(const std::string& task_id, const TaskRecord& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return std::nullopt;

    auto& current = it->second;
    remove_from_indexes(current);

    // Apply patch fields (non-empty values override)
    if (patch.status != TaskStatus::QUEUED || current.status == TaskStatus::QUEUED) {
        current.status = patch.status;
    }
    if (patch.started_at) current.started_at = patch.started_at;
    if (patch.ended_at) current.ended_at = patch.ended_at;
    if (patch.last_event_at) current.last_event_at = patch.last_event_at;
    if (patch.error) current.error = patch.error;
    if (patch.progress_summary) current.progress_summary = patch.progress_summary;
    if (patch.terminal_summary) current.terminal_summary = patch.terminal_summary;
    if (patch.terminal_outcome) current.terminal_outcome = patch.terminal_outcome;
    if (patch.cleanup_after) current.cleanup_after = patch.cleanup_after;
    if (patch.parent_flow_id) current.parent_flow_id = patch.parent_flow_id;

    if (is_terminal_task_status(current.status) && !current.cleanup_after) {
        int64_t terminal_at = current.ended_at.value_or(current.last_event_at.value_or(now_ms()));
        current.cleanup_after = terminal_at + DEFAULT_TASK_RETENTION_MS;
    }

    add_to_indexes(current);
    persist_upsert(current);
    return current;
}

bool TaskRegistry::delete_task(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return false;
    remove_from_indexes(it->second);
    tasks_.erase(it);
    delivery_states_.erase(task_id);
    persist_delete(task_id);
    return true;
}

// ──── Query ────

std::optional<TaskRecord> TaskRegistry::get_task_by_id(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(trim(task_id));
    return it != tasks_.end() ? std::make_optional(it->second) : std::nullopt;
}

std::vector<TaskRecord> TaskRegistry::get_tasks_by_run_id_unlocked(const std::string& run_id) const {
    auto it = by_run_id_.find(trim(run_id));
    if (it == by_run_id_.end()) return {};
    std::vector<TaskRecord> result;
    for (const auto& task_id : it->second) {
        auto task_it = tasks_.find(task_id);
        if (task_it != tasks_.end()) result.push_back(task_it->second);
    }
    return result;
}

std::optional<TaskRecord> TaskRegistry::find_task_by_run_id(const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto matches = get_tasks_by_run_id_unlocked(run_id);
    if (matches.empty()) return std::nullopt;
    // Prefer CLI runtime, then oldest
    std::sort(matches.begin(), matches.end(), [](const TaskRecord& a, const TaskRecord& b) {
        int pa = (a.runtime == TaskRuntime::CLI) ? 1 : 0;
        int pb = (b.runtime == TaskRuntime::CLI) ? 1 : 0;
        if (pa != pb) return pa < pb;
        return a.created_at < b.created_at;
    });
    return matches[0];
}

std::vector<TaskRecord> TaskRegistry::list_from_index(
    const std::map<std::string, std::set<std::string>>& index,
    const std::string& key) const {
    auto it = index.find(trim(key));
    if (it == index.end()) return {};
    std::vector<TaskRecord> result;
    for (const auto& task_id : it->second) {
        auto task_it = tasks_.find(task_id);
        if (task_it != tasks_.end()) result.push_back(task_it->second);
    }
    std::sort(result.begin(), result.end(), [](const TaskRecord& a, const TaskRecord& b) {
        return a.created_at > b.created_at;
    });
    return result;
}

std::vector<TaskRecord> TaskRegistry::list_task_records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TaskRecord> result;
    result.reserve(tasks_.size());
    for (const auto& [id, task] : tasks_) result.push_back(task);
    std::sort(result.begin(), result.end(), [](const TaskRecord& a, const TaskRecord& b) {
        return a.created_at > b.created_at;
    });
    return result;
}

std::vector<TaskRecord> TaskRegistry::list_tasks_for_session_key(const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return list_from_index(by_related_session_key_, session_key);
}

std::vector<TaskRecord> TaskRegistry::list_tasks_for_owner_key(const std::string& owner_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return list_from_index(by_owner_key_, owner_key);
}

std::vector<TaskRecord> TaskRegistry::list_tasks_for_flow_id(const std::string& flow_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return list_from_index(by_parent_flow_id_, flow_id);
}

std::vector<TaskRecord> TaskRegistry::list_tasks_for_agent_id(const std::string& agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto lookup = trim(agent_id);
    std::vector<TaskRecord> result;
    for (const auto& [id, task] : tasks_) {
        if (task.agent_id && trim(*task.agent_id) == lookup) {
            result.push_back(task);
        }
    }
    std::sort(result.begin(), result.end(), [](const TaskRecord& a, const TaskRecord& b) {
        return a.created_at > b.created_at;
    });
    return result;
}

std::optional<TaskRecord> TaskRegistry::resolve_task_for_lookup_token(const std::string& token) const {
    auto lookup = trim(token);
    if (lookup.empty()) return std::nullopt;
    auto by_id = get_task_by_id(lookup);
    if (by_id) return by_id;
    auto by_run = find_task_by_run_id(lookup);
    if (by_run) return by_run;
    auto by_session = list_tasks_for_session_key(lookup);
    if (!by_session.empty()) return by_session[0];
    return std::nullopt;
}

// ──── Status mutations ────

std::optional<TaskRecord> TaskRegistry::mark_task_terminal_by_id(
    const std::string& task_id, TaskStatus status, int64_t ended_at,
    const std::optional<std::string>& error,
    const std::optional<std::string>& terminal_summary,
    const std::optional<TaskTerminalOutcome>& terminal_outcome) {
    TaskRecord patch;
    patch.status = status;
    patch.ended_at = ended_at;
    patch.last_event_at = ended_at;
    if (error) patch.error = error;
    if (terminal_summary) patch.terminal_summary = terminal_summary;
    if (terminal_outcome) patch.terminal_outcome = terminal_outcome;
    return update_task(task_id, patch);
}

std::optional<TaskRecord> TaskRegistry::mark_task_lost_by_id(
    const std::string& task_id, int64_t ended_at,
    const std::optional<std::string>& error) {
    return mark_task_terminal_by_id(task_id, TaskStatus::LOST, ended_at, error);
}

std::optional<TaskRecord> TaskRegistry::set_task_progress_by_id(
    const std::string& task_id,
    const std::optional<std::string>& progress_summary) {
    TaskRecord patch;
    patch.last_event_at = now_ms();
    if (progress_summary) patch.progress_summary = progress_summary;
    return update_task(task_id, patch);
}

std::vector<TaskRecord> TaskRegistry::mark_task_running_by_run_id(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto matches = get_tasks_by_run_id_unlocked(run_id);
    std::vector<TaskRecord> updated;
    for (auto& task : matches) {
        if (is_terminal_task_status(task.status)) continue;
        task.status = TaskStatus::RUNNING;
        task.started_at = now_ms();
        task.last_event_at = now_ms();
        tasks_[task.task_id] = task;
        persist_upsert(task);
        updated.push_back(task);
    }
    return updated;
}

std::vector<TaskRecord> TaskRegistry::mark_task_terminal_by_run_id(
    const std::string& run_id, TaskStatus status, int64_t ended_at,
    const std::optional<std::string>& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto matches = get_tasks_by_run_id_unlocked(run_id);
    std::vector<TaskRecord> updated;
    for (auto& task : matches) {
        if (is_terminal_task_status(task.status)) continue;
        remove_from_indexes(task);
        task.status = status;
        task.ended_at = ended_at;
        task.last_event_at = ended_at;
        if (error) task.error = error;
        if (!task.cleanup_after) {
            task.cleanup_after = ended_at + DEFAULT_TASK_RETENTION_MS;
        }
        add_to_indexes(task);
        tasks_[task.task_id] = task;
        persist_upsert(task);
        updated.push_back(task);
    }
    return updated;
}

TaskRegistrySnapshot TaskRegistry::get_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    TaskRegistrySnapshot snap;
    for (const auto& [id, task] : tasks_) snap.tasks.push_back(task);
    for (const auto& [id, state] : delivery_states_) snap.delivery_states.push_back(state);
    return snap;
}

TaskRegistrySummary TaskRegistry::get_summary() const {
    return summarize_task_records(list_task_records());
}

TaskRegistry::CancelResult TaskRegistry::cancel_task_by_id(const std::string& task_id) {
    auto task = get_task_by_id(task_id);
    if (!task) return {false, false, "Task not found."};
    if (is_terminal_task_status(task->status)) {
        return {true, false, "Task is already terminal.", task};
    }
    auto updated = mark_task_terminal_by_id(task_id, TaskStatus::CANCELLED, now_ms(), "Cancelled by operator.");
    return {true, true, std::nullopt, updated};
}

std::optional<TaskRecord> TaskRegistry::link_task_to_flow_by_id(
    const std::string& task_id, const std::string& flow_id) {
    auto fid = trim(flow_id);
    if (fid.empty()) return std::nullopt;
    auto task = get_task_by_id(task_id);
    if (!task) return std::nullopt;
    if (task->parent_flow_id && !task->parent_flow_id->empty()) return task;
    TaskRecord patch;
    patch.parent_flow_id = fid;
    return update_task(task_id, patch);
}

} // namespace tasks
} // namespace hunterclaw
