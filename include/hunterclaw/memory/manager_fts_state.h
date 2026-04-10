#pragma once

#include <string>
#include <optional>

// 严格对齐 OpenClaw src/memory/manager-fts-state.ts (22行)
// FTS 行删除的独立函数

namespace hunterclaw {
namespace memory {

// Forward declarations
class SQLiteVectorStorage;

// ============================================================================
// deleteMemoryFtsRows (对齐 manager-fts-state.ts:4-21)
// ============================================================================

struct DeleteFtsRowsParams {
    SQLiteVectorStorage* db = nullptr;
    std::string table_name = "chunks_fts";
    std::string path;
    std::string source;
    std::optional<std::string> current_model;
};

void delete_memory_fts_rows(const DeleteFtsRowsParams& params);

} // namespace memory
} // namespace hunterclaw
