#pragma once

// 严格对齐 OpenClaw auto-reply/send-policy.ts (47行)

#include <optional>
#include <string>

namespace hunterclaw {
namespace auto_reply {

enum class SendPolicyOverride { ALLOW, DENY };

/// Normalize a send policy override string.
/// 对齐 normalizeSendPolicyOverride
std::optional<SendPolicyOverride> normalize_send_policy_override(const std::string& raw);

/// Parse a /send command.
/// 对齐 parseSendPolicyCommand
struct SendPolicyParseResult {
  bool has_command = false;
  std::optional<std::string> mode;  // "allow" | "deny" | "inherit"
};

SendPolicyParseResult parse_send_policy_command(const std::string& raw);

}  // namespace auto_reply
}  // namespace hunterclaw
