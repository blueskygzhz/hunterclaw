#pragma once

// 严格对齐 OpenClaw config/sessions/store-cache.ts (88行)
// + store-lock-state.ts (52行) + store-migrations.ts (28行)
// Session store 缓存 + 写锁 + 迁移

#include "hunterclaw/sessions/session_manager.h"
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace hunterclaw {
namespace sessions {

// ============================================================================
// Store Cache (对齐 store-cache.ts)
// ============================================================================

static constexpr int64_t DEFAULT_SESSION_STORE_TTL_MS = 45000;  // 45 seconds

class SessionStoreCache {
 public:
  static SessionStoreCache& instance();

  bool is_enabled() const;
  void clear();
  void invalidate(const std::string& store_path);

  std::optional<std::unordered_map<std::string, SessionEntry>> get(
      const std::string& store_path
  ) const;

  void set(
      const std::string& store_path,
      const std::unordered_map<std::string, SessionEntry>& store
  );

 private:
  SessionStoreCache() = default;
  mutable std::mutex mutex_;
  struct CacheEntry {
    std::unordered_map<std::string, SessionEntry> store;
    std::chrono::steady_clock::time_point expires_at;
  };
  std::unordered_map<std::string, CacheEntry> cache_;
};

// ============================================================================
// Store Migrations (对齐 store-migrations.ts)
// ============================================================================

/// Apply best-effort field migrations to session store entries.
/// provider → channel, lastProvider → lastChannel, room → groupChannel
void apply_session_store_migrations(
    std::unordered_map<std::string, SessionEntry>& store
);

}  // namespace sessions
}  // namespace hunterclaw
