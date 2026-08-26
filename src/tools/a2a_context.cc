// ============================================================================
// A2A Context 构建器实现 — 严格对齐 OpenClaw sessions-send-helpers.ts
// ============================================================================

#include "hunterclaw/tools/a2a_context.h"
#include "hunterclaw/tools/support_tokens.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace hunterclaw {
namespace tools {

namespace {

/// 対齐 helpers.ts:89,93 —— session key 占位符，保护 provider prompt-cache 复用。
constexpr const char* REQUESTER_SESSION_PLACEHOLDER = "<REQUESTER_SESSION>";
constexpr const char* TARGET_SESSION_PLACEHOLDER = "<TARGET_SESSION>";

/// 対齐 helpers.ts:79-96 buildAgentSessionLines(params)
///   requesterSessionKey 存在 → "Agent 1 (requester) session: <REQUESTER_SESSION>."
///   requesterChannel 存在   → "Agent 1 (requester) channel: {channel}."
///   总是输出               → "Agent 2 (target) session: <TARGET_SESSION>."
///   targetChannel 存在      → "Agent 2 (target) channel: {channel}."
/// 空值行按 filter(Boolean) 语义丢弃。
std::vector<std::string> build_agent_session_lines(
    std::string_view requester_session_key,
    std::string_view requester_channel,
    std::string_view target_channel)
{
    std::vector<std::string> lines;
    if (!requester_session_key.empty()) {
        lines.emplace_back(std::string("Agent 1 (requester) session: ")
                           + REQUESTER_SESSION_PLACEHOLDER + ".");
    }
    if (!requester_channel.empty()) {
        lines.emplace_back("Agent 1 (requester) channel: "
                           + std::string(requester_channel) + ".");
    }
    lines.emplace_back(std::string("Agent 2 (target) session: ")
                       + TARGET_SESSION_PLACEHOLDER + ".");
    if (!target_channel.empty()) {
        lines.emplace_back("Agent 2 (target) channel: "
                           + std::string(target_channel) + ".");
    }
    return lines;
}

/// 対齐 Array.prototype.join("\n")
std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream oss;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) oss << "\n";
        oss << lines[i];
    }
    return oss.str();
}

}  // namespace

int resolve_ping_pong_turns() {
    // 対齐 helpers.ts:159 Math.min(MAX_PING_PONG_TURNS, DEFAULT_AGENTNG_PONG_TURNS)
    return std::min(MAX_PING_PONG_TURNS, DEFAULT_AGENT_PING_PONG_TURNS);
}

std::string build_agent_to_agent_message_context(
    std::string_view requester_session_key,
    std::string_view requester_channel,
    std::string_view target_session_key)
{
    // 対齐 helpers.ts:104-107 —— 注意真源此处不传 targetChannel，故 target channel 行不出现。
    (void)target_session_key;  // 対齐:93 使用占位符，不使用具体 key
    std::vector<std::string> lines;
    lines.emplace_back("Agent-to-agent message context:");
    for (auto& line : build_agent_session_lines(
             requester_session_key, requester_channel, /*target_channel=*/"")) {
        lines.push_back(std::move(line));
    }
    return join_lines(lines);
}

std::string build_agent_to_agent_reply_context(
    std::string_view requester_session_key,
    std::string_view requester_channel,
    std::string_view target_session_key,
    std::string_view target_channel,
    A2ARole current_role,
    int turn,
    int max_turns)
{
    (void)target_session_key;  // 対齐:93 占位符
    // 対齐 helpers.ts:120-121
    const char* current_label = (current_role == A2ARole::Requester)
        ? "Agent 1 (requester)" : "Agent 2 (target)";

    std::vector<std::string> lines;
    lines.emplace_back("Agent-to-agent reply step:");
    lines.emplace_back(std::string("Current agent: ") + current_label + ".");
    lines.emplace_back("Turn " + std::to_string(turn) + " of "
                       + std::to_string(max_turns) + ".");
    for (auto& line : build_agent_session_lines(
             requester_session_key, requester_channel, target_channel)) {
        lines.push_back(std::move(line));
    }
    // 対齐 helpers.ts:127
    lines.emplace_back("If you want to stop the ping-pong, reply exactly \""
                       + support_tokens::REPLY_SKIP_TOKEN + "\".");
    return join_lines(lines);
}

std::string build_agent_to_agent_announce_context(
    std::string_view requester_session_key,
    std::string_view requester_channel,
    std::string_view target_session_key,
    std::string_view target_channel,
    std::string_view original_message,
    std::string_view round_one_reply,
    std::string_view latest_reply)
{
    (void)target_session_key;  // 対齐:93 占位符

    std::vector<std::string> lines;
    lines.emplace_back("Agent-to-agent announce step:");
    for (auto& line : build_agent_session_lines(
             requester_session_key, requester_channel, target_channel)) {
        lines.push_back(std::move(line));
    }
    // 対齐 helpers.ts:145-149
    lines.emplace_back("Original request: " + std::string(original_message));
    lines.emplace_back(!round_one_reply.empty()
        ? "Round 1 reply: " + std::string(round_one_reply)
        : std::string("Round 1 reply: (not available)."));
    lines.emplace_back(!latest_reply.empty()
        ? "Latest reply: " + std::string(latest_reply)
        : std::string("Latest reply: (not available)."));
    // 対齐 helpers.ts:150-152
    lines.emplace_back("If you want to remain silent, reply exactly \""
                       + support_tokens::ANNOUNCE_SKIP_TOKEN + "\".");
    lines.emplace_back("Any other reply will be posted to the target channel.");
    lines.emplace_back("After this reply, the agent-to-agent conversation is over.");
    return join_lines(lines);
}

}  // namespace tools
}  // namespace hunterclaw
