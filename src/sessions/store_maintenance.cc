// 严格对齐 OpenClaw config/sessions/store-maintenance.ts (334行)
#include "hunterclaw/sessions/store_maintenance.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace sessions {

// 对齐 store-maintenance.ts:39-100
ResolvedSessionMaintenanceConfig resolve_session_maintenance_config(
    const std::string& config_json
) {
  ResolvedSessionMaintenanceConfig config;
  try {
    auto cfg = json::parse(config_json);
    if (cfg.contains("session") && cfg["session"].is_object()) {
      auto& session = cfg["session"];
      if (session.contains("maintenance") && session["maintenance"].is_object()) {
        auto& m = session["maintenance"];
        if (m.contains("mode") && m["mode"].is_string())
          config.mode = m["mode"].get<std::string>();
        if (m.contains("pruneAfter") && m["pruneAfter"].is_string()) {
          // Parse duration — simplified: "30d" → 30 * 24 * 60 * 60 * 1000
          std::string raw = m["pruneAfter"].get<std::string>();
          if (raw.back() == 'd') {
            int days = std::atoi(raw.substr(0, raw.size()-1).c_str());
            if (days > 0) config.prune_after_ms = static_cast<int64_t>(days) * 24 * 60 * 60 * 1000;
          }
        }
        if (m.contains("maxEntries") && m["maxEntries"].is_number())
          config.max_entries = m["maxEntries"].get<int>();
        if (m.contains("maxDiskBytes") && m["maxDiskBytes"].is_number())
          config.max_disk_bytes = m["maxDiskBytes"].get<int64_t>();
      }
    }
  } catch (...) {}
  
  if (config.max_disk_bytes && !config.high_water_bytes) {
    config.high_water_bytes = static_cast<int64_t>(*config.max_disk_bytes * 0.8);
  }
  return config;
}

// 对齐 store-maintenance.ts:137-213
PruneResult prune_stale_entries(
    std::unordered_map<std::string, SessionEntry>& store,
    int64_t prune_after_ms,
    const std::optional<std::string>& active_session_key,
    int64_t now_ms
) {
  if (now_ms == 0) {
    now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()
    ).count();
  }
  
  PruneResult result;
  std::vector<std::string> to_remove;
  
  for (const auto& [key, entry] : store) {
    if (active_session_key && key == *active_session_key) continue;
    int64_t age = now_ms - entry.updated_at;
    if (age > prune_after_ms) {
      to_remove.push_back(key);
    }
  }
  
  for (const auto& key : to_remove) {
    store.erase(key);
  }
  
  result.removed = static_cast<int>(to_remove.size());
  result.removed_keys = to_remove;
  return result;
}

// 对齐 store-maintenance.ts:215-275
PruneResult cap_entry_count(
    std::unordered_map<std::string, SessionEntry>& store,
    int max_entries,
    const std::optional<std::string>& active_session_key
) {
  PruneResult result;
  if (static_cast<int>(store.size()) <= max_entries) return result;
  
  // Sort by updated_at ascending (oldest first)
  std::vector<std::pair<std::string, int64_t>> entries;
  for (const auto& [key, entry] : store) {
    if (active_session_key && key == *active_session_key) continue;
    entries.push_back({key, entry.updated_at});
  }
  
  std::sort(entries.begin(), entries.end(),
    [](const auto& a, const auto& b) { return a.second < b.second; });
  
  int to_remove_count = static_cast<int>(store.size()) - max_entries;
  for (int i = 0; i < to_remove_count && i < static_cast<int>(entries.size()); ++i) {
    store.erase(entries[i].first);
    result.removed_keys.push_back(entries[i].first);
  }
  
  result.removed = static_cast<int>(result.removed_keys.size());
  return result;
}

// 对齐 store-maintenance.ts:277-334
bool rotate_session_store(const std::string& store_path, int64_t rotate_bytes) {
  try {
    if (!fs::exists(store_path)) return false;
    auto size = fs::file_size(store_path);
    if (static_cast<int64_t>(size) < rotate_bytes) return false;
    
    // Rename to .bak.<timestamp>
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%SZ", gmtime(&time_t_now));
    std::string bak_path = store_path + ".bak." + buf;
    fs::rename(store_path, bak_path);
    
    // Clean up old backups — keep only 3 most recent
    fs::path dir = fs::path(store_path).parent_path();
    std::string base = fs::path(store_path).filename().string();
    std::vector<fs::path> backups;
    for (const auto& entry : fs::directory_iterator(dir)) {
      std::string name = entry.path().filename().string();
      if (name.find(base + ".bak.") == 0) {
        backups.push_back(entry.path());
      }
    }
    std::sort(backups.begin(), backups.end());
    while (backups.size() > 3) {
      fs::remove(backups.front());
      backups.erase(backups.begin());
    }
    
    return true;
  } catch (...) { return false; }
}

std::optional<SessionMaintenanceWarning> get_active_session_maintenance_warning(
    const std::unordered_map<std::string, SessionEntry>& store,
    const ResolvedSessionMaintenanceConfig& config,
    const std::optional<std::string>& active_session_key
) {
  if (config.mode == "off") return std::nullopt;
  
  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()
  ).count();
  
  bool would_prune = false;
  for (const auto& [key, entry] : store) {
    if (active_session_key && key == *active_session_key) continue;
    if ((now - entry.updated_at) > config.prune_after_ms) { would_prune = true; break; }
  }
  
  bool would_cap = static_cast<int>(store.size()) > config.max_entries;
  
  if (!would_prune && !would_cap) return std::nullopt;
  
  SessionMaintenanceWarning warning;
  warning.active_session_key = active_session_key.value_or("");
  if (active_session_key) {
    auto it = store.find(*active_session_key);
    if (it != store.end()) warning.active_updated_at = it->second.updated_at;
  }
  warning.total_entries = static_cast<int>(store.size());
  warning.prune_after_ms = config.prune_after_ms;
  warning.max_entries = config.max_entries;
  warning.would_prune = would_prune;
  warning.would_cap = would_cap;
  return warning;
}

}  // namespace sessions
}  // namespace hunterclaw
