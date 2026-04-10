#include "hunterclaw/memory/manager_source_state.h"
#include "hunterclaw/memory/sqlite_storage.h"

namespace hunterclaw {
namespace memory {

// ============================================================================
// loadMemorySourceFileState (对齐 manager-source-state.ts:19-34)
// ============================================================================
SourceFileState load_memory_source_file_state(
    SQLiteVectorStorage& db,
    const std::string& source
) {
    SourceFileState state;
    auto stmt = db.prepare_statement(MEMORY_SOURCE_FILE_STATE_SQL);
    if (!stmt.is_ok()) return state;

    sqlite3_bind_text(stmt.get(), 1, source.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        SourceFileStateRow row;
        row.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        row.hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        state.hashes[row.path] = row.hash;
        state.rows.push_back(std::move(row));
    }
    return state;
}

// ============================================================================
// resolveMemorySourceExistingHash (对齐 manager-source-state.ts:36-50)
// ============================================================================
std::optional<std::string> resolve_memory_source_existing_hash(
    SQLiteVectorStorage& db,
    const std::string& source,
    const std::string& path,
    const std::optional<std::map<std::string, std::string>>& existing_hashes
) {
    // 对齐: if (params.existingHashes) return params.existingHashes.get(params.path);
    if (existing_hashes) {
        auto it = existing_hashes->find(path);
        if (it != existing_hashes->end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // 对齐: db.prepare(MEMORY_SOURCE_FILE_HASH_SQL).get(path, source)?.hash
    auto stmt = db.prepare_statement(MEMORY_SOURCE_FILE_HASH_SQL);
    if (!stmt.is_ok()) return std::nullopt;

    sqlite3_bind_text(stmt.get(), 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, source.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)));
    }
    return std::nullopt;
}

} // namespace memory
} // namespace hunterclaw
