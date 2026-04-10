#pragma once

// ============================================================================
// Manager Targeted Sync — 严格对齐 OpenClaw manager-targeted-sync.ts (92 行)
// ============================================================================

#include <string>
#include <vector>
#include <set>
#include <functional>

namespace hunterclaw {
namespace memory {

/**
 * Clear synced session files from dirty set.
 * (对齐 manager-targeted-sync.ts:11-23 clearMemorySyncedSessionFiles)
 */
bool clear_memory_synced_session_files(
    std::set<std::string>& sessions_dirty_files,
    const std::vector<std::string>* target_session_files = nullptr
);

/**
 * Run targeted session sync with fallback provider activation.
 * (对齐 manager-targeted-sync.ts:25-92 runMemoryTargetedSessionSync)
 */
struct TargetedSessionSyncResult {
    bool handled = false;
    bool sessions_dirty = false;
};

struct TargetedSessionSyncParams {
    bool has_session_source = false;
    const std::set<std::string>* target_session_files = nullptr;
    std::string reason;
    std::set<std::string>& sessions_dirty_files;
    std::function<void(bool needs_full_reindex, const std::vector<std::string>& files)> sync_session_files;
    std::function<bool(const std::string& message)> should_fallback_on_error;
    std::function<bool(const std::string& reason)> activate_fallback_provider;
    std::function<void(const std::string& reason, bool force)> run_safe_reindex;
    bool use_unsafe_reindex = false;
};

TargetedSessionSyncResult run_memory_targeted_session_sync(TargetedSessionSyncParams& params);

}  // namespace memory
}  // namespace hunterclaw
