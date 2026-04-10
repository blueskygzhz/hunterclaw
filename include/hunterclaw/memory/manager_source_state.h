#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>

// 严格对齐 OpenClaw src/memory/manager-source-state.ts (51行)

namespace hunterclaw {
namespace memory {

// Forward declarations
class SQLiteVectorStorage;

// ============================================================================
// SQL constants (对齐 manager-source-state.ts:16-17)
// ============================================================================
constexpr const char* MEMORY_SOURCE_FILE_STATE_SQL =
    "SELECT path, hash FROM files WHERE source = ?";
constexpr const char* MEMORY_SOURCE_FILE_HASH_SQL =
    "SELECT hash FROM files WHERE path = ? AND source = ?";

// ============================================================================
// MemorySourceFileState (对齐 manager-source-state.ts:4-7 + 19-34)
// ============================================================================
struct SourceFileStateRow {
    std::string path;
    std::string hash;
};

struct SourceFileState {
    std::vector<SourceFileStateRow> rows;
    std::map<std::string, std::string> hashes;
};

// ============================================================================
// loadMemorySourceFileState (对齐 manager-source-state.ts:19-34)
// ============================================================================
SourceFileState load_memory_source_file_state(
    SQLiteVectorStorage& db,
    const std::string& source
);

// ============================================================================
// resolveMemorySourceExistingHash (对齐 manager-source-state.ts:36-50)
// ============================================================================
std::optional<std::string> resolve_memory_source_existing_hash(
    SQLiteVectorStorage& db,
    const std::string& source,
    const std::string& path,
    const std::optional<std::map<std::string, std::string>>& existing_hashes = std::nullopt
);

} // namespace memory
} // namespace hunterclaw
