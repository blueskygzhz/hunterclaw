#pragma once

// 严格对齐 OpenClaw config/sessions/reset.ts (177行)
// Session 重置策略: daily/idle 模式 + freshness 计算

#include <optional>
#include <string>

namespace hunterclaw {
namespace sessions {

// ============================================================================
// Types (对齐 reset.ts:10-23)
// ============================================================================

enum class SessionResetMode { DAILY, IDLE };
enum class SessionResetType { DIRECT, GROUP, THREAD };

struct SessionResetPolicy {
  SessionResetMode mode = SessionResetMode::DAILY;
  int at_hour = 4;                       // Default: 4 AM
  std::optional<int> idle_minutes;       // For idle mode
};

struct SessionFreshness {
  bool fresh = true;
  std::optional<int64_t> daily_reset_at;
  std::optional<int64_t> idle_expires_at;
};

static constexpr int DEFAULT_RESET_AT_HOUR = 4;
static constexpr int DEFAULT_IDLE_MINUTES = 30;

// ============================================================================
// Functions (对齐 reset.ts:30-177)
// ============================================================================

/// Check if a session key belongs to a thread session.
bool is_thread_session_key(const std::optional<std::string>& session_key);

/// Resolve the session reset type from context.
SessionResetType resolve_session_reset_type(
    const std::optional<std::string>& session_key = std::nullopt,
    bool is_group = false,
    bool is_thread = false
);

/// Resolve the reset policy for a session type.
SessionResetPolicy resolve_session_reset_policy(
    const std::string& config_json,
    SessionResetType reset_type
);

/// Check if a session is fresh (not expired).
SessionFreshness resolve_session_freshness(
    int64_t updated_at,
    int64_t now,
    const SessionResetPolicy& policy
);

/// Convert SessionResetMode to/from string.
std::string session_reset_mode_to_string(SessionResetMode mode);
std::optional<SessionResetMode> string_to_session_reset_mode(const std::string& str);

}  // namespace sessions
}  // namespace hunterclaw
