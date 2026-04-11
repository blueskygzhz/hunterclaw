#pragma once

// 严格对齐 OpenClaw auto-reply/command-auth.ts (724行)
// 命令授权系统: 解析 provider + owner allowlist + sender 鉴权

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Types (对齐 command-auth.ts:18-26)
// ============================================================================

struct CommandAuthorization {
  std::optional<std::string> provider_id;
  std::vector<std::string> owner_list;
  std::optional<std::string> sender_id;
  bool sender_is_owner = false;
  bool is_authorized_sender = false;
  std::optional<std::string> from;
  std::optional<std::string> to;
};

// ============================================================================
// Resolve (对齐 command-auth.ts:563-724)
// ============================================================================

/// Resolve command authorization from message context + config.
/// 对齐 resolveCommandAuthorization
CommandAuthorization resolve_command_authorization(
    const std::string& config_json,
    const std::string& channel,
    const std::optional<std::string>& from = std::nullopt,
    const std::optional<std::string>& to = std::nullopt,
    const std::optional<std::string>& account_id = std::nullopt,
    const std::optional<std::string>& surface = std::nullopt
);

/// Check if a sender is in the owner list.
/// 对齐 isSenderOwner
bool is_sender_owner(
    const std::vector<std::string>& owner_list,
    const std::string& sender_id
);

/// Check if allow-all mode is enabled.
/// 对齐 resolveOwnerAuthorizationState
bool is_allow_all_mode(const std::string& config_json);

/// Resolve the owner list from config.
/// 对齐 resolveExplicitOwners
std::vector<std::string> resolve_explicit_owners(const std::string& config_json);

/// Resolve provider-specific allowFrom list.
/// 对齐 resolveProviderAllowFrom
std::vector<std::string> resolve_provider_allow_from(
    const std::string& config_json,
    const std::string& provider_id,
    const std::optional<std::string>& account_id = std::nullopt
);

/// Normalize a sender ID for comparison.
std::string normalize_sender_id(const std::string& raw);

}  // namespace auto_reply
}  // namespace hunterclaw
