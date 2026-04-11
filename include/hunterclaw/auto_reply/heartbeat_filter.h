#pragma once

// 严格对齐 OpenClaw auto-reply/heartbeat-filter.ts (96行)
// + heartbeat-reply-payload.ts (23行)

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

/// Check if a user message is a heartbeat poll message.
/// 对齐 isHeartbeatUserMessage
bool is_heartbeat_user_message(
    const std::string& role,
    const std::string& content,
    const std::optional<std::string>& heartbeat_prompt = std::nullopt
);

/// Check if an assistant message is a HEARTBEAT_OK response.
/// 对齐 isHeartbeatOkResponse
bool is_heartbeat_ok_response(
    const std::string& role,
    const std::string& content,
    int ack_max_chars = 300
);

/// Filter out heartbeat user→HEARTBEAT_OK pairs from message history.
/// 对齐 filterHeartbeatPairs
struct SimpleMessage {
  std::string role;
  std::string content;
};

std::vector<SimpleMessage> filter_heartbeat_pairs(
    const std::vector<SimpleMessage>& messages,
    int ack_max_chars = 300,
    const std::optional<std::string>& heartbeat_prompt = std::nullopt
);

}  // namespace auto_reply
}  // namespace hunterclaw
