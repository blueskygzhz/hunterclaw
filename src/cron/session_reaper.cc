// 严格对齐 OpenClaw: src/cron/session-reaper.ts (150 行)
// Cron session reaper — prunes completed isolated cron run sessions

#include "hunterclaw/cron/session_reaper.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace hunterclaw {
namespace cron {

namespace {

// 对齐 OpenClaw: lastSweepAtMsByStore (per-store throttle state)
std::mutex g_throttle_mutex;
std::unordered_map<std::string, int64_t> g_last_sweep_at_ms_by_store;

// 对齐 OpenClaw: isCronRunSessionKey pattern
// Pattern: `...:cron:<jobId>:run:<uuid>`
const std::regex CRON_RUN_SESSION_KEY_RE(R"(:cron:[^:]+:run:[^:]+$)");

}  // namespace

// ============================================================================
// resolveRetentionMs (对齐 OpenClaw resolveRetentionMs)
// ============================================================================

std::optional<int64_t> resolve_retention_ms(const SessionRetentionConfig& config) {
  if (!config.enabled) {
    return std::nullopt;  // pruning disabled
  }

  if (config.retention_ms > 0) {
    return config.retention_ms;
  }

  // Parse raw retention string (e.g. "24h", "48h", "7d")
  if (!config.raw_retention.empty()) {
    const std::string& raw = config.raw_retention;
    try {
      size_t pos = 0;
      double value = std::stod(raw, &pos);
      if (pos < raw.size()) {
        char unit = raw[pos];
        switch (unit) {
          case 's': return static_cast<int64_t>(value * 1000);
          case 'm': return static_cast<int64_t>(value * 60 * 1000);
          case 'h': return static_cast<int64_t>(value * 3600 * 1000);
          case 'd': return static_cast<int64_t>(value * 86400 * 1000);
          default: break;
        }
      }
      // Default unit: hours (对齐 OpenClaw parseDurationMs defaultUnit: "h")
      return static_cast<int64_t>(value * 3600 * 1000);
    } catch (...) {
      return DEFAULT_SESSION_RETENTION_MS;
    }
  }

  return DEFAULT_SESSION_RETENTION_MS;
}

// ============================================================================
// isCronRunSessionKey (对齐 OpenClaw isCronRunSessionKey)
// ============================================================================

bool is_cron_run_session_key(const std::string& key) {
  return std::regex_search(key, CRON_RUN_SESSION_KEY_RE);
}

// ============================================================================
// sweepCronRunSessions (对齐 OpenClaw sweepCronRunSessions)
// ============================================================================

ReaperResult sweep_cron_run_sessions(
    const std::string& session_store_path,
    const SessionRetentionConfig& retention_config,
    int64_t now_ms,
    const ReaperLogger& logger,
    const SessionStoreAdapter& store_adapter,
    bool force) {

  // Throttle: don't sweep more often than every 5 minutes (对齐 OpenClaw)
  {
    std::lock_guard<std::mutex> lock(g_throttle_mutex);
    auto it = g_last_sweep_at_ms_by_store.find(session_store_path);
    int64_t last_sweep_at_ms = (it != g_last_sweep_at_ms_by_store.end()) ? it->second : 0;

    if (!force && now_ms - last_sweep_at_ms < MIN_SWEEP_INTERVAL_MS) {
      return {false, 0};
    }
  }

  // Resolve retention
  auto retention_ms_opt = resolve_retention_ms(retention_config);
  if (!retention_ms_opt.has_value()) {
    std::lock_guard<std::mutex> lock(g_throttle_mutex);
    g_last_sweep_at_ms_by_store[session_store_path] = now_ms;
    return {false, 0};
  }

  int64_t retention_ms = *retention_ms_opt;
  int pruned = 0;
  std::vector<std::string> keys_to_remove;
  std::vector<std::pair<std::string, std::string>> pruned_sessions;  // {sessionId, sessionFile}

  try {
    // Load session store
    auto store = store_adapter.load(session_store_path);
    int64_t cutoff = now_ms - retention_ms;

    for (const auto& [key, entry] : store) {
      if (!is_cron_run_session_key(key)) {
        continue;
      }

      int64_t updated_at = entry.updated_at;
      if (updated_at < cutoff) {
        keys_to_remove.push_back(key);
        if (!entry.session_id.empty()) {
          pruned_sessions.push_back({entry.session_id, entry.session_file});
        }
        pruned++;
      }
    }

    // Remove expired entries
    if (!keys_to_remove.empty()) {
      store_adapter.remove_entries(session_store_path, keys_to_remove);
    }
  } catch (const std::exception& e) {
    if (logger.warn) {
      logger.warn("cron-reaper: failed to sweep session store: " + std::string(e.what()));
    }
    return {false, 0};
  }

  // Update throttle timestamp
  {
    std::lock_guard<std::mutex> lock(g_throttle_mutex);
    g_last_sweep_at_ms_by_store[session_store_path] = now_ms;
  }

  // Archive removed session transcripts (best-effort)
  // 对齐 OpenClaw: archiveRemovedSessionTranscripts + cleanupArchivedSessionTranscripts
  if (!pruned_sessions.empty() && store_adapter.archive_transcripts) {
    try {
      store_adapter.archive_transcripts(pruned_sessions, retention_ms, now_ms);
    } catch (const std::exception& e) {
      if (logger.warn) {
        logger.warn("cron-reaper: transcript cleanup failed: " + std::string(e.what()));
      }
    }
  }

  if (pruned > 0 && logger.info) {
    logger.info("cron-reaper: pruned " + std::to_string(pruned) +
                " expired cron run session(s), retentionMs=" + std::to_string(retention_ms));
  }

  return {true, pruned};
}

// ============================================================================
// resetReaperThrottle (对齐 OpenClaw resetReaperThrottle)
// ============================================================================

void reset_reaper_throttle() {
  std::lock_guard<std::mutex> lock(g_throttle_mutex);
  g_last_sweep_at_ms_by_store.clear();
}

}  // namespace cron
}  // namespace hunterclaw
