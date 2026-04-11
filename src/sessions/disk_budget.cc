// 严格对齐 OpenClaw config/sessions/disk-budget.ts (380行)
#include "hunterclaw/sessions/disk_budget.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace sessions {

int64_t calculate_session_disk_usage(const std::string& sessions_dir) {
  int64_t total = 0;
  try {
    for (const auto& entry : fs::recursive_directory_iterator(sessions_dir)) {
      if (entry.is_regular_file()) {
        total += static_cast<int64_t>(entry.file_size());
      }
    }
  } catch (...) {}
  return total;
}

// 对齐 disk-budget.ts:94-380
SessionDiskBudgetSweepResult enforce_session_disk_budget(
    const std::string& sessions_dir,
    std::unordered_map<std::string, SessionEntry>& store,
    const SessionDiskBudgetConfig& config,
    const std::optional<std::string>& active_session_key,
    bool dry_run
) {
  int64_t max_bytes = config.max_disk_bytes.value_or(0);
  int64_t high_water = config.high_water_bytes.value_or(static_cast<int64_t>(max_bytes * 0.8));
  
  int64_t total_before = calculate_session_disk_usage(sessions_dir);
  
  SessionDiskBudgetSweepResult result;
  result.total_bytes_before = total_before;
  result.total_bytes_after = total_before;
  result.removed_files = 0;
  result.removed_entries = 0;
  result.freed_bytes = 0;
  result.max_bytes = max_bytes;
  result.high_water_bytes = high_water;
  result.over_budget = (max_bytes > 0 && total_before > max_bytes);
  
  if (max_bytes <= 0 || total_before <= high_water) return result;
  
  // Sort entries by updated_at ascending (oldest first)
  std::vector<std::pair<std::string, int64_t>> entries;
  for (const auto& [key, entry] : store) {
    if (active_session_key && key == *active_session_key) continue;
    entries.push_back({key, entry.updated_at});
  }
  std::sort(entries.begin(), entries.end(),
    [](const auto& a, const auto& b) { return a.second < b.second; });
  
  int64_t total = total_before;
  
  for (const auto& [key, _] : entries) {
    if (total <= high_water) break;
    
    auto it = store.find(key);
    if (it == store.end()) continue;
    
    // Try to remove the transcript file (derive path from session_id)
    if (!it->second.session_id.empty() && !dry_run) {
      try {
        // Look for transcript file matching session_id in sessions_dir
        std::string pattern = it->second.session_id + ".jsonl";
        for (const auto& f : fs::directory_iterator(sessions_dir)) {
          if (f.path().filename().string().find(pattern) != std::string::npos) {
            auto size = f.file_size();
            fs::remove(f.path());
            total -= static_cast<int64_t>(size);
            result.removed_files++;
            result.freed_bytes += static_cast<int64_t>(size);
            break;
          }
        }
      } catch (...) {}
    }
    
    if (!dry_run) {
      store.erase(it);
    }
    result.removed_entries++;
  }
  
  result.total_bytes_after = total;
  return result;
}

}  // namespace sessions
}  // namespace hunterclaw
