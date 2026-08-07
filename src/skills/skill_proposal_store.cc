// 🎯 对齐 OpenClaw skills/workshop/store-sqlite-*.ts 的 SQLite 持久化宿主层。
//   写路径 BEGIN IMMEDIATE + 进程内互斥；乐观并发校验对齐 commitPendingTransition。

#include "hunterclaw/skills/skill_proposal_store.h"
#include "hunterclaw/agents/agent_paths.h"
#include "hunterclaw/infra/utilities.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <chrono>
#include <mutex>
#include <string>

namespace hunterclaw {
namespace skills {

namespace {
using json = nlohmann::json;

std::mutex& write_mutex() { static std::mutex m; return m; }
std::string& test_db_path_override() { static std::string p; return p; }

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

void ensure_parent_dir(const std::string& file_path) {
    size_t slash = file_path.find_last_of('/');
    if (slash == std::string::npos) return;
    std::string dir = file_path.substr(0, slash), cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' || i + 1 == dir.size())
            if (cur != "/" && !cur.empty()) ::mkdir(cur.c_str(), 0700);
    }
}

const char* kCreateProposalsSql =
    "CREATE TABLE IF NOT EXISTS skill_proposals ("
    "  id TEXT NOT NULL PRIMARY KEY,"
    "  format_version INTEGER NOT NULL CHECK (format_version = 1),"
    "  status TEXT NOT NULL,"
    "  record_json TEXT NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ")";
const char* kCreateEventsSql =
    "CREATE TABLE IF NOT EXISTS skill_proposal_events ("
    "  event_id TEXT NOT NULL PRIMARY KEY,"
    "  proposal_id TEXT NOT NULL,"
    "  sequence INTEGER NOT NULL,"
    "  event_json TEXT NOT NULL,"
    "  occurred_at INTEGER NOT NULL"
    ")";

core::Result<sqlite3*> open_db(bool create_tables) {
    std::string path = resolve_skill_proposal_state_db_path();
    ensure_parent_dir(path);
    sqlite3* db = nullptr;
    int flags = create_tables ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY;
    if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
        std::string e = db ? sqlite3_errmsg(db) : "open failed";
        if (db) sqlite3_close(db);
        return core::Result<sqlite3*>::Err("skill_proposal db open failed: " + e);
    }
    sqlite3_busy_timeout(db, 5000);
    if (create_tables) {
        char* err = nullptr;
        if (sqlite3_exec(db, kCreateProposalsSql, nullptr, nullptr, &err) != SQLITE_OK ||
            sqlite3_exec(db, kCreateEventsSql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "create table failed";
            sqlite3_free(err); sqlite3_close(db);
            return core::Result<sqlite3*>::Err("skill_proposal db init failed: " + e);
        }
    }
    return core::Result<sqlite3*>::Ok(db);
}

int64_t next_sequence(sqlite3* db, const std::string& proposal_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COALESCE(MAX(sequence),0)+1 FROM skill_proposal_events WHERE proposal_id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 1;
    sqlite3_bind_text(stmt, 1, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
    int64_t seq = 1;
    if (sqlite3_step(stmt) == SQLITE_ROW) seq = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return seq;
}

core::Result<void> insert_event_row(sqlite3* db, SkillProposalEvent& ev) {
    ev.sequence = next_sequence(db, ev.proposal_id);
    std::string ejson = ev.to_json().dump();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO skill_proposal_events "
        "(event_id, proposal_id, sequence, event_json, occurred_at) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return core::Result<void>::Err(std::string("prepare event failed: ") + sqlite3_errmsg(db));
    sqlite3_bind_text(stmt, 1, ev.event_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ev.proposal_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, ev.sequence);
    sqlite3_bind_text(stmt, 4, ejson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, now_ms());
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return core::Result<void>::Err(std::string("insert event failed: ") + sqlite3_errmsg(db));
    return core::Result<void>::Ok();
}

core::Result<void> upsert_proposal_row(sqlite3* db, const SkillProposalRecord& r) {
    std::string rjson = r.to_json().dump();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO skill_proposals "
        "(id, format_version, status, record_json, updated_at) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return core::Result<void>::Err(std::string("prepare proposal failed: ") + sqlite3_errmsg(db));
    sqlite3_bind_text(stmt, 1, r.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, SKILL_PROPOSAL_FORMAT_VERSION);
    std::string st = to_string(r.status);
    sqlite3_bind_text(stmt, 3, st.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rjson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, now_ms());
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return core::Result<void>::Err(std::string("upsert proposal failed: ") + sqlite3_errmsg(db));
    return core::Result<void>::Ok();
}

core::Result<std::optional<SkillProposalRecord>> read_row(sqlite3* db, const std::string& id) {
    using Ret = core::Result<std::optional<SkillProposalRecord>>;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT record_json FROM skill_proposals WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return Ret::Err(std::string("prepare read failed: ") + sqlite3_errmsg(db));
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<SkillProposalRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* j = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (j) {
            try { out = SkillProposalRecord::from_json(json::parse(j)); }
            catch (const std::exception& e) { sqlite3_finalize(stmt); return Ret::Err(std::string("corrupt record: ") + e.what()); }
        }
    }
    sqlite3_finalize(stmt);
    return Ret::Ok(std::move(out));
}

} // namespace

std::string resolve_skill_proposal_state_db_path() {
    if (!test_db_path_override().empty()) return test_db_path_override();
    return agents::resolve_state_dir() + "/state/hunterclaw.sqlite";
}
void set_skill_proposal_state_db_path_for_test(const std::string& path) {
    test_db_path_override() = path;
}

core::Result<std::optional<SkillProposalRecord>> read_skill_proposal(const std::string& id) {
    auto dbr = open_db(/*create=*/true);
    if (dbr.is_err()) return core::Result<std::optional<SkillProposalRecord>>::Err(dbr.error);
    sqlite3* db = dbr.get();
    auto res = read_row(db, id);
    sqlite3_close(db);
    return res;
}

core::Result<std::vector<SkillProposalRecord>> list_skill_proposals(
    std::optional<SkillProposalStatus> status_filter) {
    using Ret = core::Result<std::vector<SkillProposalRecord>>;
    auto dbr = open_db(true);
    if (dbr.is_err()) return Ret::Err(dbr.error);
    sqlite3* db = dbr.get();
    std::vector<SkillProposalRecord> out;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "SELECT record_json FROM skill_proposals";
    if (status_filter) sql += " WHERE status=?";
    sql += " ORDER BY updated_at DESC";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::string e = sqlite3_errmsg(db); sqlite3_close(db);
        return Ret::Err("prepare list failed: " + e);
    }
    if (status_filter) {
        std::string st = to_string(*status_filter);
        sqlite3_bind_text(stmt, 1, st.c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* j = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (j) { try { out.push_back(SkillProposalRecord::from_json(json::parse(j))); } catch (...) {} }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Ret::Ok(std::move(out));
}

core::Result<CreateProposalResult> insert_skill_proposal(const SkillProposalRecord& record) {
    using Ret = core::Result<CreateProposalResult>;
    std::lock_guard<std::mutex> lock(write_mutex());
    auto dbr = open_db(true);
    if (dbr.is_err()) return Ret::Err(dbr.error);
    sqlite3* db = dbr.get();

    char* err = nullptr;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "begin failed"; sqlite3_free(err); sqlite3_close(db);
        return Ret::Err("skill_proposal begin failed: " + e);
    }
    auto rollback = [&](const std::string& msg) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); sqlite3_close(db);
        return Ret::Err(msg);
    };
    auto w = upsert_proposal_row(db, record);
    if (w.is_err()) return rollback(w.error);

    SkillProposalEvent ev;
    ev.event_id = infra::generate_uuid();
    ev.proposal_id = record.id;
    ev.proposed_version = record.proposed_version;
    ev.revision_hash = hash_skill_proposal_revision(record);
    ev.type = SkillProposalEventType::Created;
    ev.occurred_at = record.created_at;
    auto ew = insert_event_row(db, ev);
    if (ew.is_err()) return rollback(ew.error);

    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "commit failed"; sqlite3_free(err);
        return rollback("commit failed: " + e);
    }
    sqlite3_close(db);
    return Ret::Ok(CreateProposalResult{record, ev});
}

core::Result<RecordEvaluationResult> record_skill_proposal_evaluation(
    const std::string& proposal_id, const std::string& expected_proposed_version,
    const std::string& expected_revision_hash, const SkillProposalEvaluation& evaluation,
    const SkillProposalEvent& event_in) {
    using Ret = core::Result<RecordEvaluationResult>;
    std::lock_guard<std::mutex> lock(write_mutex());
    auto dbr = open_db(true);
    if (dbr.is_err()) return Ret::Err(dbr.error);
    sqlite3* db = dbr.get();

    char* err = nullptr;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "begin failed"; sqlite3_free(err); sqlite3_close(db);
        return Ret::Err("begin failed: " + e);
    }
    auto rollback = [&](const std::string& msg) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); sqlite3_close(db);
        return Ret::Err(msg);
    };
    auto cur = read_row(db, proposal_id);
    if (cur.is_err()) return rollback(cur.error);
    if (!cur.get().has_value()) return rollback("skill proposal not found: " + proposal_id);
    SkillProposalRecord record = *cur.get();

    // 乐观并发校验（对齐 commitPendingSkillProposalTransition）。
    if (record.status != SkillProposalStatus::Pending)
        return rollback("Only pending proposals can be evaluated. Current: " + to_string(record.status));
    if (record.proposed_version != expected_proposed_version ||
        hash_skill_proposal_revision(record) != expected_revision_hash)
        return rollback("Skill proposal " + proposal_id + " changed while evaluation was running.");

    record.evaluation = evaluation;
    record.updated_at = event_in.occurred_at;
    auto w = upsert_proposal_row(db, record);
    if (w.is_err()) return rollback(w.error);

    SkillProposalEvent ev = event_in;
    auto ew = insert_event_row(db, ev);
    if (ew.is_err()) return rollback(ew.error);

    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "commit failed"; sqlite3_free(err);
        return rollback("commit failed: " + e);
    }
    sqlite3_close(db);
    return Ret::Ok(RecordEvaluationResult{record, ev});
}

core::Result<SkillProposalRecord> transition_skill_proposal_status(
    const std::string& proposal_id, SkillProposalStatus next_status,
    SkillProposalEventType event_type, std::optional<std::string> correlation_id) {
    using Ret = core::Result<SkillProposalRecord>;
    std::lock_guard<std::mutex> lock(write_mutex());
    auto dbr = open_db(true);
    if (dbr.is_err()) return Ret::Err(dbr.error);
    sqlite3* db = dbr.get();

    char* err = nullptr;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "begin failed"; sqlite3_free(err); sqlite3_close(db);
        return Ret::Err("begin failed: " + e);
    }
    auto rollback = [&](const std::string& msg) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); sqlite3_close(db);
        return Ret::Err(msg);
    };
    auto cur = read_row(db, proposal_id);
    if (cur.is_err()) return rollback(cur.error);
    if (!cur.get().has_value()) return rollback("skill proposal not found: " + proposal_id);
    SkillProposalRecord record = *cur.get();

    std::string ts = std::to_string(now_ms());
    record.status = next_status;
    record.updated_at = ts;
    auto w = upsert_proposal_row(db, record);
    if (w.is_err()) return rollback(w.error);

    SkillProposalEvent ev;
    ev.event_id = infra::generate_uuid();
    ev.proposal_id = record.id;
    ev.proposed_version = record.proposed_version;
    ev.revision_hash = hash_skill_proposal_revision(record);
    ev.type = event_type;
    ev.occurred_at = ts;
    ev.correlation_id = correlation_id;
    auto ew = insert_event_row(db, ev);
    if (ew.is_err()) return rollback(ew.error);

    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "commit failed"; sqlite3_free(err);
        return rollback("commit failed: " + e);
    }
    sqlite3_close(db);
    return Ret::Ok(std::move(record));
}

core::Result<std::vector<SkillProposalEvent>> list_skill_proposal_events(const std::string& proposal_id) {
    using Ret = core::Result<std::vector<SkillProposalEvent>>;
    auto dbr = open_db(true);
    if (dbr.is_err()) return Ret::Err(dbr.error);
    sqlite3* db = dbr.get();
    std::vector<SkillProposalEvent> out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT event_json FROM skill_proposal_events WHERE proposal_id=? ORDER BY sequence ASC";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::string e = sqlite3_errmsg(db); sqlite3_close(db);
        return Ret::Err("prepare events failed: " + e);
    }
    sqlite3_bind_text(stmt, 1, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* j = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (j) { try { out.push_back(SkillProposalEvent::from_json(json::parse(j))); } catch (...) {} }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Ret::Ok(std::move(out));
}

} // namespace skills
} // namespace hunterclaw
