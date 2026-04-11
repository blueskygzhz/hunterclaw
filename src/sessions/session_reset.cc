// 严格对齐 OpenClaw config/sessions/reset.ts (177行)
#include "hunterclaw/sessions/session_reset.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace sessions {

static std::string to_lower(const std::string& s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(), ::tolower);
  return r;
}

bool is_thread_session_key(const std::optional<std::string>& session_key) {
  if (!session_key || session_key->empty()) return false;
  return session_key->find(":thread:") != std::string::npos;
}

// 对齐 reset.ts:34-49
SessionResetType resolve_session_reset_type(
    const std::optional<std::string>& session_key,
    bool is_group, bool is_thread
) {
  if (is_thread || is_thread_session_key(session_key)) return SessionResetType::THREAD;
  if (is_group) return SessionResetType::GROUP;
  if (session_key) {
    std::string lower = to_lower(*session_key);
    if (lower.find(":group:") != std::string::npos || lower.find(":channel:") != std::string::npos)
      return SessionResetType::GROUP;
  }
  return SessionResetType::DIRECT;
}

// 对齐 reset.ts:51-100
SessionResetPolicy resolve_session_reset_policy(
    const std::string& config_json,
    SessionResetType reset_type
) {
  SessionResetPolicy policy;
  policy.mode = SessionResetMode::DAILY;
  policy.at_hour = DEFAULT_RESET_AT_HOUR;
  
  try {
    auto cfg = json::parse(config_json);
    if (!cfg.contains("session")) return policy;
    auto& session = cfg["session"];
    
    // Resolve reset config based on type
    json reset_cfg;
    std::string type_key;
    switch (reset_type) {
      case SessionResetType::DIRECT: type_key = "direct"; break;
      case SessionResetType::GROUP: type_key = "group"; break;
      case SessionResetType::THREAD: type_key = "thread"; break;
    }
    
    if (session.contains("reset") && session["reset"].is_object()) {
      auto& reset = session["reset"];
      if (reset.contains(type_key) && reset[type_key].is_object()) {
        reset_cfg = reset[type_key];
      } else {
        reset_cfg = reset;
      }
    }
    
    if (reset_cfg.contains("mode")) {
      auto m = string_to_session_reset_mode(reset_cfg["mode"].get<std::string>());
      if (m) policy.mode = *m;
    }
    if (reset_cfg.contains("atHour") && reset_cfg["atHour"].is_number()) {
      policy.at_hour = std::clamp(reset_cfg["atHour"].get<int>(), 0, 23);
    }
    if (reset_cfg.contains("idleMinutes") && reset_cfg["idleMinutes"].is_number()) {
      policy.idle_minutes = std::max(1, reset_cfg["idleMinutes"].get<int>());
    }
    
    // Fallback idle minutes
    if (policy.mode == SessionResetMode::IDLE && !policy.idle_minutes) {
      if (session.contains("idleMinutes") && session["idleMinutes"].is_number()) {
        policy.idle_minutes = std::max(1, session["idleMinutes"].get<int>());
      } else {
        policy.idle_minutes = DEFAULT_IDLE_MINUTES;
      }
    }
  } catch (...) {}
  
  return policy;
}

// 对齐 reset.ts:102-177
SessionFreshness resolve_session_freshness(
    int64_t updated_at, int64_t now, const SessionResetPolicy& policy
) {
  SessionFreshness result;
  result.fresh = true;
  
  if (policy.mode == SessionResetMode::IDLE) {
    int idle_ms = (policy.idle_minutes.value_or(DEFAULT_IDLE_MINUTES)) * 60 * 1000;
    int64_t expires_at = updated_at + idle_ms;
    result.idle_expires_at = expires_at;
    if (now >= expires_at) {
      result.fresh = false;
    }
  } else {
    // Daily mode
    time_t now_sec = static_cast<time_t>(now / 1000);
    struct tm* tm_now = localtime(&now_sec);
    
    // Calculate today's reset time
    struct tm reset_time = *tm_now;
    reset_time.tm_hour = policy.at_hour;
    reset_time.tm_min = 0;
    reset_time.tm_sec = 0;
    int64_t daily_reset_at = static_cast<int64_t>(mktime(&reset_time)) * 1000;
    
    // If reset time is in the future today, use yesterday's
    if (daily_reset_at > now) {
      daily_reset_at -= 24LL * 60 * 60 * 1000;
    }
    
    result.daily_reset_at = daily_reset_at;
    if (updated_at < daily_reset_at) {
      result.fresh = false;
    }
  }
  
  return result;
}

std::string session_reset_mode_to_string(SessionResetMode mode) {
  switch (mode) {
    case SessionResetMode::DAILY: return "daily";
    case SessionResetMode::IDLE: return "idle";
  }
  return "daily";
}

std::optional<SessionResetMode> string_to_session_reset_mode(const std::string& str) {
  std::string lower = to_lower(str);
  if (lower == "daily") return SessionResetMode::DAILY;
  if (lower == "idle") return SessionResetMode::IDLE;
  return std::nullopt;
}

}  // namespace sessions
}  // namespace hunterclaw
