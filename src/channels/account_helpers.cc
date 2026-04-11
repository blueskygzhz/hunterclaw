#include "hunterclaw/channels/account_helpers.h"
#include <algorithm>
#include <set>

namespace hunterclaw {
namespace channels {

namespace {
    std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        return s.substr(start, s.find_last_not_of(" \t\n\r") - start + 1);
    }

    std::optional<std::string> read_string(const nlohmann::json& j, const std::string& key) {
        if (!j.contains(key) || !j[key].is_string()) return std::nullopt;
        auto s = trim(j[key].get<std::string>());
        return s.empty() ? std::nullopt : std::make_optional(s);
    }

    std::optional<bool> read_bool(const nlohmann::json& j, const std::string& key) {
        if (!j.contains(key) || !j[key].is_boolean()) return std::nullopt;
        return j[key].get<bool>();
    }

    std::optional<int> read_int(const nlohmann::json& j, const std::string& key) {
        if (!j.contains(key) || !j[key].is_number_integer()) return std::nullopt;
        return j[key].get<int>();
    }

    std::optional<int64_t> read_int64(const nlohmann::json& j, const std::string& key) {
        if (!j.contains(key) || !j[key].is_number()) return std::nullopt;
        return j[key].get<int64_t>();
    }

    std::optional<std::string> read_credential_status(const nlohmann::json& j, const std::string& key) {
        if (!j.contains(key) || !j[key].is_string()) return std::nullopt;
        auto v = j[key].get<std::string>();
        if (v == "available" || v == "configured_unavailable" || v == "missing") return v;
        return std::nullopt;
    }

    const std::vector<std::string> CREDENTIAL_STATUS_KEYS = {
        "tokenStatus", "botTokenStatus", "appTokenStatus", "signingSecretStatus", "userTokenStatus"
    };

    const std::vector<std::string> CREDENTIAL_VALUE_KEYS = {
        "token", "botToken", "appToken", "signingSecret", "userToken"
    };
} // namespace

// ============================================================================
// list_combined_account_ids (对齐 OC)
// ============================================================================

std::vector<std::string> list_combined_account_ids(
    const std::vector<std::string>& configured_account_ids,
    const std::vector<std::string>& additional_account_ids,
    const std::optional<std::string>& implicit_account_id,
    const std::optional<std::string>& fallback_account_id_when_empty) {
    std::set<std::string> ids;
    for (const auto& id : configured_account_ids) {
        if (!id.empty()) ids.insert(id);
    }
    for (const auto& id : additional_account_ids) {
        if (!id.empty()) ids.insert(id);
    }
    if (implicit_account_id && !implicit_account_id->empty()) {
        ids.insert(*implicit_account_id);
    }
    if (ids.empty() && fallback_account_id_when_empty && !fallback_account_id_when_empty->empty()) {
        return {*fallback_account_id_when_empty};
    }
    return {ids.begin(), ids.end()};
}

// ============================================================================
// resolve_listed_default_account_id (对齐 OC)
// ============================================================================

std::string resolve_listed_default_account_id(
    const std::vector<std::string>& account_ids,
    const std::optional<std::string>& configured_default,
    bool allow_unlisted_default) {
    if (configured_default && !configured_default->empty()) {
        if (allow_unlisted_default) return *configured_default;
        for (const auto& id : account_ids) {
            if (id == *configured_default) return *configured_default;
        }
    }
    for (const auto& id : account_ids) {
        if (id == "default") return "default";
    }
    return account_ids.empty() ? "default" : account_ids[0];
}

// ============================================================================
// list_configured_account_ids (对齐 OC)
// ============================================================================

std::vector<std::string> list_configured_account_ids(
    const core::Config& cfg,
    const std::string& channel_key) {
    std::vector<std::string> result;
    if (!cfg.data.contains("channels")) return result;
    auto& channels = cfg.data["channels"];
    if (!channels.contains(channel_key)) return result;
    auto& ch = channels[channel_key];
    if (!ch.contains("accounts") || !ch["accounts"].is_object()) return result;
    for (auto it = ch["accounts"].begin(); it != ch["accounts"].end(); ++it) {
        if (!it.key().empty()) result.push_back(it.key());
    }
    return result;
}

// ============================================================================
// merge_account_config (对齐 OC)
// ============================================================================

nlohmann::json merge_account_config(
    const nlohmann::json& channel_config,
    const nlohmann::json& account_config,
    const std::vector<std::string>& omit_keys) {
    std::set<std::string> skip(omit_keys.begin(), omit_keys.end());
    skip.insert("accounts");
    nlohmann::json base = nlohmann::json::object();
    if (channel_config.is_object()) {
        for (auto it = channel_config.begin(); it != channel_config.end(); ++it) {
            if (skip.count(it.key()) == 0) base[it.key()] = it.value();
        }
    }
    if (account_config.is_object()) {
        for (auto it = account_config.begin(); it != account_config.end(); ++it) {
            base[it.key()] = it.value();
        }
    }
    return base;
}

// ============================================================================
// resolve_merged_account_config (对齐 OC)
// ============================================================================

nlohmann::json resolve_merged_account_config(
    const nlohmann::json& channel_config,
    const std::string& account_id,
    const std::vector<std::string>& omit_keys) {
    nlohmann::json account_config;
    if (channel_config.is_object() && channel_config.contains("accounts") &&
        channel_config["accounts"].is_object() && channel_config["accounts"].contains(account_id)) {
        account_config = channel_config["accounts"][account_id];
    }
    return merge_account_config(channel_config, account_config, omit_keys);
}

// ============================================================================
// resolve_configured_from_credential_statuses (对齐 OC)
// ============================================================================

std::optional<bool> resolve_configured_from_credential_statuses(const nlohmann::json& account) {
    if (!account.is_object()) return std::nullopt;
    bool saw = false;
    for (const auto& key : CREDENTIAL_STATUS_KEYS) {
        auto status = read_credential_status(account, key);
        if (!status) continue;
        saw = true;
        if (*status != "missing") return true;
    }
    return saw ? std::make_optional(false) : std::nullopt;
}

// ============================================================================
// has_configured_unavailable_credential_status (对齐 OC)
// ============================================================================

bool has_configured_unavailable_credential_status(const nlohmann::json& account) {
    if (!account.is_object()) return false;
    for (const auto& key : CREDENTIAL_STATUS_KEYS) {
        auto status = read_credential_status(account, key);
        if (status && *status == "configured_unavailable") return true;
    }
    return false;
}

// ============================================================================
// has_resolved_credential_value (对齐 OC)
// ============================================================================

bool has_resolved_credential_value(const nlohmann::json& account) {
    if (!account.is_object()) return false;
    for (const auto& key : CREDENTIAL_VALUE_KEYS) {
        auto v = read_string(account, key);
        if (v) return true;
    }
    for (const auto& key : CREDENTIAL_STATUS_KEYS) {
        auto status = read_credential_status(account, key);
        if (status && *status == "available") return true;
    }
    return false;
}

// ============================================================================
// project_credential_snapshot_fields (对齐 OC)
// ============================================================================

ChannelAccountSnapshot project_credential_snapshot_fields(const nlohmann::json& account) {
    ChannelAccountSnapshot snap;
    if (!account.is_object()) return snap;
    snap.token_source = read_string(account, "tokenSource");
    snap.bot_token_source = read_string(account, "botTokenSource");
    snap.app_token_source = read_string(account, "appTokenSource");
    snap.signing_secret_source = read_string(account, "signingSecretSource");
    snap.token_status = read_credential_status(account, "tokenStatus");
    snap.bot_token_status = read_credential_status(account, "botTokenStatus");
    snap.app_token_status = read_credential_status(account, "appTokenStatus");
    snap.signing_secret_status = read_credential_status(account, "signingSecretStatus");
    snap.user_token_status = read_credential_status(account, "userTokenStatus");
    return snap;
}

// ============================================================================
// project_safe_channel_account_snapshot_fields (对齐 OC)
// ============================================================================

ChannelAccountSnapshot project_safe_channel_account_snapshot_fields(const nlohmann::json& account) {
    if (!account.is_object()) return {};
    auto cred = project_credential_snapshot_fields(account);
    cred.name = read_string(account, "name");
    cred.linked = read_bool(account, "linked");
    cred.running = read_bool(account, "running");
    cred.connected = read_bool(account, "connected");
    cred.reconnect_attempts = read_int(account, "reconnectAttempts");
    cred.mode = read_string(account, "mode");
    cred.dm_policy = read_string(account, "dmPolicy");
    cred.base_url = read_string(account, "baseUrl");
    cred.cli_path = read_string(account, "cliPath");
    cred.db_path = read_string(account, "dbPath");
    cred.port = read_int(account, "port");
    return cred;
}

// ============================================================================
// build_channel_account_snapshot (对齐 OC: account-summary.ts)
// ============================================================================

ChannelAccountSnapshot build_channel_account_snapshot(
    const ChannelPlugin& plugin,
    const nlohmann::json& account,
    const core::Config& cfg,
    const std::string& account_id,
    bool enabled,
    bool configured) {
    auto snap = project_safe_channel_account_snapshot_fields(account);
    // Apply plugin describeAccount if available
    if (plugin.config && plugin.config->describe_account) {
        auto described = plugin.config->describe_account(account, cfg);
        // Merge described fields
        if (described.enabled.has_value()) snap.enabled = described.enabled;
        if (described.configured.has_value()) snap.configured = described.configured;
        if (described.name.has_value()) snap.name = described.name;
        if (described.mode.has_value()) snap.mode = described.mode;
    }
    snap.enabled = enabled;
    snap.configured = configured;
    snap.account_id = account_id;
    return snap;
}

// ============================================================================
// resolve_channel_account_enabled (对齐 OC)
// ============================================================================

bool resolve_channel_account_enabled(
    const ChannelPlugin& plugin,
    const nlohmann::json& account,
    const core::Config& cfg) {
    if (plugin.config && plugin.config->is_enabled) {
        return plugin.config->is_enabled(account, cfg);
    }
    if (account.is_object() && account.contains("enabled") && account["enabled"].is_boolean()) {
        return account["enabled"].get<bool>();
    }
    return true;
}

// ============================================================================
// resolve_channel_account_configured (对齐 OC)
// ============================================================================

bool resolve_channel_account_configured(
    const ChannelPlugin& plugin,
    const nlohmann::json& account,
    const core::Config& cfg) {
    if (plugin.config && plugin.config->is_configured) {
        return plugin.config->is_configured(account, cfg);
    }
    return true;
}

// ============================================================================
// describe_account_snapshot (对齐 OC)
// ============================================================================

ChannelAccountSnapshot describe_account_snapshot(
    const nlohmann::json& account,
    const std::optional<bool>& configured) {
    ChannelAccountSnapshot snap;
    snap.account_id = account.value("accountId", "default");
    snap.name = read_string(account, "name");
    snap.enabled = !account.contains("enabled") || account.value("enabled", true);
    snap.configured = configured;
    return snap;
}

// ============================================================================
// describe_webhook_account_snapshot (对齐 OC)
// ============================================================================

ChannelAccountSnapshot describe_webhook_account_snapshot(
    const nlohmann::json& account,
    const std::optional<bool>& configured,
    const std::string& mode) {
    auto snap = describe_account_snapshot(account, configured);
    snap.mode = mode;
    return snap;
}

} // namespace channels
} // namespace hunterclaw
