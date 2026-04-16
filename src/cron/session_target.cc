// 严格对齐 OpenClaw: src/cron/session-target.ts (17行)
// sessionTarget 安全验证

#include "hunterclaw/cron/session_target.h"

#include <algorithm>

namespace hunterclaw {
namespace cron {

// ============================================================================
// isInvalidCronSessionTargetIdError (对齐 OpenClaw)
// ============================================================================

bool is_invalid_cron_session_target_id_error(const std::exception& error) {
  return std::string(error.what()) == INVALID_CRON_SESSION_TARGET_ID_ERROR;
}

// ============================================================================
// assertSafeCronSessionTargetId (对齐 OpenClaw)
// ============================================================================

std::string assert_safe_cron_session_target_id(const std::string& session_id) {
  // Trim whitespace
  std::string trimmed = session_id;
  auto start = trimmed.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    throw std::runtime_error(INVALID_CRON_SESSION_TARGET_ID_ERROR);
  }
  auto end = trimmed.find_last_not_of(" \t\r\n");
  trimmed = trimmed.substr(start, end - start + 1);

  if (trimmed.empty()) {
    throw std::runtime_error(INVALID_CRON_SESSION_TARGET_ID_ERROR);
  }

  // Check for path traversal characters (对齐 OpenClaw)
  if (trimmed.find('/') != std::string::npos ||
      trimmed.find('\\') != std::string::npos ||
      trimmed.find('\0') != std::string::npos) {
    throw std::runtime_error(INVALID_CRON_SESSION_TARGET_ID_ERROR);
  }

  return trimmed;
}

}  // namespace cron
}  // namespace hunterclaw
