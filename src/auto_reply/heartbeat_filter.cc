// 严格对齐 OpenClaw auto-reply/heartbeat-filter.ts (96行)
#include "hunterclaw/auto_reply/heartbeat_filter.h"
#include "hunterclaw/auto_reply/heartbeat.h"
#include <algorithm>

namespace hunterclaw {
namespace auto_reply {

static const std::string HEARTBEAT_TASK_PROMPT_PREFIX =
    "Run the following periodic tasks (only those due based on their intervals):";
static const std::string HEARTBEAT_TASK_PROMPT_ACK =
    "After completing all due tasks, reply HEARTBEAT_OK.";

static std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

// 对齐 heartbeat-filter.ts:36-55
bool is_heartbeat_user_message(
    const std::string& role,
    const std::string& content,
    const std::optional<std::string>& heartbeat_prompt
) {
  if (role != "user") return false;
  std::string trimmed = trim(content);
  if (trimmed.empty()) return false;
  
  if (heartbeat_prompt) {
    std::string p = trim(*heartbeat_prompt);
    if (!p.empty() && trimmed.substr(0, p.size()) == p) return true;
  }
  
  return (trimmed.find(HEARTBEAT_TASK_PROMPT_PREFIX) != std::string::npos &&
          trimmed.find(HEARTBEAT_TASK_PROMPT_ACK) != std::string::npos);
}

// 对齐 heartbeat-filter.ts:57-69
bool is_heartbeat_ok_response(
    const std::string& role,
    const std::string& content,
    int ack_max_chars
) {
  if (role != "assistant") return false;
  std::string trimmed = trim(content);
  if (trimmed.empty()) return false;
  
  // Check if response is just HEARTBEAT_OK (with optional whitespace)
  std::string upper = trimmed;
  std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
  
  // Strip "HEARTBEAT_OK" and check if remainder is within ack_max_chars
  auto pos = upper.find("HEARTBEAT_OK");
  if (pos == std::string::npos) return false;
  
  std::string remainder = trimmed.substr(0, pos) + trimmed.substr(pos + 12);
  remainder = trim(remainder);
  return static_cast<int>(remainder.size()) <= ack_max_chars;
}

// 对齐 heartbeat-filter.ts:71-96
std::vector<SimpleMessage> filter_heartbeat_pairs(
    const std::vector<SimpleMessage>& messages,
    int ack_max_chars,
    const std::optional<std::string>& heartbeat_prompt
) {
  if (messages.size() < 2) return messages;
  
  std::vector<SimpleMessage> result;
  size_t i = 0;
  while (i < messages.size()) {
    if (i + 1 < messages.size() &&
        is_heartbeat_user_message(messages[i].role, messages[i].content, heartbeat_prompt) &&
        is_heartbeat_ok_response(messages[i+1].role, messages[i+1].content, ack_max_chars)) {
      i += 2;  // Skip the pair
      continue;
    }
    result.push_back(messages[i]);
    ++i;
  }
  return result;
}

}  // namespace auto_reply
}  // namespace hunterclaw
