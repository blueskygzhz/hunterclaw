#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <optional>
#include <functional>

// 严格对齐 OpenClaw src/memory/manager-session-sync-state.ts (30行)

namespace hunterclaw {
namespace memory {

// ============================================================================
// MemorySourceFileStateRow (对齐 manager-source-state.ts:4-7)
// ============================================================================
struct MemorySourceFileStateRow {
    std::string path;
    std::string hash;
};

// ============================================================================
// SessionSyncPlan (对齐 manager-session-sync-state.ts:10-15)
// ============================================================================
struct SessionSyncPlan {
    std::optional<std::set<std::string>> active_paths;  // null for targeted
    std::optional<std::vector<MemorySourceFileStateRow>> existing_rows;
    std::optional<std::map<std::string, std::string>> existing_hashes;
    bool index_all = false;
};

// ============================================================================
// resolveMemorySessionSyncPlan (对齐 manager-session-sync-state.ts:3-29)
// ============================================================================
struct ResolveSessionSyncPlanParams {
    bool needs_full_reindex = false;
    std::vector<std::string> files;
    std::optional<std::set<std::string>> target_session_files;
    std::set<std::string> sessions_dirty_files;
    std::optional<std::vector<MemorySourceFileStateRow>> existing_rows;
    std::function<std::string(const std::string&)> session_path_for_file;
};

SessionSyncPlan resolve_memory_session_sync_plan(const ResolveSessionSyncPlanParams& params);

} // namespace memory
} // namespace hunterclaw
