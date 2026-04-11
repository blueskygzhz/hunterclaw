#pragma once

// 严格对齐 OpenClaw config/sessions/store-maintenance.ts (334行)
// Session store 维护: 过期清理 + 条目上限 + 文件轮转

#include "hunterclaw/sessions/session_manager.h"
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace sessions {

// ============================================================================
// Config (对齐 store-maintenance.ts:29-37)
// ============================================================================

struct ResolvedSessionMaintenanceConfig {
  std::string mode = "warn";                   // "warn" | "enforce" | "off"
  int64_t prune_after_ms = 30LL * 24 * 60 * 60 * 1000;  // 30 days
  int max_entries = 500;
  int64_t rotate_bytes = 10485760;             // 10 MB
  std::optional<int64_t> reset_archive_retention_ms;
  std::optional<int64_t> max_disk_bytes;
  std::optional<int64_t> high_water_bytes;
};

/// Resolve maintenance config from JSON config.
ResolvedSessionMaintenanceConfig resolve_session_maintenance_config(
    const std::string& config_json
);

// ============================================================================
// Prune (对齐 store-maintenance.ts:137-213)
// ============================================================================

struct PruneResult {
  int removed = 0;
  std::vector<std::string> removed_keys;
};

/// Prune stale session entries older than prune_after_ms.
PruneResult prune_stale_entries(
    std::unordered_map<std::string, SessionEntry>& store,
    int64_t prune_after_ms,
    const std::optional<std::string>& active_session_key = std::nullopt,
    int64_t now_ms = 0
);

/// Cap the number of session entries to max_entries.
PruneResult cap_entry_count(
    std::unordered_map<std::string, SessionEntry>& store,
    int max_entries,
    const std::optional<std::string>& active_session_key = std::nullopt
);

// ============================================================================
// Rotate (对齐 store-maintenance.ts:277-334)
// ============================================================================

/// Rotate the session store file if it exceeds rotate_bytes.
bool rotate_session_store(
    const std::string& store_path,
    int64_t rotate_bytes
);

// ============================================================================
// Warning (对齐 store-maintenance.ts:101-135)
// ============================================================================

struct SessionMaintenanceWarning {
  std::string active_session_key;
  std::optional<int64_t> active_updated_at;
  int total_entries;
  int64_t prune_after_ms;
  int max_entries;
  bool would_prune;
  bool would_cap;
};

/// Check if maintenance is needed and return a warning if so.
std::optional<SessionMaintenanceWarning> get_active_session_maintenance_warning(
    const std::unordered_map<std::string, SessionEntry>& store,
    const ResolvedSessionMaintenanceConfig& config,
    const std::optional<std::string>& active_session_key = std::nullopt
);

}  // namespace sessions
}  // namespace hunterclaw
