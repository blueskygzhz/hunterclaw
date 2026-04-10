#pragma once

#include <string>
#include <vector>
#include <set>
#include <optional>

// 严格对齐 OpenClaw src/memory/manager-session-reindex.ts (30行)

namespace hunterclaw {
namespace memory {

// ============================================================================
// shouldSyncSessionsForReindex (对齐 manager-session-reindex.ts:1-29)
// ============================================================================

struct ShouldSyncSessionsParams {
    bool has_session_source = false;
    bool sessions_dirty = false;
    int dirty_session_file_count = 0;
    std::optional<std::string> reason;
    std::optional<bool> force;
    std::optional<std::vector<std::string>> session_files;
    bool needs_full_reindex = false;
};

bool should_sync_sessions_for_reindex(const ShouldSyncSessionsParams& params);

} // namespace memory
} // namespace hunterclaw
