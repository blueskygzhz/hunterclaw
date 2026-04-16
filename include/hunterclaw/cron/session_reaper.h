#pragma once

// 严格对齐 OpenClaw: src/cron/session-reaper.ts
// Cron session reaper — prunes completed isolated cron run sessions
// from the session store after a configurable retention period.
//
// Pattern: sessions keyed as `...:cron:<jobId>:run:<uuid>` are ephemeral
// run records. The base session (`...:cron:<jobId>`) is kept as-is.

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace hunterclaw {
namespace cron {

// 对齐 OpenClaw: DEFAULT_RETENTION_MS = 24 * 3_600_000 (24 hours)
constexpr int64_t DEFAULT_SESSION_RETENTION_MS = 24 * 3600 * 1000;

// 对齐 OpenClaw: MIN_SWEEP_INTERVAL_MS = 5 * 60_000 (5 minutes)
constexpr int64_t MIN_SWEEP_INTERVAL_MS = 5 * 60 * 1000;

/**
 * Reaper 配置 (对齐 OpenClaw CronConfig.sessionRetention)
 */
struct SessionRetentionConfig {
  bool enabled = true;                   // false = pruning disabled
  int64_t retention_ms = DEFAULT_SESSION_RETENTION_MS;
  std::string raw_retention;             // 原始配置字符串 (e.g. "24h", "48h")
};

/**
 * ReaperResult (对齐 OpenClaw ReaperResult)
 */
struct ReaperResult {
  bool swept = false;
  int pruned = 0;
};

/**
 * Logger interface (对齐 OpenClaw Logger)
 */
struct ReaperLogger {
  std::function<void(const std::string& msg)> info;
  std::function<void(const std::string& msg)> warn;
};

/**
 * Session store entry for reaper inspection
 * (对齐 OpenClaw SessionEntry used by reaper)
 */
struct ReaperSessionEntry {
  std::string session_id;
  std::string session_file;
  int64_t updated_at = 0;
};

/**
 * Session store adapter interface
 * (对齐 OpenClaw updateSessionStore + loadSessionStore)
 */
struct SessionStoreAdapter {
  // Load all session entries for a given store path
  std::function<std::unordered_map<std::string, ReaperSessionEntry>(
      const std::string& store_path)> load;

  // Update session store (remove expired entries)
  std::function<void(const std::string& store_path,
                     const std::vector<std::string>& keys_to_remove)> remove_entries;

  // Archive removed session transcripts (best-effort)
  std::function<void(const std::vector<std::pair<std::string, std::string>>& session_files,
                     int64_t retention_ms, int64_t now_ms)> archive_transcripts;
};

/**
 * 解析 retention 配置
 * 对齐 OpenClaw: resolveRetentionMs(cronConfig)
 *
 * @param config retention 配置
 * @return retention 毫秒数, nullopt 表示 pruning disabled
 */
std::optional<int64_t> resolve_retention_ms(const SessionRetentionConfig& config);

/**
 * 执行会话清理
 * 对齐 OpenClaw: sweepCronRunSessions(params)
 *
 * Designed to be called from the cron timer tick — self-throttles via
 * MIN_SWEEP_INTERVAL_MS to avoid excessive I/O.
 *
 * Lock ordering: this function acquires the session-store file lock via
 * the adapter. It must be called OUTSIDE of the cron service's own locked()
 * section to avoid lock-order inversions.
 *
 * @param session_store_path  Resolved path to sessions.json — required
 * @param retention_config    Session retention configuration
 * @param now_ms              Current time in ms (default: system clock)
 * @param logger              Logger for info/warn messages
 * @param store_adapter       Session store adapter for I/O
 * @param force               Override for testing — skips the min-interval throttle
 * @return ReaperResult with sweep status and pruned count
 */
ReaperResult sweep_cron_run_sessions(
    const std::string& session_store_path,
    const SessionRetentionConfig& retention_config,
    int64_t now_ms,
    const ReaperLogger& logger,
    const SessionStoreAdapter& store_adapter,
    bool force = false);

/**
 * 检查 session key 是否是 cron run session key
 * 对齐 OpenClaw: isCronRunSessionKey(key)
 *
 * Pattern: `...:cron:<jobId>:run:<uuid>`
 */
bool is_cron_run_session_key(const std::string& key);

/**
 * 重置 throttle 计时器 (for tests)
 * 对齐 OpenClaw: resetReaperThrottle()
 */
void reset_reaper_throttle();

}  // namespace cron
}  // namespace hunterclaw
