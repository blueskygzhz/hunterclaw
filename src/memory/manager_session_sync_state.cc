#include "hunterclaw/memory/manager_session_sync_state.h"

namespace hunterclaw {
namespace memory {

// ============================================================================
// resolveMemorySessionSyncPlan (对齐 manager-session-sync-state.ts:3-29)
// ============================================================================
SessionSyncPlan resolve_memory_session_sync_plan(const ResolveSessionSyncPlanParams& params) {
    SessionSyncPlan plan;

    // 对齐: const activePaths = params.targetSessionFiles ? null : new Set(...)
    if (params.target_session_files) {
        plan.active_paths = std::nullopt;
    } else {
        std::set<std::string> paths;
        for (const auto& file : params.files) {
            if (params.session_path_for_file) {
                paths.insert(params.session_path_for_file(file));
            }
        }
        plan.active_paths = paths;
    }

    // 对齐: const existingRows = activePaths === null ? null : (params.existingRows ?? []);
    if (!plan.active_paths) {
        plan.existing_rows = std::nullopt;
        plan.existing_hashes = std::nullopt;
    } else {
        plan.existing_rows = params.existing_rows.value_or(std::vector<MemorySourceFileStateRow>{});
        std::map<std::string, std::string> hashes;
        if (plan.existing_rows) {
            for (const auto& row : *plan.existing_rows) {
                hashes[row.path] = row.hash;
            }
        }
        plan.existing_hashes = hashes;
    }

    // 对齐: indexAll = needsFullReindex || Boolean(targetSessionFiles) || sessionsDirtyFiles.size === 0
    plan.index_all = params.needs_full_reindex ||
                     params.target_session_files.has_value() ||
                     params.sessions_dirty_files.empty();

    return plan;
}

} // namespace memory
} // namespace hunterclaw
