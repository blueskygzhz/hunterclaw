// 严格对齐 OpenClaw auto-reply/send-policy.ts (47行)
#include "hunterclaw/auto_reply/send_policy.h"
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace auto_reply {

static std::string trim_lower(const std::string& s) {
  std::string r = s;
  auto start = r.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = r.find_last_not_of(" \t\n\r");
  r = r.substr(start, end - start + 1);
  std::transform(r.begin(), r.end(), r.begin(), ::tolower);
  return r;
}

std::optional<SendPolicyOverride> normalize_send_policy_override(const std::string& raw) {
  std::string v = trim_lower(raw);
  if (v.empty()) return std::nullopt;
  if (v == "allow" || v == "on") return SendPolicyOverride::ALLOW;
  if (v == "deny" || v == "off") return SendPolicyOverride::DENY;
  return std::nullopt;
}

SendPolicyParseResult parse_send_policy_command(const std::string& raw) {
  if (raw.empty()) return {false, std::nullopt};
  std::string trimmed = raw;
  auto s = trimmed.find_first_not_of(" \t\n\r");
  if (s == std::string::npos) return {false, std::nullopt};
  auto e = trimmed.find_last_not_of(" \t\n\r");
  trimmed = trimmed.substr(s, e - s + 1);
  if (trimmed.empty()) return {false, std::nullopt};
  
  std::regex send_re(R"(^/send(?:\s+([a-zA-Z]+))?\s*$)", std::regex_constants::icase);
  std::smatch match;
  if (!std::regex_match(trimmed, match, send_re)) return {false, std::nullopt};
  
  std::string token = trim_lower(match[1].str());
  if (token.empty()) return {true, std::nullopt};
  if (token == "inherit" || token == "default" || token == "reset") return {true, "inherit"};
  
  auto mode = normalize_send_policy_override(token);
  if (mode == SendPolicyOverride::ALLOW) return {true, "allow"};
  if (mode == SendPolicyOverride::DENY) return {true, "deny"};
  return {true, std::nullopt};
}

}  // namespace auto_reply
}  // namespace hunterclaw
