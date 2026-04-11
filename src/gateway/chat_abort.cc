// ============================================================================
// chat_abort.cc - Chat Run Abort
// 🎯 严格对齐 OpenClaw gateway/chat-abort.ts (130 行)
//
// 核心功能:
//   - isChatStopCommandText: 检测 abort 请求文本
//   - resolveChatRunExpiresAtMs: 计算 run 过期时间
//   - abortChatRunById: 按 runId 中止
//   - abortChatRunsForSessionKey: 按 sessionKey 批量中止
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <set>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// 对齐 chat-abort.ts:13 isChatStopCommandText
bool is_chat_stop_command_text(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    // trim
    auto s = lower.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return false;
    auto e = lower.find_last_not_of(" \t\n\r");
    lower = lower.substr(s, e - s + 1);
    return lower == "stop" || lower == "/stop" || lower == "cancel" || lower == "/cancel" ||
           lower == "abort" || lower == "/abort";
}

// 对齐 chat-abort.ts:17-30 resolveChatRunExpiresAtMs
int64_t resolve_chat_run_expires_at_ms(
    int64_t now,
    int64_t timeout_ms,
    int64_t grace_ms,
    int64_t min_ms,
    int64_t max_ms
) {
    if (grace_ms <= 0) grace_ms = 60000;
    if (min_ms <= 0) min_ms = 2 * 60000;
    if (max_ms <= 0) max_ms = 24LL * 60 * 60000;
    int64_t bounded_timeout = std::max(int64_t(0), timeout_ms);
    int64_t target = now + bounded_timeout + grace_ms;
    int64_t min_val = now + min_ms;
    int64_t max_val = now + max_ms;
    return std::min(max_val, std::max(min_val, target));
}

// 对齐 chat-abort.ts:76-108 abortChatRunById
// 此函数由 chat.cc 中的 chat abort 逻辑调用，通过传入的 abort_controllers map 操作
// 在 HunterClaw 中，abort 逻辑集成在 gateway.cc 和 chat.cc 中

}  // namespace gateway
}  // namespace hunterclaw
