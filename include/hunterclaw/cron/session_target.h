#pragma once

// 严格对齐 OpenClaw: src/cron/session-target.ts (17行)
// sessionTarget 安全验证

#include <stdexcept>
#include <string>

namespace hunterclaw {
namespace cron {

// 对齐 OpenClaw: INVALID_CRON_SESSION_TARGET_ID_ERROR
constexpr const char* INVALID_CRON_SESSION_TARGET_ID_ERROR =
    "invalid cron sessionTarget session id";

/**
 * 检查错误是否为 invalid session target id 错误
 * 对齐 OpenClaw: isInvalidCronSessionTargetIdError(error)
 */
bool is_invalid_cron_session_target_id_error(const std::exception& error);

/**
 * 验证 cron sessionTarget 的 session ID 安全性
 * 对齐 OpenClaw: assertSafeCronSessionTargetId(sessionId)
 *
 * Throws if:
 * - Session ID is empty
 * - Contains "/" or "\" or null bytes
 *
 * @param session_id  Session ID to validate
 * @return trimmed session ID
 */
std::string assert_safe_cron_session_target_id(const std::string& session_id);

}  // namespace cron
}  // namespace hunterclaw
