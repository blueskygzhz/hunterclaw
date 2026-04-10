#include "hunterclaw/memory/manager_session_reindex.h"

namespace hunterclaw {
namespace memory {

// ============================================================================
// shouldSyncSessionsForReindex (对齐 manager-session-reindex.ts:1-29)
// ============================================================================
bool should_sync_sessions_for_reindex(const ShouldSyncSessionsParams& params) {
    if (!params.has_session_source) {
        return false;
    }
    // 对齐: if (params.sync?.sessionFiles?.some(f => f.trim().length > 0))
    if (params.session_files) {
        for (const auto& f : *params.session_files) {
            std::string trimmed = f;
            // trim
            size_t start = trimmed.find_first_not_of(" \t\n\r");
            if (start != std::string::npos && trimmed.size() - start > 0) {
                return true;
            }
        }
    }
    if (params.force && *params.force) {
        return true;
    }
    if (params.needs_full_reindex) {
        return true;
    }
    // 对齐: if (reason === "session-start" || reason === "watch") return false;
    if (params.reason) {
        if (*params.reason == "session-start" || *params.reason == "watch") {
            return false;
        }
    }
    return params.sessions_dirty && params.dirty_session_file_count > 0;
}

} // namespace memory
} // namespace hunterclaw
