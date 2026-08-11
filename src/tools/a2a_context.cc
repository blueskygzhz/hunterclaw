// ============================================================================
// A2A Context 构建器实现 — 严格对齐 OpenClaw sessions-send-helpers.ts
// ============================================================================

#include "hunterclaw/tools/a2a_context.h"
#include "hunterclaw/tools/support_tokens.h"
#include <sstream>

namespace hunterclaw {
namespace tools {

std::string build_agent_to_agent_message_context(
    std::string_view source_session_key,
    std::string_view agent_name_or_id,
    std::string_view message)
{
    // 对齐 OpenClaw:
    // [SYSTEM: This is a message from Agent {agentName} ({sessionKey}).
    // If you want to reply, include ONLY the reply in your response.
    // Include __REPLY_SKIP__ if you don't want to reply.
    // Include __ANNOUNCE_SKIP__ if you don't want to announce to the
    // source channel.]
    // ---
    // {message}
    std::ostringstream oss;
    oss << "[SYSTEM: This is a message from Agent " << agent_name_or_id
        << " (" << source_session_key << "). "
        << "If you want to reply, include ONLY the reply in your response. "
        << "Include " << support_tokens::REPLY_SKIP_TOKEN
        << " if you don't want to reply. "
        << "Include " << support_tokens::ANNOUNCE_SKIP_TOKEN
        << " if you don't want to announce to the source channel.]\n"
        << "---\n"
        << message;
    return oss.str();
}

std::string build_agent_to_agent_reply_context(
    int turn,
    int max_turns,
    std::string_view reply_text)
{
    // 对齐 OpenClaw:
    // Agent-to-agent reply step: Turn {turn + 1}/{maxTurns}. Reply to the
    // last message. Include __REPLY_SKIP__ if done replying.
    std::ostringstream oss;
    oss << "Agent-to-agent reply step: Turn " << turn
        << " of " << max_turns << ". "
        << "Reply to the last message. "
        << "Include " << support_tokens::REPLY_SKIP_TOKEN
        << " if done replying.\n\n"
        << reply_text;
    return oss.str();
}

std::string build_agent_to_agent_announce_context(
    std::string_view original_message)
{
    // 对齐 OpenClaw:
    // Agent-to-agent announce step: Original request:
    // {originalMessage}
    //
    // You may want to send a different message.
    // If you want to skip announcing, include __ANNOUNCE_SKIP__.
    std::ostringstream oss;
    oss << "Agent-to-agent announce step: Original request:\n"
        << "---\n"
        << original_message << "\n"
        << "---\n\n"
        << "You may want to send a different message. "
        << "If you want to skip announcing, "
        << "include " << support_tokens::ANNOUNCE_SKIP_TOKEN << ".";
    return oss.str();
}

}  // namespace tools
}  // namespace hunterclaw
