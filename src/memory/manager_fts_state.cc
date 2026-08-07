#include "hunterclaw/memory/manager_fts_state.h"
#include "hunterclaw/memory/sqlite_storage.h"

namespace hunterclaw {
namespace memory {

// ============================================================================
// deleteMemoryFtsRows (对齐 manager-fts-state.ts:4-21)
// ============================================================================
void delete_memory_fts_rows(const DeleteFtsRowsParams& params) {
    if (!params.db) return;

    if (params.current_model) {
        // DELETE FROM memory_index_chunks_fts WHERE path = ? AND source = ? AND model = ?
        const std::string sql = "DELETE FROM " + params.table_name +
            " WHERE path = ? AND source = ? AND model = ?";
        auto stmt = params.db->prepare_statement(sql);
        if (stmt.is_ok()) {
            sqlite3_bind_text(stmt.get(), 1, params.path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 2, params.source.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 3, params.current_model->c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt.get());
        }
        return;
    }

    // DELETE FROM memory_index_chunks_fts WHERE path = ? AND source = ?
    const std::string sql = "DELETE FROM " + params.table_name +
        " WHERE path = ? AND source = ?";
    auto stmt = params.db->prepare_statement(sql);
    if (stmt.is_ok()) {
        sqlite3_bind_text(stmt.get(), 1, params.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt.get(), 2, params.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt.get());
    }
}

} // namespace memory
} // namespace hunterclaw
