// 严格对齐 OpenClaw: tasks/task-registry.store.sqlite.ts (541行)
// + tasks/task-registry.paths.ts (23行)
// SQLite persistence backend for TaskRegistry

#include "hunterclaw/tasks/task_registry_sqlite_store.h"
#include <sqlite3.h>
#include <filesystem>
#include <mutex>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace tasks {

namespace fs = std::filesystem;

// ============================================================================
// Paths (对齐 OC: task-registry.paths.ts)
// ============================================================================

std::string resolve_task_state_dir(const std::string& state_dir) {
    std::string dir = state_dir;
    if (dir.empty()) {
        const char* env = std::getenv("HUNTERCLAW_STATE_DIR");
        if (env && *env) dir = env;
        else dir = (fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".hunterclaw" / "state").string();
    }
    return dir;
}

std::string resolve_task_registry_dir(const std::string& state_dir) {
    return (fs::path(resolve_task_state_dir(state_dir)) / "tasks").string();
}

std::string resolve_task_registry_sqlite_path(const std::string& state_dir) {
    return (fs::path(resolve_task_registry_dir(state_dir)) / "runs.sqlite").string();
}

// ============================================================================
// Internal SQLite helpers
// ============================================================================

namespace {

static auto log() { return spdlog::get("default"); }

bool exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (log()) log()->warn("[task-sqlite] exec error: {} — {}", sql, err ? err : "");
        sqlite3_free(err);
        return false;
    }
    return true;
}

// Bind optional string
void bind_text_or_null(sqlite3_stmt* stmt, int idx, const std::optional<std::string>& val) {
    if (val && !val->empty()) sqlite3_bind_text(stmt, idx, val->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, idx);
}
void bind_text(sqlite3_stmt* stmt, int idx, const std::string& val) {
    sqlite3_bind_text(stmt, idx, val.c_str(), -1, SQLITE_TRANSIENT);
}
void bind_int64_or_null(sqlite3_stmt* stmt, int idx, const std::optional<int64_t>& val) {
    if (val) sqlite3_bind_int64(stmt, idx, *val);
    else sqlite3_bind_null(stmt, idx);
}

std::string get_text(sqlite3_stmt* stmt, int col) {
    auto p = sqlite3_column_text(stmt, col);
    return p ? std::string(reinterpret_cast<const char*>(p)) : "";
}
std::optional<std::string> get_text_opt(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    auto p = sqlite3_column_text(stmt, col);
    return p ? std::make_optional(std::string(reinterpret_cast<const char*>(p))) : std::nullopt;
}
std::optional<int64_t> get_int64_opt(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int64(stmt, col);
}

// ============================================================================
// Schema (对齐 OC: ensureSchema — task_runs 24 columns + task_delivery_state 3 columns + 8 indexes)
// ============================================================================

void ensure_schema(sqlite3* db) {
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS task_runs ("
        "  task_id TEXT PRIMARY KEY,"
        "  runtime TEXT NOT NULL,"
        "  source_id TEXT,"
        "  owner_key TEXT NOT NULL,"
        "  scope_kind TEXT NOT NULL,"
        "  child_session_key TEXT,"
        "  parent_flow_id TEXT,"
        "  parent_task_id TEXT,"
        "  agent_id TEXT,"
        "  run_id TEXT,"
        "  label TEXT,"
        "  task TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  delivery_status TEXT NOT NULL,"
        "  notify_policy TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  started_at INTEGER,"
        "  ended_at INTEGER,"
        "  last_event_at INTEGER,"
        "  cleanup_after INTEGER,"
        "  error TEXT,"
        "  progress_summary TEXT,"
        "  terminal_summary TEXT,"
        "  terminal_outcome TEXT"
        ");");

    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS task_delivery_state ("
        "  task_id TEXT PRIMARY KEY,"
        "  requester_origin_json TEXT,"
        "  last_notified_event_at INTEGER"
        ");");

    // 8 indexes (对齐 OC)
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_task_runs_run_id ON task_runs(run_id);");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_task_runs_status ON task_runs(status);");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_task_runs_runtime_status ON task_runs(runtime, status);");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_task_runs_cleanup_after ON task_runs(cleanup_after);");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_task_runs_last_event_at ON task_runs(last_event_at);");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_task_runs_owner_key ON task_runs(owner_key);");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_task_runs_parent_flow_id ON task_runs(parent_flow_id);");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_task_runs_child_session_key ON task_runs(child_session_key);");
}

// ============================================================================
// Row ↔ TaskRecord mapping
// ============================================================================

TaskRecord row_to_task_record(sqlite3_stmt* stmt) {
    TaskRecord r;
    r.task_id = get_text(stmt, 0);
    r.runtime = task_runtime_from_string(get_text(stmt, 1)).value_or(TaskRuntime::SUBAGENT);
    r.source_id = get_text_opt(stmt, 2);
    r.owner_key = get_text(stmt, 3);
    r.scope_kind = (get_text(stmt, 4) == "system") ? TaskScopeKind::SYSTEM : TaskScopeKind::SESSION;
    r.child_session_key = get_text_opt(stmt, 5);
    r.parent_flow_id = get_text_opt(stmt, 6);
    r.parent_task_id = get_text_opt(stmt, 7);
    r.agent_id = get_text_opt(stmt, 8);
    r.run_id = get_text_opt(stmt, 9);
    r.label = get_text_opt(stmt, 10);
    r.task = get_text(stmt, 11);
    r.status = task_status_from_string(get_text(stmt, 12)).value_or(TaskStatus::QUEUED);
    r.delivery_status = task_delivery_status_from_string(get_text(stmt, 13)).value_or(TaskDeliveryStatus::PENDING);
    r.notify_policy = task_notify_policy_from_string(get_text(stmt, 14)).value_or(TaskNotifyPolicy::DONE_ONLY);
    r.created_at = sqlite3_column_int64(stmt, 15);
    r.started_at = get_int64_opt(stmt, 16);
    r.ended_at = get_int64_opt(stmt, 17);
    r.last_event_at = get_int64_opt(stmt, 18);
    r.cleanup_after = get_int64_opt(stmt, 19);
    r.error = get_text_opt(stmt, 20);
    r.progress_summary = get_text_opt(stmt, 21);
    r.terminal_summary = get_text_opt(stmt, 22);
    auto outcome_str = get_text_opt(stmt, 23);
    if (outcome_str) r.terminal_outcome = task_terminal_outcome_from_string(*outcome_str);
    return r;
}

} // anonymous namespace

// ============================================================================
// SQLite Store Implementation
// ============================================================================

class SqliteTaskRegistryStoreImpl {
public:
    SqliteTaskRegistryStoreImpl(const std::string& db_path) : db_path_(db_path) {
        // Ensure directory exists
        fs::create_directories(fs::path(db_path).parent_path());
        // Open DB
        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            if (log()) log()->error("[task-sqlite] Failed to open {}: {}", db_path, sqlite3_errmsg(db_));
            db_ = nullptr;
            return;
        }
        // PRAGMA (对齐 OC)
        exec_sql(db_, "PRAGMA journal_mode = WAL;");
        exec_sql(db_, "PRAGMA synchronous = NORMAL;");
        exec_sql(db_, "PRAGMA busy_timeout = 5000;");
        ensure_schema(db_);
        if (log()) log()->info("[task-sqlite] Opened task registry store: {}", db_path);
    }

    ~SqliteTaskRegistryStoreImpl() {
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
    }

    TaskRegistrySnapshot load_snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        TaskRegistrySnapshot snap;
        if (!db_) return snap;

        // Load task_runs
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT task_id,runtime,source_id,owner_key,scope_kind,"
            "child_session_key,parent_flow_id,parent_task_id,agent_id,run_id,"
            "label,task,status,delivery_status,notify_policy,"
            "created_at,started_at,ended_at,last_event_at,cleanup_after,"
            "error,progress_summary,terminal_summary,terminal_outcome "
            "FROM task_runs ORDER BY created_at ASC, task_id ASC",
            -1, &stmt, nullptr);
        if (stmt) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                snap.tasks.push_back(row_to_task_record(stmt));
            }
            sqlite3_finalize(stmt);
        }

        // Load delivery states
        sqlite3_prepare_v2(db_,
            "SELECT task_id, requester_origin_json, last_notified_event_at "
            "FROM task_delivery_state ORDER BY task_id ASC",
            -1, &stmt, nullptr);
        if (stmt) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                TaskDeliveryState s;
                s.task_id = get_text(stmt, 0);
                // requester_origin_json is stored but not parsed in C++ (simplified)
                s.last_notified_event_at = get_int64_opt(stmt, 2);
                snap.delivery_states.push_back(s);
            }
            sqlite3_finalize(stmt);
        }
        return snap;
    }

    void save_snapshot(const TaskRegistrySnapshot& snap) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return;
        exec_sql(db_, "BEGIN IMMEDIATE;");
        exec_sql(db_, "DELETE FROM task_delivery_state;");
        exec_sql(db_, "DELETE FROM task_runs;");
        for (const auto& task : snap.tasks) upsert_task_unlocked(task);
        for (const auto& state : snap.delivery_states) upsert_delivery_state_unlocked(state);
        exec_sql(db_, "COMMIT;");
    }

    void upsert_task(const TaskRecord& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return;
        upsert_task_unlocked(task);
    }

    void delete_task(const std::string& task_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "DELETE FROM task_runs WHERE task_id = ?", -1, &stmt, nullptr);
        if (stmt) {
            bind_text(stmt, 1, task_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        sqlite3_prepare_v2(db_, "DELETE FROM task_delivery_state WHERE task_id = ?", -1, &stmt, nullptr);
        if (stmt) {
            bind_text(stmt, 1, task_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void upsert_delivery_state(const TaskDeliveryState& state) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return;
        upsert_delivery_state_unlocked(state);
    }

    void delete_delivery_state(const std::string& task_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "DELETE FROM task_delivery_state WHERE task_id = ?", -1, &stmt, nullptr);
        if (stmt) {
            bind_text(stmt, 1, task_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
    }

private:
    void upsert_task_unlocked(const TaskRecord& r) {
        static const char* SQL =
            "INSERT INTO task_runs (task_id,runtime,source_id,owner_key,scope_kind,"
            "child_session_key,parent_flow_id,parent_task_id,agent_id,run_id,"
            "label,task,status,delivery_status,notify_policy,"
            "created_at,started_at,ended_at,last_event_at,cleanup_after,"
            "error,progress_summary,terminal_summary,terminal_outcome)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(task_id) DO UPDATE SET "
            "runtime=excluded.runtime,source_id=excluded.source_id,"
            "owner_key=excluded.owner_key,scope_kind=excluded.scope_kind,"
            "child_session_key=excluded.child_session_key,parent_flow_id=excluded.parent_flow_id,"
            "parent_task_id=excluded.parent_task_id,agent_id=excluded.agent_id,"
            "run_id=excluded.run_id,label=excluded.label,task=excluded.task,"
            "status=excluded.status,delivery_status=excluded.delivery_status,"
            "notify_policy=excluded.notify_policy,created_at=excluded.created_at,"
            "started_at=excluded.started_at,ended_at=excluded.ended_at,"
            "last_event_at=excluded.last_event_at,cleanup_after=excluded.cleanup_after,"
            "error=excluded.error,progress_summary=excluded.progress_summary,"
            "terminal_summary=excluded.terminal_summary,terminal_outcome=excluded.terminal_outcome";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, SQL, -1, &stmt, nullptr) != SQLITE_OK) return;
        int i = 1;
        bind_text(stmt, i++, r.task_id);
        bind_text(stmt, i++, task_runtime_string(r.runtime));
        bind_text_or_null(stmt, i++, r.source_id);
        bind_text(stmt, i++, r.owner_key);
        bind_text(stmt, i++, task_scope_kind_string(r.scope_kind));
        bind_text_or_null(stmt, i++, r.child_session_key);
        bind_text_or_null(stmt, i++, r.parent_flow_id);
        bind_text_or_null(stmt, i++, r.parent_task_id);
        bind_text_or_null(stmt, i++, r.agent_id);
        bind_text_or_null(stmt, i++, r.run_id);
        bind_text_or_null(stmt, i++, r.label);
        bind_text(stmt, i++, r.task);
        bind_text(stmt, i++, task_status_string(r.status));
        bind_text(stmt, i++, task_delivery_status_string(r.delivery_status));
        bind_text(stmt, i++, task_notify_policy_string(r.notify_policy));
        sqlite3_bind_int64(stmt, i++, r.created_at);
        bind_int64_or_null(stmt, i++, r.started_at);
        bind_int64_or_null(stmt, i++, r.ended_at);
        bind_int64_or_null(stmt, i++, r.last_event_at);
        bind_int64_or_null(stmt, i++, r.cleanup_after);
        bind_text_or_null(stmt, i++, r.error);
        bind_text_or_null(stmt, i++, r.progress_summary);
        bind_text_or_null(stmt, i++, r.terminal_summary);
        auto outcome = r.terminal_outcome
            ? std::make_optional(task_terminal_outcome_string(*r.terminal_outcome))
            : std::nullopt;
        bind_text_or_null(stmt, i++, outcome);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    void upsert_delivery_state_unlocked(const TaskDeliveryState& s) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO task_delivery_state "
            "(task_id, requester_origin_json, last_notified_event_at) VALUES (?,?,?)",
            -1, &stmt, nullptr);
        if (!stmt) return;
        bind_text(stmt, 1, s.task_id);
        sqlite3_bind_null(stmt, 2); // requester_origin_json (simplified)
        bind_int64_or_null(stmt, 3, s.last_notified_event_at);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    std::string db_path_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

// ============================================================================
// Factory (对齐 OC: openTaskRegistryDatabase + createStatements)
// ============================================================================

std::shared_ptr<TaskRegistryStore> make_sqlite_task_registry_store(const std::string& db_path) {
    auto impl = std::make_shared<SqliteTaskRegistryStoreImpl>(db_path);
    auto store = std::make_shared<TaskRegistryStore>();
    store->load_snapshot = [impl]() { return impl->load_snapshot(); };
    store->save_snapshot = [impl](const TaskRegistrySnapshot& s) { impl->save_snapshot(s); };
    store->upsert_task = [impl](const TaskRecord& t) { impl->upsert_task(t); };
    store->delete_task = [impl](const std::string& id) { impl->delete_task(id); };
    store->upsert_delivery_state = [impl](const TaskDeliveryState& s) { impl->upsert_delivery_state(s); };
    store->delete_delivery_state = [impl](const std::string& id) { impl->delete_delivery_state(id); };
    store->close = [impl]() { impl->close(); };
    return store;
}

} // namespace tasks
} // namespace hunterclaw
