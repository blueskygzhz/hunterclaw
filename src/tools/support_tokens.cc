// ============================================================================
// Support Tokens 实现 — 严格对齐 OpenClaw sessions-send-tokens.ts
// ============================================================================

#include "hunterclaw/tools/support_tokens.h"
#include <algorithm>

namespace hunterclaw {
namespace tools {
namespace support_tokens {

// ── Token 常量 ──────────────────────────────────────────────────────────────

const std::string ANNOUNCE_SKIP_TOKEN = "__ANNOUNCE_SKIP__";
const std::string REPLY_SKIP_TOKEN    = "__REPLY_SKIP__";
const std::string SILENT_REPLY_TOKEN  = "__SILENT_REPLY__";
const std::string HEARTBEAT_TOKEN     = "__HEARTBEAT__";

// ── 辅助 ────────────────────────────────────────────────────────────────────

static bool contains_token(const std::string& text, const std::string& token) {
    return text.find(token) != std::string::npos;
}

// ── 判定函数 ────────────────────────────────────────────────────────────────

bool is_announce_skip(const std::string& reply) {
    return contains_token(reply, ANNOUNCE_SKIP_TOKEN);
}

bool is_reply_skip(const std::string& reply) {
    return contains_token(reply, REPLY_SKIP_TOKEN);
}

bool is_silent_reply(const std::string& reply) {
    return contains_token(reply, SILENT_REPLY_TOKEN);
}

bool is_heartbeat(const std::string& reply) {
    return contains_token(reply, HEARTBEAT_TOKEN);
}

bool is_non_deliverable_sessions_reply(const std::string& reply) {
    return is_announce_skip(reply) ||
           is_reply_skip(reply) ||
           is_silent_reply(reply) ||
           is_heartbeat(reply);
}

std::string strip_support_tokens(const std::string& reply) {
    std::string result = reply;

    auto erase_all = [&](const std::string& token) {
        size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::string::npos) {
            result.erase(pos, token.length());
        }
    };

    erase_all(ANNOUNCE_SKIP_TOKEN);
    erase_all(REPLY_SKIP_TOKEN);
    erase_all(SILENT_REPLY_TOKEN);
    erase_all(HEARTBEAT_TOKEN);

    return result;
}

}  // namespace support_tokens
}  // namespace tools
}  // namespace hunterclaw
