#pragma once

#include <string>
#include <vector>

// 严格对齐 OpenClaw src/memory/manager-vector-write.ts (26行)

namespace hunterclaw {
namespace memory {

// Forward declarations
class SQLiteVectorStorage;

// ============================================================================
// replaceMemoryVectorRow (对齐 manager-vector-write.ts:12-25)
// ============================================================================
struct ReplaceVectorRowParams {
    SQLiteVectorStorage* db = nullptr;
    std::string id;
    std::vector<float> embedding;
    std::string table_name = "chunks_vec";
};

void replace_memory_vector_row(const ReplaceVectorRowParams& params);

} // namespace memory
} // namespace hunterclaw
