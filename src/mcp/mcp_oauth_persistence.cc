#include "hunterclaw/mcp/mcp_oauth_persistence.h"
#include "hunterclaw/agents/agent_paths.h"

#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>
#include <mutex>

// 🎯 对齐 OpenClaw mcp-oauth-store.ts的持久化宿主层。

namespace hunterclaw {
namespace mcp {

namespace {

using json = nlohmann::json;

std::mutex& write_mutex() {
    static std::mutex m;
    return m;
}

std::string& test_db_path_override() {
    static std::string p;
    return p;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// mkdir -p（对齐打开 db 前确保目录存在）。
void ensure_parent_dir(const std::string& file_path) {
    size_t slash = file_path.find_last_of('/');
    if (slash == std::string::npos) return;
    std::string dir = file_path.substr(0, slash);
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' || i + 1 == dir.size()) {
            if (cur != "/" && !cur.empty()) {
                ::mkdir(cur.c_str(), 0700);
            }
        }
    }
}

const char* kCreateTableSql =
    "CREATE TABLE IF NOT EXISTS mcp_oauth_stores ("
    "  store_key TEXT NOT NULL PRIMARY KEY,"
    "  format_version INTEGER NOT NULL CHECK (format_version = 1),"
    "  store_json TEXT NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ")";  // 对齐 openclaw schema；去 STRICT 以兼容 <3.37 的 sqlite（语义等价）。

bool table_exists(sqlite3* db, const char* name) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

// 从 row 解析（对齐 storeFromRow）：无行→{}；format_version 不符→损坏。
core::Result<McpOAuthStore> store_from_row(
    const std::string& store_key, bool has_row, int format_version,
    const std::string& store_json) {
    if (!has_row) {
        return core::Result<McpOAuthStore>::Ok(McpOAuthStore{});
    }
    if (format_version != MCP_OAUTH_STORE_FORMAT_VERSION) {
        return core::Result<McpOAuthStore>::Err(
            "mcp-oauth store \"" + store_key + "\" corrupt: unsupported format version " +
            std::to_string(format_version));
    }
    json parsed;
    try {
        parsed = json::parse(store_json);
    } catch (const std::exception& e) {
        return core::Result<McpOAuthStore>::Err(
            "mcp-oauth store \"" + store_key + "\" corrupt: invalid JSON");
    }
    return parse_mcp_oauth_store(store_key, parsed);
}

// 在已打开的 db 上读取一行。
core::Result<McpOAuthStore> read_from_database(sqlite3* db, const std::string& store_key) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT format_version, store_json FROM mcp_oauth_stores WHERE store_key = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return core::Result<McpOAuthStore>::Err(
            std::string("prepare failed: ") + sqlite3_errmsg(db));
    }
    sqlite3_bind_text(stmt, 1, store_key.c_str(), -1, SQLITE_TRANSIENT);
    bool has_row = false;
    int format_version = 0;
    std::string store_json;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        has_row = true;
        format_version = sqlite3_column_int(stmt, 0);
        const char* j = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (j) store_json = j;
    }
    sqlite3_finalize(stmt);
    return store_from_row(store_key, has_row, format_version, store_json);
}

// 写入一行（对齐 replaceMcpOAuthStore）：写前校验，ON CONFLICT 更新。
core::Result<void> replace_store(sqlite3* db, const std::string& store_key,
                                 const McpOAuthStore& next) {
    // 对齐 parseMcpOAuthStoreJson(storeKey, JSON.stringify(next)) 的写前校验。
    json store_json_value = next.to_json();
    auto validated = parse_mcp_oauth_store(store_key, store_json_value);
    if (validated.is_err()) {
        return core::Result<void>::Err(validated.error);
    }
    std::string store_json = store_json_value.dump();
    int64_t updated_at = now_ms();

    sqlite3_stmt* stmt = nullptr;
    // INSERT OR REPLACE 兼容旧 sqlite（<3.24 无 UPSERT）；store_key 为 PK，语义等价。
    const char* sql =
        "INSERT OR REPLACE INTO mcp_oauth_stores "
        "(store_key, format_version, store_json, updated_at) "
        "VALUES (?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return core::Result<void>::Err(std::string("prepare failed: ") + sqlite3_errmsg(db));
    }
    sqlite3_bind_text(stmt, 1, store_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, MCP_OAUTH_STORE_FORMAT_VERSION);
    sqlite3_bind_text(stmt, 3, store_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, updated_at);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return core::Result<void>::Err(std::string("write failed: ") + sqlite3_errmsg(db));
    }
    return core::Result<void>::Ok();
}

// 打开可写 db（创建目录 + 建表）。
core::Result<sqlite3*> open_writable_db() {
    std::string path = resolve_mcp_oauth_state_db_path();
    ensure_parent_dir(path);
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "unknown";
        if (db) sqlite3_close(db);
        return core::Result<sqlite3*>::Err("failed to open state db: " + err);
    }
    sqlite3_busy_timeout(db, 5000);
    char* errmsg = nullptr;
    if (sqlite3_exec(db, kCreateTableSql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string err = errmsg ? errmsg : "unknown";
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return core::Result<sqlite3*>::Err("failed to ensure table: " + err);
    }
    return core::Result<sqlite3*>::Ok(db);
}

} // namespace

std::string resolve_mcp_oauth_state_db_path() {
    if (!test_db_path_override().empty()) {
        return test_db_path_override();
    }
    // 对齐 {stateDir}/state/openclaw.sqlite
    return agents::resolve_state_dir() + "/state/openclaw.sqlite";
}

void set_mcp_oauth_state_db_path_for_test(const std::string& path) {
    test_db_path_override() = path;
}

core::Result<McpOAuthStore> read_mcp_oauth_store(const std::string& store_key) {
    auto db_res = open_writable_db();
    if (db_res.is_err()) return core::Result<McpOAuthStore>::Err(db_res.error);
    sqlite3* db = db_res.get();
    auto result = read_from_database(db, store_key);
    sqlite3_close(db);
    return result;
}

core::Result<McpOAuthStore> read_mcp_oauth_store_read_only(const std::string& store_key) {
    std::string path = resolve_mcp_oauth_state_db_path();
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        // db 不存在 → {}
        return core::Result<McpOAuthStore>::Ok(McpOAuthStore{});
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return core::Result<McpOAuthStore>::Ok(McpOAuthStore{});
    }
    core::Result<McpOAuthStore> result = core::Result<McpOAuthStore>::Ok(McpOAuthStore{});
    if (table_exists(db, "mcp_oauth_stores")) {
        result = read_from_database(db, store_key);
    }
    sqlite3_close(db);
    return result;
}

core::Result<McpOAuthStore> update_mcp_oauth_store(
    const std::string& store_key,
    const std::function<McpOAuthStore(const McpOAuthStore&)>& update) {
    // 进程内串行化（对齐 lease）；BEGIN IMMEDIATE 提供跨进程写锁。
    std::lock_guard<std::mutex> lock(write_mutex());
    auto db_res = open_writable_db();
    if (db_res.is_err()) return core::Result<McpOAuthStore>::Err(db_res.error);
    sqlite3* db = db_res.get();

    char* errmsg = nullptr;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string err = errmsg ? errmsg : "unknown";
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return core::Result<McpOAuthStore>::Err("begin transaction failed: " + err);
    }

    auto current = read_from_database(db, store_key);
    if (current.is_err()) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        return current;
    }

    McpOAuthStore next = update(current.get());
    auto write_res = replace_store(db, store_key, next);
    if (write_res.is_err()) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        return core::Result<McpOAuthStore>::Err(write_res.error);
    }

    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string err = errmsg ? errmsg : "unknown";
        sqlite3_free(errmsg);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        return core::Result<McpOAuthStore>::Err("commit failed: " + err);
    }
    sqlite3_close(db);
    return core::Result<McpOAuthStore>::Ok(std::move(next));
}

core::Result<void> clear_mcp_oauth_store(const std::string& store_key) {
    // 对齐 clearMcpOAuthStore: 写 {credentialState:"cleared"}。
    auto res = update_mcp_oauth_store(store_key, [](const McpOAuthStore&) {
        McpOAuthStore cleared;
        cleared.credential_state = "cleared";
        return cleared;
    });
    if (res.is_err()) return core::Result<void>::Err(res.error);
    return core::Result<void>::Ok();
}

core::Result<McpOAuthCredentialsStatus> read_mcp_oauth_credentials_status(
    const std::string& store_key) {
    auto store = read_mcp_oauth_store_read_only(store_key);
    if (store.is_err()) {
        return core::Result<McpOAuthCredentialsStatus>::Err(store.error);
    }
    return core::Result<McpOAuthCredentialsStatus>::Ok(
        mcp_oauth_credentials_status(store.get()));
}

} // namespace mcp
} // namespace hunterclaw
