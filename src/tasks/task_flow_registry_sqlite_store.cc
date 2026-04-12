// 严格对齐 OpenClaw: tasks/task-flow-registry.store.sqlite.ts (405行)
// + tasks/task-flow-registry.paths.ts (11行)

#include "hunterclaw/tasks/task_flow_registry_sqlite_store.h"
#include "hunterclaw/tasks/task_registry_sqlite_store.h" // resolve_task_state_dir
#include <sqlite3.h>
#include <filesystem>
#include <mutex>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace tasks {

namespace fs = std::filesystem;

std::string resolve_task_flow_registry_dir(const std::string& state_dir) {
    return (fs::path(resolve_task_state_dir(state_dir)) / "flows").string();
}

std::string resolve_task_flow_registry_sqlite_path(const std::string& state_dir) {
    return (fs::path(resolve_task_flow_registry_dir(state_dir)) / "registry.sqlite").string();
}

namespace {

static auto log() { return spdlog::get("default"); }

bool exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (log()) log()->warn("[flow-sqlite] exec error: {} — {}", sql, err ? err : "");
        sqlite3_free(err);
        return false;
    }
    return true;
}

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

void ensure_flow_schema(sqlite3* db) {
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS flow_runs ("
        "  flow_id TEXT PRIMARY KEY,"
        "  sync_mode TEXT NOT NULL DEFAULT 'managed',"
        "  owner_key TEXT NOT NULL,"
        "  requester_origin_json TEXT,"
        "  controller_id TEXT,"
        "  revision INTEGER NOT NULL DEFAULT 0,"
        "  status TEXT NOT NULL,"
        "  notify_policy TEXT NOT NULL,"
        "  goal TEXT NOT NULL,"
        "  current_step TEXT,"
        "  blocked_task_id TEXT,"
        "  blocked_summary TEXT,"
        "  state_json TEXT,"
        "  wait_json TEXT,"
        "  cancel_requested_at INTEGER,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  ended_at INTEGER"
        ");");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_flow_runs_status ON flow_runs(status);");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_flow_runs_owner_key ON flow_runs(owner_key);");
    exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_flow_runs_updated_at ON flow_runs(updated_at);");
}

TaskFlowRecord row_to_flow(sqlite3_stmt* s) {
    TaskFlowRecord f;
    f.flow_id = get_text(s, 0);
    auto sm = get_text(s, 1);
    f.sync_mode = (sm == "task_mirrored") ? TaskFlowSyncMode::TASK_MIRRORED : TaskFlowSyncMode::MANAGED;
    f.owner_key = get_text(s, 2);
    // requester_origin_json col 3 (simplified — not parsed)
    f.controller_id = get_text_opt(s, 4);
    f.revision = sqlite3_column_type(s, 5) != SQLITE_NULL ? (int)sqlite3_column_int64(s, 5) : 0;
    auto status_str = get_text(s, 6);
    // Parse status
    if (status_str == "running") f.status = TaskFlowStatus::RUNNING;
    else if (status_str == "waiting") f.status = TaskFlowStatus::WAITING;
    else if (status_str == "blocked") f.status = TaskFlowStatus::BLOCKED;
    else if (status_str == "succeeded") f.status = TaskFlowStatus::SUCCEEDED;
    else if (status_str == "failed") f.status = TaskFlowStatus::FAILED;
    else if (status_str == "cancelled") f.status = TaskFlowStatus::CANCELLED;
    else if (status_str == "lost") f.status = TaskFlowStatus::LOST;
    else f.status = TaskFlowStatus::QUEUED;
    auto np_str = get_text(s, 7);
    f.notify_policy = task_notify_policy_from_string(np_str).value_or(TaskNotifyPolicy::DONE_ONLY);
    f.goal = get_text(s, 8);
    f.current_step = get_text_opt(s, 9);
    f.blocked_task_id = get_text_opt(s, 10);
    f.blocked_summary = get_text_opt(s, 11);
    // state_json, wait_json (cols 12, 13) — simplified, not parsed to json
    f.cancel_requested_at = get_int64_opt(s, 14);
    f.created_at = sqlite3_column_int64(s, 15);
    f.updated_at = sqlite3_column_int64(s, 16);
    f.ended_at = get_int64_opt(s, 17);
    return f;
}

} // anonymous namespace

class SqliteFlowRegistryStoreImpl {
public:
    SqliteFlowRegistryStoreImpl(const std::string& db_path) {
        fs::create_directories(fs::path(db_path).parent_path());
        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            if (log()) log()->error("[flow-sqlite] Failed to open {}: {}", db_path, sqlite3_errmsg(db_));
            db_ = nullptr; return;
        }
        exec_sql(db_, "PRAGMA journal_mode = WAL;");
        exec_sql(db_, "PRAGMA synchronous = NORMAL;");
        exec_sql(db_, "PRAGMA busy_timeout = 5000;");
        ensure_flow_schema(db_);
        if (log()) log()->info("[flow-sqlite] Opened flow registry store: {}", db_path);
    }
    ~SqliteFlowRegistryStoreImpl() { if (db_) { sqlite3_close(db_); db_ = nullptr; } }

    TaskFlowRegistrySnapshot load_snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        TaskFlowRegistrySnapshot snap;
        if (!db_) return snap;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT flow_id,sync_mode,owner_key,requester_origin_json,controller_id,"
            "revision,status,notify_policy,goal,current_step,"
            "blocked_task_id,blocked_summary,state_json,wait_json,cancel_requested_at,"
            "created_at,updated_at,ended_at FROM flow_runs ORDER BY created_at ASC",
            -1, &stmt, nullptr);
        if (stmt) {
            while (sqlite3_step(stmt) == SQLITE_ROW) snap.flows.push_back(row_to_flow(stmt));
            sqlite3_finalize(stmt);
        }
        return snap;
    }

    void save_snapshot(const TaskFlowRegistrySnapshot& snap) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!db_) return;
        exec_sql(db_, "BEGIN IMMEDIATE;");
        exec_sql(db_, "DELETE FROM flow_runs;");
        for (const auto& f : snap.flows) upsert_unlocked(f);
        exec_sql(db_, "COMMIT;");
    }

    void upsert_flow(const TaskFlowRecord& f) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!db_) return;
        upsert_unlocked(f);
    }

    void delete_flow(const std::string& flow_id) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!db_) return;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "DELETE FROM flow_runs WHERE flow_id = ?", -1, &stmt, nullptr);
        if (stmt) { bind_text(stmt, 1, flow_id); sqlite3_step(stmt); sqlite3_finalize(stmt); }
    }

    void close() { std::lock_guard<std::mutex> lk(mu_); if (db_) { sqlite3_close(db_); db_ = nullptr; } }

private:
    void upsert_unlocked(const TaskFlowRecord& f) {
        static const char* SQL =
            "INSERT INTO flow_runs (flow_id,sync_mode,owner_key,requester_origin_json,controller_id,"
            "revision,status,notify_policy,goal,current_step,"
            "blocked_task_id,blocked_summary,state_json,wait_json,cancel_requested_at,"
            "created_at,updated_at,ended_at)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(flow_id) DO UPDATE SET "
            "sync_mode=excluded.sync_mode,owner_key=excluded.owner_key,"
            "requester_origin_json=excluded.requester_origin_json,"
            "controller_id=excluded.controller_id,revision=excluded.revision,"
            "status=excluded.status,notify_policy=excluded.notify_policy,"
            "goal=excluded.goal,current_step=excluded.current_step,"
            "blocked_task_id=excluded.blocked_task_id,blocked_summary=excluded.blocked_summary,"
            "state_json=excluded.state_json,wait_json=excluded.wait_json,"
            "cancel_requested_at=excluded.cancel_requested_at,"
            "created_at=excluded.created_at,updated_at=excluded.updated_at,"
            "ended_at=excluded.ended_at";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, SQL, -1, &stmt, nullptr) != SQLITE_OK) return;
        int i = 1;
        bind_text(stmt, i++, f.flow_id);
        bind_text(stmt, i++, f.sync_mode == TaskFlowSyncMode::TASK_MIRRORED ? "task_mirrored" : "managed");
        bind_text(stmt, i++, f.owner_key);
        sqlite3_bind_null(stmt, i++); // requester_origin_json
        bind_text_or_null(stmt, i++, f.controller_id);
        sqlite3_bind_int64(stmt, i++, f.revision);
        bind_text(stmt, i++, task_flow_status_string(f.status));
        bind_text(stmt, i++, task_notify_policy_string(f.notify_policy));
        bind_text(stmt, i++, f.goal);
        bind_text_or_null(stmt, i++, f.current_step);
        bind_text_or_null(stmt, i++, f.blocked_task_id);
        bind_text_or_null(stmt, i++, f.blocked_summary);
        sqlite3_bind_null(stmt, i++); // state_json
        sqlite3_bind_null(stmt, i++); // wait_json
        bind_int64_or_null(stmt, i++, f.cancel_requested_at);
        sqlite3_bind_int64(stmt, i++, f.created_at);
        sqlite3_bind_int64(stmt, i++, f.updated_at);
        bind_int64_or_null(stmt, i++, f.ended_at);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3* db_ = nullptr;
    std::mutex mu_;
};

std::shared_ptr<TaskFlowRegistryStore> make_sqlite_task_flow_registry_store(const std::string& db_path) {
    auto impl = std::make_shared<SqliteFlowRegistryStoreImpl>(db_path);
    auto store = std::make_shared<TaskFlowRegistryStore>();
    store->load_snapshot = [impl]() { return impl->load_snapshot(); };
    store->save_snapshot = [impl](const TaskFlowRegistrySnapshot& s) { impl->save_snapshot(s); };
    store->upsert_flow = [impl](const TaskFlowRecord& f) { impl->upsert_flow(f); };
    store->delete_flow = [impl](const std::string& id) { impl->delete_flow(id); };
    store->close = [impl]() { impl->close(); };
    return store;
}

} // namespace tasks
} // namespace hunterclaw
