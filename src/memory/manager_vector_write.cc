#include "hunterclaw/memory/manager_vector_write.h"
#include "hunterclaw/memory/sqlite_storage.h"

namespace hunterclaw {
namespace memory {

// ============================================================================
// replaceMemoryVectorRow (对齐 manager-vector-write.ts:12-25)
// ============================================================================
void replace_memory_vector_row(const ReplaceVectorRowParams& params) {
    if (!params.db || params.embedding.empty()) return;

    // 对齐: try { db.prepare("DELETE FROM chunks_vec WHERE id = ?").run(id); } catch {}
    {
        std::string del_sql = "DELETE FROM " + params.table_name + " WHERE id = ?";
        auto del_stmt = params.db->prepare_statement(del_sql);
        if (del_stmt.is_ok()) {
            sqlite3_bind_text(del_stmt.get(), 1, params.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del_stmt.get());
        }
    }

    // 对齐: db.prepare("INSERT INTO chunks_vec (id, embedding) VALUES (?, ?)").run(id, vectorToBlob)
    std::string ins_sql = "INSERT INTO " + params.table_name + " (id, embedding) VALUES (?, ?)";
    auto ins_stmt = params.db->prepare_statement(ins_sql);
    if (ins_stmt.is_ok()) {
        sqlite3_bind_text(ins_stmt.get(), 1, params.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(ins_stmt.get(), 2,
            params.embedding.data(),
            static_cast<int>(params.embedding.size() * sizeof(float)),
            SQLITE_TRANSIENT);
        sqlite3_step(ins_stmt.get());
    }
}

} // namespace memory
} // namespace hunterclaw
