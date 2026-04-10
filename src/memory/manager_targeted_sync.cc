#include "hunterclaw/memory/manager_targeted_sync.h"
#include <stdexcept>

// 严格对齐 OpenClaw memory/manager-targeted-sync.ts (92 行)

namespace hunterclaw {
namespace memory {

// 对齐 manager-targeted-sync.ts:11-23
bool clear_memory_synced_session_files(
    std::set<std::string>& sessions_dirty_files,
    const std::vector<std::string>* target_session_files
) {
    if (!target_session_files) {
        sessions_dirty_files.clear();
    } else {
        for (const auto& f : *target_session_files) {
            sessions_dirty_files.erase(f);
        }
    }
    return !sessions_dirty_files.empty();
}

// 对齐 manager-targeted-sync.ts:25-92
TargetedSessionSyncResult run_memory_targeted_session_sync(TargetedSessionSyncParams& params) {
    if (!params.has_session_source || !params.target_session_files) {
        return {false, !params.sessions_dirty_files.empty()};
    }

    std::vector<std::string> target_files(
        params.target_session_files->begin(),
        params.target_session_files->end());

    try {
        params.sync_session_files(false, target_files);
        bool dirty = clear_memory_synced_session_files(
            params.sessions_dirty_files, &target_files);
        return {true, dirty};
    } catch (const std::exception& e) {
        std::string reason = e.what();
        bool activated = params.should_fallback_on_error(reason) &&
                         params.activate_fallback_provider(reason);
        if (!activated) throw;
        params.run_safe_reindex(params.reason, true);
        return {true, !params.sessions_dirty_files.empty()};
    }
}

}  // namespace memory
}  // namespace hunterclaw
