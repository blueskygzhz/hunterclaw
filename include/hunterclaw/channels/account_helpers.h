#pragma once

// 严格对齐 OpenClaw: channels/plugins/account-helpers.ts (220行)
// + channels/account-snapshot-fields.ts (205行)
// + channels/account-summary.ts (68行)
// Multi-account config management framework + credential snapshot projection

#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Account list helpers (对齐 OC: createAccountListHelpers)
// ============================================================================

/// List combined account ids from configured + additional + implicit + fallback
/// (对齐 OC: listCombinedAccountIds)
std::vector<std::string> list_combined_account_ids(
    const std::vector<std::string>& configured_account_ids,
    const std::vector<std::string>& additional_account_ids = {},
    const std::optional<std::string>& implicit_account_id = std::nullopt,
    const std::optional<std::string>& fallback_account_id_when_empty = std::nullopt);

/// Resolve the default account id from a list (对齐 OC: resolveListedDefaultAccountId)
std::string resolve_listed_default_account_id(
    const std::vector<std::string>& account_ids,
    const std::optional<std::string>& configured_default = std::nullopt,
    bool allow_unlisted_default = false);

/// List configured account ids from config.channels.<key>.accounts (对齐 OC)
std::vector<std::string> list_configured_account_ids(
    const core::Config& cfg,
    const std::string& channel_key);

/// Merge channel-level config with per-account override (对齐 OC: mergeAccountConfig)
nlohmann::json merge_account_config(
    const nlohmann::json& channel_config,
    const nlohmann::json& account_config,
    const std::vector<std::string>& omit_keys = {});

/// Resolve merged account config (对齐 OC: resolveMergedAccountConfig)
nlohmann::json resolve_merged_account_config(
    const nlohmann::json& channel_config,
    const std::string& account_id,
    const std::vector<std::string>& omit_keys = {});

// ============================================================================
// Account snapshot projection (对齐 OC: account-snapshot-fields.ts)
// ============================================================================

/// Credential status values (对齐 OC)
enum class CredentialStatus { AVAILABLE, CONFIGURED_UNAVAILABLE, MISSING };

/// Resolve configured from credential statuses (对齐 OC: resolveConfiguredFromCredentialStatuses)
std::optional<bool> resolve_configured_from_credential_statuses(const nlohmann::json& account);

/// Check if any credential has "configured_unavailable" (对齐 OC)
bool has_configured_unavailable_credential_status(const nlohmann::json& account);

/// Check if any credential has resolved value (对齐 OC: hasResolvedCredentialValue)
bool has_resolved_credential_value(const nlohmann::json& account);

/// Project safe subset of account fields into snapshot (对齐 OC: projectSafeChannelAccountSnapshotFields)
ChannelAccountSnapshot project_safe_channel_account_snapshot_fields(const nlohmann::json& account);

/// Project credential snapshot fields (对齐 OC: projectCredentialSnapshotFields)
ChannelAccountSnapshot project_credential_snapshot_fields(const nlohmann::json& account);

// ============================================================================
// Account summary (对齐 OC: account-summary.ts)
// ============================================================================

/// Build channel account snapshot from plugin+account (对齐 OC: buildChannelAccountSnapshot)
ChannelAccountSnapshot build_channel_account_snapshot(
    const ChannelPlugin& plugin,
    const nlohmann::json& account,
    const core::Config& cfg,
    const std::string& account_id,
    bool enabled,
    bool configured);

/// Resolve if account is enabled (对齐 OC: resolveChannelAccountEnabled)
bool resolve_channel_account_enabled(
    const ChannelPlugin& plugin,
    const nlohmann::json& account,
    const core::Config& cfg);

/// Resolve if account is configured (对齐 OC: resolveChannelAccountConfigured)
bool resolve_channel_account_configured(
    const ChannelPlugin& plugin,
    const nlohmann::json& account,
    const core::Config& cfg);

/// Describe account snapshot (对齐 OC: describeAccountSnapshot)
ChannelAccountSnapshot describe_account_snapshot(
    const nlohmann::json& account,
    const std::optional<bool>& configured = std::nullopt);

/// Describe webhook account snapshot (对齐 OC: describeWebhookAccountSnapshot)
ChannelAccountSnapshot describe_webhook_account_snapshot(
    const nlohmann::json& account,
    const std::optional<bool>& configured = std::nullopt,
    const std::string& mode = "webhook");

} // namespace channels
} // namespace hunterclaw
