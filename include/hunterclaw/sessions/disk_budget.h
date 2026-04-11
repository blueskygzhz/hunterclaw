#pragma once

// 严格对齐 OpenClaw config/sessions/disk-budget.ts (380行)
// Session 磁盘预算管理: 文件大小统计 + 预算执行 + 清理

#include "hunterclaw/sessions/session_manager.h"
#include <optional>
#include <string>
#include <unordered_map>

namespace hunterclaw {
namespace sessions {

// ============================================================================
// Types (对齐 disk-budget.ts:11-25)
// ============================================================================

struct SessionDiskBudgetConfig {
  std::optional<int64_t> max_disk_bytes;
  std::optional<int64_t> high_water_bytes;
};

struct SessionDiskBudgetSweepResult {
  int64_t total_bytes_before;
  int64_t total_bytes_after;
  int removed_files;
  int removed_entries;
  int64_t freed_bytes;
  int64_t max_bytes;
  int64_t high_water_bytes;
  bool over_budget;
};

// ============================================================================
// Functions (对齐 disk-budget.ts:94-380)
// ============================================================================

/// Enforce session disk budget by removing old transcripts.
SessionDiskBudgetSweepResult enforce_session_disk_budget(
    const std::string& sessions_dir,
    std::unordered_map<std::string, SessionEntry>& store,
    const SessionDiskBudgetConfig& config,
    const std::optional<std::string>& active_session_key = std::nullopt,
    bool dry_run = false
);

/// Calculate total disk usage for session transcripts.
int64_t calculate_session_disk_usage(const std::string& sessions_dir);

}  // namespace sessions
}  // namespace hunterclaw
