#pragma once
// ✅ session_key.h — 薄包装，复用 resolver.h 中的规范定义
// Aligned with openclaw: src/routing/session-key.ts + account-id.ts

#include <string>
#include <optional>
#include <unordered_map>
#include "hunterclaw/routing/resolver.h"  // ✅ 规范定义来源

namespace hunterclaw {
namespace routing {

// ============================================================================
// Constants — 带 SK_ 前缀避免冲突
// ============================================================================
inline constexpr const char* SK_DEFAULT_AGENT_ID  = DEFAULT_AGENT_ID;
inline constexpr const char* SK_DEFAULT_MAIN_KEY  = DEFAULT_MAIN_KEY;
inline constexpr const char* SK_DEFAULT_ACCOUNT_ID= DEFAULT_ACCOUNT_ID;

// ============================================================================
// SessionKeyShape — 兼容旧代码的枚举值别名
// resolver.h 用 MISSING/AGENT/LEGACY_OR_ALIAS/MALFORMED_AGENT (全大写)
// session_key.h 旧代码用 Missing/Agent/LegacyOrAlias/MalformedAgent
// 使用 constexpr 变量桥接
// ============================================================================
namespace session_key_shape {
  inline constexpr SessionKeyShape Missing       = SessionKeyShape::MISSING;
  inline constexpr SessionKeyShape Agent         = SessionKeyShape::AGENT;
  inline constexpr SessionKeyShape LegacyOrAlias = SessionKeyShape::LEGACY_OR_ALIAS;
  inline constexpr SessionKeyShape MalformedAgent= SessionKeyShape::MALFORMED_AGENT;
}

// ============================================================================
// 内联包装函数（保持向后兼容，委托给 resolver.cc）
// ============================================================================

// build_agent_main_session_key 简化签名版本
inline std::string build_agent_main_session_key(const std::string& agent_id,
                                                 const std::string& main_key = "") {
  return ::hunterclaw::routing::build_agent_main_session_key({agent_id,
      main_key.empty() ? std::nullopt : std::optional<std::string>(main_key)});
}

// ============================================================================
// isAcpSessionKey / isCronSessionKey / isSubagentSessionKey
// ============================================================================
inline bool sk_is_acp_session_key(const std::string& key) {
  auto parsed = parse_agent_session_key(key);
  return parsed && parsed->rest.find("acp:") == 0;
}

inline bool sk_is_cron_session_key(const std::string& key) {
  auto parsed = parse_agent_session_key(key);
  return parsed && parsed->rest.find("cron:") == 0;
}

inline bool sk_is_subagent_session_key(const std::string& key) {
  auto parsed = parse_agent_session_key(key);
  return parsed && parsed->rest.find("sub:") == 0;
}

// ============================================================================
// Account Lookup (对齐 account-lookup.ts:resolveAccountEntry)
// ============================================================================
template<typename T>
inline std::optional<T> resolve_account_entry(
    const std::unordered_map<std::string, T>& accounts,
    const std::string& account_id
) {
  if (accounts.empty()) return std::nullopt;
  auto it = accounts.find(account_id);
  if (it != accounts.end()) return it->second;
  std::string lower = account_id;
  for (auto& c : lower) c = std::tolower(c);
  for (const auto& [key, val] : accounts) {
    std::string key_lower = key;
    for (auto& c : key_lower) c = std::tolower(c);
    if (key_lower == lower) return val;
  }
  return std::nullopt;
}

// ============================================================================
// getSubagentDepth — 对齐 OpenClaw session-key-utils.ts
// ============================================================================
inline int sk_get_subagent_depth(const std::string& key) {
  return get_subagent_depth(key);
}

// ============================================================================
// scopedHeartbeatWakeOptions — 对齐 OpenClaw session-key.ts:33-38
// 如果 sessionKey 是 agent: 格式，返回 true（调用方应在 wakeOptions 中注入 sessionKey）
// ============================================================================
inline bool sk_should_scope_heartbeat_wake(const std::string& key) {
  return should_scope_heartbeat_wake(key);
}

} // namespace routing
} // namespace hunterclaw
