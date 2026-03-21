// ============================================================================
// Feishu (飞书/Lark) Channel Plugin — HunterClaw
// ============================================================================
//
// Implements make_feishu_plugin() which returns a ChannelPlugin descriptor
// aligned with the HunterClaw ChannelPlugin architecture.
//
// Feishu is a HunterClaw-specific channel (not in OpenClaw).
// Architecture follows the same pattern as discord/slack/telegram plugins.

#include "hunterclaw/channels/plugin.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <chrono>

namespace hunterclaw {
namespace channels {

namespace {

auto get_logger() {
    static auto logger = []() {
        auto log = spdlog::get("channels");
        if (!log) log = spdlog::stdout_logger_mt("channels");
        return log;
    }();
    return logger;
}

// ---------------------------------------------------------------------------
// Config helpers
// ---------------------------------------------------------------------------

// Resolve feishu account config: merge top-level + per-account
static nlohmann::json resolve_feishu_account(const core::Config& cfg, const std::string& account_id) {
    const auto& j = cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("feishu")) {
        return nlohmann::json::object();
    }
    const auto& fs = j["channels"]["feishu"];
    nlohmann::json base = fs;
    // Remove accounts sub-object from base
    if (base.contains("accounts")) base.erase("accounts");

    if (fs.contains("accounts") && fs["accounts"].is_object()) {
        std::string aid = account_id.empty() ? "default" : account_id;
        if (fs["accounts"].contains(aid) && fs["accounts"][aid].is_object()) {
            // Merge: per-account overrides base
            for (auto it = fs["accounts"][aid].begin(); it != fs["accounts"][aid].end(); ++it) {
                base[it.key()] = it.value();
            }
        }
    }
    return base;
}

// Resolve app_id from account config or env
static std::string resolve_feishu_app_id(const nlohmann::json& account, const core::Config& cfg, const std::string& account_id) {
    if (account.contains("appId") && account["appId"].is_string()) return account["appId"].get<std::string>();
    if (account.contains("app_id") && account["app_id"].is_string()) return account["app_id"].get<std::string>();
    const char* env = std::getenv("FEISHU_APP_ID");
    if (env && *env) return env;
    return {};
}

// Resolve app_secret from account config or env
static std::string resolve_feishu_app_secret(const nlohmann::json& account, const core::Config& cfg, const std::string& account_id) {
    if (account.contains("appSecret") && account["appSecret"].is_string()) return account["appSecret"].get<std::string>();
    if (account.contains("app_secret") && account["app_secret"].is_string()) return account["app_secret"].get<std::string>();
    const char* env = std::getenv("FEISHU_APP_SECRET");
    if (env && *env) return env;
    return {};
}

// ---------------------------------------------------------------------------
// Outbound: send message via Feishu IM API
// ---------------------------------------------------------------------------

static core::Result<OutboundDeliveryResult> send_feishu_message(
    const core::Config& cfg,
    const std::string& to,
    const std::string& text,
    const std::optional<std::string>& media_url,
    const std::string& account_id
) {
    const auto account = resolve_feishu_account(cfg, account_id);
    const std::string app_id = resolve_feishu_app_id(account, cfg, account_id);
    const std::string app_secret = resolve_feishu_app_secret(account, cfg, account_id);

    if (app_id.empty() || app_secret.empty()) {
        return core::Result<OutboundDeliveryResult>::Err("Feishu appId or appSecret not configured");
    }

    // Step 1: Get tenant_access_token
    httplib::Client auth_cli("https://open.feishu.cn");
    auth_cli.set_connection_timeout(8);
    auth_cli.set_read_timeout(8);
    nlohmann::json token_body = {{"app_id", app_id}, {"app_secret", app_secret}};
    auto token_res = auth_cli.Post("/open-apis/auth/v3/tenant_access_token/internal",
                                   token_body.dump(), "application/json");
    if (!token_res || token_res->status != 200) {
        return core::Result<OutboundDeliveryResult>::Err("Feishu auth failed");
    }
    std::string access_token;
    try {
        auto tr = nlohmann::json::parse(token_res->body);
        if (tr.value("code", -1) != 0) {
            return core::Result<OutboundDeliveryResult>::Err("Feishu auth error: " + tr.value("msg", "unknown"));
        }
        access_token = tr.value("tenant_access_token", std::string{});
    } catch (...) {
        return core::Result<OutboundDeliveryResult>::Err("Feishu auth response parse error");
    }

    // Step 2: Send message
    httplib::Client msg_cli("https://open.feishu.cn");
    msg_cli.set_connection_timeout(10);
    msg_cli.set_read_timeout(30);
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token}};

    nlohmann::json msg_body;
    msg_body["receive_id"] = to;

    if (media_url.has_value()) {
        // Send image card
        msg_body["msg_type"] = "interactive";
        nlohmann::json card = {
            {"elements", nlohmann::json::array({
                {{"tag", "img"}, {"img_key", media_url.value()}, {"alt", {{"tag", "plain_text"}, {"content", text}}}}
            })}
        };
        msg_body["content"] = card.dump();
    } else {
        msg_body["msg_type"] = "text";
        msg_body["content"] = nlohmann::json{{"text", text}}.dump();
    }

    // Determine receive_id_type: chat_id (oc_xxx) or open_id (ou_xxx) or user_id
    std::string receive_id_type = "chat_id";
    if (to.rfind("ou_", 0) == 0) receive_id_type = "open_id";
    else if (to.rfind("on_", 0) == 0) receive_id_type = "union_id";

    std::string url = "/open-apis/im/v1/messages?receive_id_type=" + receive_id_type;
    auto res = msg_cli.Post(url, headers, msg_body.dump(), "application/json");
    if (!res || res->status != 200) {
        std::string err = res ? ("Feishu sendMessage failed: " + res->body) : "connection failed";
        return core::Result<OutboundDeliveryResult>::Err(err);
    }

    OutboundDeliveryResult result;
    result.channel = "feishu";
    try {
        auto resp = nlohmann::json::parse(res->body);
        if (resp.value("code", -1) != 0) {
            return core::Result<OutboundDeliveryResult>::Err("Feishu sendMessage error: " + resp.value("msg", "unknown"));
        }
        if (resp.contains("data") && resp["data"].contains("message_id")) {
            result.message_id = resp["data"]["message_id"].get<std::string>();
        }
    } catch (...) {}
    return core::Result<OutboundDeliveryResult>::Ok(result);
}

} // namespace

// ---------------------------------------------------------------------------
// make_feishu_plugin() — Feishu ChannelPlugin descriptor
// ---------------------------------------------------------------------------

ChannelPlugin make_feishu_plugin() {
    ChannelPlugin p;
    p.id = "feishu";

    // ── meta ─────────────────────────────────────────────────────────────────
    p.meta.id              = "feishu";
    p.meta.label           = "Feishu";
    p.meta.selection_label = "Feishu / Lark (Bot)";
    p.meta.detail_label    = "Feishu Bot";
    p.meta.docs_path       = "/channels/feishu";
    p.meta.docs_label      = "feishu";
    p.meta.blurb           = "Feishu (飞书) / Lark enterprise messaging platform.";
    p.meta.system_image    = "message";
    p.meta.order           = 20;

    // ── capabilities ─────────────────────────────────────────────────────────
    p.capabilities.chat_types      = {"direct", "group"};
    p.capabilities.reactions       = false;
    p.capabilities.media           = true;
    p.capabilities.native_commands = false;
    p.capabilities.threads         = false;

    // ── defaults ─────────────────────────────────────────────────────────────
    ChannelPlugin::Defaults defaults;
    defaults.queue = ChannelPlugin::Defaults::Queue{500};
    p.defaults = defaults;

    // ── reload ────────────────────────────────────────────────────────────────
    ChannelPlugin::ReloadConfig reload;
    reload.config_prefixes = {"channels.feishu"};
    p.reload = reload;

    // ── outbound ─────────────────────────────────────────────────────────────
    ChannelOutboundAdapter outbound;
    outbound.delivery_mode    = "direct";
    outbound.chunker          = nullptr;
    outbound.text_chunk_limit = 4000;

    outbound.send_text = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        return send_feishu_message(ctx.cfg, ctx.to, ctx.text, std::nullopt, ctx.account_id.value_or("default"));
    };
    outbound.send_media = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        return send_feishu_message(ctx.cfg, ctx.to, ctx.text, ctx.media_url, ctx.account_id.value_or("default"));
    };
    outbound.send_payload = [](const ChannelOutboundPayloadContext& ctx) -> core::Result<OutboundDeliveryResult> {
        const std::string text = ctx.payload.value("text", std::string{});
        std::optional<std::string> media_url;
        if (ctx.payload.contains("mediaUrl") && ctx.payload["mediaUrl"].is_string()) {
            media_url = ctx.payload["mediaUrl"].get<std::string>();
        }
        return send_feishu_message(ctx.cfg, ctx.to, text, media_url, ctx.account_id.value_or("default"));
    };
    p.outbound = outbound;

    // ── config ────────────────────────────────────────────────────────────────
    auto config = std::make_shared<ChannelConfigAdapter>();

    config->list_account_ids = [](const core::Config& cfg) -> std::vector<std::string> {
        const auto& j = cfg.data;
        if (!j.contains("channels") || !j["channels"].contains("feishu")) return {};
        const auto& fs = j["channels"]["feishu"];
        if (fs.contains("accounts") && fs["accounts"].is_object()) {
            std::vector<std::string> ids;
            for (auto it = fs["accounts"].begin(); it != fs["accounts"].end(); ++it) {
                ids.push_back(it.key());
            }
            return ids;
        }
        // Single account
        if (fs.contains("appId") || fs.contains("app_id")) return {"default"};
        const char* env = std::getenv("FEISHU_APP_ID");
        if (env && *env) return {"default"};
        return {};
    };

    config->resolve_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        return resolve_feishu_account(cfg, account_id);
    };

    config->inspect_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        const auto account = resolve_feishu_account(cfg, account_id);
        const std::string app_id = resolve_feishu_app_id(account, cfg, account_id);
        const std::string app_secret = resolve_feishu_app_secret(account, cfg, account_id);
        nlohmann::json result = account;
        result["appId"] = app_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(app_id);
        result["appSecret"] = app_secret.empty() ? nlohmann::json(nullptr) : nlohmann::json("***");
        return result;
    };

    config->default_account_id = [](const core::Config& cfg) -> std::string {
        const auto& j = cfg.data;
        if (!j.contains("channels") || !j["channels"].contains("feishu")) return "default";
        const auto& fs = j["channels"]["feishu"];
        if (fs.contains("accounts") && fs["accounts"].is_object() && !fs["accounts"].empty()) {
            return fs["accounts"].begin().key();
        }
        return "default";
    };

    config->is_configured = [](const nlohmann::json& account, const core::Config& cfg) -> bool {
        if (account.contains("appId") && account["appId"].is_string() && !account["appId"].get<std::string>().empty()) return true;
        if (account.contains("app_id") && account["app_id"].is_string() && !account["app_id"].get<std::string>().empty()) return true;
        const char* env = std::getenv("FEISHU_APP_ID");
        return env && *env;
    };

    config->is_enabled = [](const nlohmann::json& account, const core::Config& cfg) -> bool {
        if (account.contains("enabled") && account["enabled"].is_boolean()) return account["enabled"].get<bool>();
        return true;
    };

    config->describe_account = [](const nlohmann::json& account, const core::Config& cfg) -> ChannelAccountSnapshot {
        ChannelAccountSnapshot snap;
        snap.account_id = account.value("accountId", std::string{"default"});
        snap.name = account.contains("name") && account["name"].is_string()
            ? std::optional<std::string>(account["name"].get<std::string>()) : std::nullopt;
        snap.configured = account.contains("appId") && account["appId"].is_string() && !account["appId"].get<std::string>().empty();
        snap.enabled = account.contains("enabled") ? std::optional<bool>(account["enabled"].get<bool>()) : std::optional<bool>(true);
        return snap;
    };

    config->resolve_allow_from = [](const core::Config& cfg, const std::string& account_id) -> std::vector<std::string> {
        const auto account = resolve_feishu_account(cfg, account_id);
        if (account.contains("allowFrom") && account["allowFrom"].is_array()) {
            std::vector<std::string> result;
            for (const auto& e : account["allowFrom"]) {
                if (e.is_string()) result.push_back(e.get<std::string>());
            }
            return result;
        }
        return {};
    };

    config->resolve_default_to = [](const core::Config& cfg, const std::string& account_id) -> std::optional<std::string> {
        const auto account = resolve_feishu_account(cfg, account_id);
        if (account.contains("defaultTo") && account["defaultTo"].is_string()) {
            return account["defaultTo"].get<std::string>();
        }
        return std::nullopt;
    };

    p.config = config;

    // ── setup ─────────────────────────────────────────────────────────────────
    ChannelSetupAdapter setup;
    setup.resolve_account_id = [](const core::Config& cfg, const std::string& account_id, const ChannelSetupInput& input) -> std::string {
        if (!account_id.empty() && account_id != "default") return account_id;
        if (input.name.has_value() && !input.name->empty()) return *input.name;
        return "default";
    };
    setup.apply_account_config = [](const core::Config& cfg, const std::string& account_id, const ChannelSetupInput& input) -> core::Config {
        core::Config new_cfg = cfg;
        auto& j = new_cfg.data;
        if (!j.contains("channels")) j["channels"] = nlohmann::json::object();
        if (!j["channels"].contains("feishu")) j["channels"]["feishu"] = nlohmann::json::object();
        auto& fs = j["channels"]["feishu"];
        if (!fs.contains("accounts")) fs["accounts"] = nlohmann::json::object();
        auto& acc = fs["accounts"][account_id];
        if (input.token.has_value()) acc["appId"] = *input.token;
        if (input.bot_token.has_value()) acc["appSecret"] = *input.bot_token;
        if (input.name.has_value()) acc["name"] = *input.name;
        return new_cfg;
    };
    p.setup = setup;

    // ── security ──────────────────────────────────────────────────────────────
    ChannelSecurityAdapter security;
    security.resolve_dm_policy = [](const core::Config& cfg, const std::string& account_id, const nlohmann::json& /*account*/) -> std::optional<ChannelSecurityDmPolicy> {
        const auto account = resolve_feishu_account(cfg, account_id);
        std::string policy;
        if (account.contains("dm") && account["dm"].contains("policy") && account["dm"]["policy"].is_string()) {
            policy = account["dm"]["policy"].get<std::string>();
        } else if (account.contains("dmPolicy") && account["dmPolicy"].is_string()) {
            policy = account["dmPolicy"].get<std::string>();
        }
        if (policy.empty()) return std::nullopt;
        ChannelSecurityDmPolicy dm_policy;
        dm_policy.policy = policy;
        dm_policy.policy_path = "channels.feishu.dm.policy";
        dm_policy.allow_from_path = "channels.feishu.dm.allowFrom";
        dm_policy.approve_hint = "Add the Feishu open_id to channels.feishu.dm.allowFrom";
        dm_policy.normalize_entry = [](const std::string& raw) -> std::string {
            std::string s = raw;
            while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
            while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
            return s;
        };
        return dm_policy;
    };
    security.collect_warnings = [](const core::Config& /*cfg*/, const std::string& /*account_id*/, const nlohmann::json& /*account*/) -> std::vector<std::string> {
        return {};
    };
    p.security = security;

    // ── heartbeat ─────────────────────────────────────────────────────────────
    ChannelHeartbeatAdapter heartbeat;
    heartbeat.check_ready = [](const core::Config& cfg, const std::string& account_id) -> core::Result<ChannelHeartbeatAdapter::ReadyResult> {
        const auto account = resolve_feishu_account(cfg, account_id);
        const std::string app_id = resolve_feishu_app_id(account, cfg, account_id);
        const std::string app_secret = resolve_feishu_app_secret(account, cfg, account_id);
        if (app_id.empty() || app_secret.empty()) {
            return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({false, "no appId or appSecret configured"});
        }
        httplib::Client cli("https://open.feishu.cn");
        cli.set_connection_timeout(8);
        cli.set_read_timeout(8);
        nlohmann::json body = {{"app_id", app_id}, {"app_secret", app_secret}};
        auto res = cli.Post("/open-apis/auth/v3/tenant_access_token/internal", body.dump(), "application/json");
        if (!res || res->status != 200) {
            return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({false, "Feishu auth failed"});
        }
        try {
            auto resp = nlohmann::json::parse(res->body);
            if (resp.value("code", -1) != 0) {
                return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({false, "Feishu auth error: " + resp.value("msg", "unknown")});
            }
        } catch (...) {}
        return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({true, "ok"});
    };
    heartbeat.resolve_recipients = [](const core::Config& cfg, const std::string& account_id, bool /*all*/) -> ChannelHeartbeatAdapter::RecipientsResult {
        const auto account = resolve_feishu_account(cfg, account_id);
        if (account.contains("defaultTo") && account["defaultTo"].is_string()) {
            std::string v = account["defaultTo"].get<std::string>();
            while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
            while (!v.empty() && std::isspace((unsigned char)v.back()))  v.pop_back();
            if (!v.empty()) return {{v}, "config.defaultTo"};
        }
        return {{}, "none"};
    };
    p.heartbeat = heartbeat;

    // ── directory ─────────────────────────────────────────────────────────────
    ChannelDirectoryAdapter directory;
    directory.self = [](const core::Config& cfg, const std::string& account_id) -> core::Result<std::optional<ChannelDirectoryEntry>> {
        // Feishu: get bot info via /bot/v3/info
        const auto account = resolve_feishu_account(cfg, account_id);
        const std::string app_id = resolve_feishu_app_id(account, cfg, account_id);
        const std::string app_secret = resolve_feishu_app_secret(account, cfg, account_id);
        if (app_id.empty() || app_secret.empty()) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
        // Get token first
        httplib::Client auth_cli("https://open.feishu.cn");
        auth_cli.set_connection_timeout(8);
        auth_cli.set_read_timeout(8);
        nlohmann::json token_body = {{"app_id", app_id}, {"app_secret", app_secret}};
        auto token_res = auth_cli.Post("/open-apis/auth/v3/tenant_access_token/internal", token_body.dump(), "application/json");
        if (!token_res || token_res->status != 200) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
        std::string access_token;
        try {
            auto tr = nlohmann::json::parse(token_res->body);
            if (tr.value("code", -1) != 0) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
            access_token = tr.value("tenant_access_token", std::string{});
        } catch (...) { return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt); }
        // Get bot info
        httplib::Client bot_cli("https://open.feishu.cn");
        bot_cli.set_connection_timeout(8);
        bot_cli.set_read_timeout(8);
        httplib::Headers headers = {{"Authorization", "Bearer " + access_token}};
        auto res = bot_cli.Get("/open-apis/bot/v3/info", headers);
        if (!res || res->status != 200) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
        try {
            auto j = nlohmann::json::parse(res->body);
            if (j.value("code", -1) != 0) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
            const auto& bot = j["data"]["bot"];
            ChannelDirectoryEntry entry;
            entry.kind = "user";
            entry.id = bot.value("open_id", std::string{});
            entry.name = bot.value("app_name", std::string{});
            return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(entry);
        } catch (...) {}
        return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
    };
    directory.list_groups = [](const core::Config& cfg, const std::string& account_id, const std::string& query, int limit) -> core::Result<std::vector<ChannelDirectoryEntry>> {
        const auto& j = cfg.data;
        std::vector<ChannelDirectoryEntry> entries;
        if (!j.contains("channels") || !j["channels"].contains("feishu")) return core::Result<std::vector<ChannelDirectoryEntry>>::Ok(entries);
        const auto account = resolve_feishu_account(cfg, account_id);
        if (account.contains("groups") && account["groups"].is_object()) {
            for (auto it = account["groups"].begin(); it != account["groups"].end(); ++it) {
                ChannelDirectoryEntry entry;
                entry.kind = "group";
                entry.id = it.key();
                if (it.value().is_object() && it.value().contains("name")) {
                    entry.name = it.value()["name"].get<std::string>();
                }
                if (!query.empty() && entry.name.has_value() && entry.name->find(query) == std::string::npos) continue;
                entries.push_back(entry);
                if (limit > 0 && (int)entries.size() >= limit) break;
            }
        }
        return core::Result<std::vector<ChannelDirectoryEntry>>::Ok(entries);
    };
    p.directory = directory;

    // ── status ────────────────────────────────────────────────────────────────
    ChannelStatusAdapter status;
    status.collect_status_issues = [](const std::vector<ChannelAccountSnapshot>& snapshots) -> std::vector<nlohmann::json> {
        std::vector<nlohmann::json> issues;
        for (const auto& snap : snapshots) {
            if (snap.configured.has_value() && !snap.configured.value()) {
                issues.push_back({
                    {"channel", "feishu"},
                    {"accountId", snap.account_id},
                    {"issue", "not_configured"},
                    {"message", "Feishu appId or appSecret not configured"}
                });
            }
        }
        return issues;
    };
    p.status = status;

    // ── actions ───────────────────────────────────────────────────────────────
    ChannelMessageActionAdapter actions;
    actions.list_actions = [](const core::Config& cfg) -> std::vector<std::string> {
        const auto& j = cfg.data;
        bool has_config = false;
        if (j.contains("channels") && j["channels"].contains("feishu")) {
            const auto& fs = j["channels"]["feishu"];
            if ((fs.contains("appId") && fs["appId"].is_string() && !fs["appId"].get<std::string>().empty()) ||
                (fs.contains("app_id") && fs["app_id"].is_string() && !fs["app_id"].get<std::string>().empty())) {
                has_config = true;
            }
        }
        if (!has_config) {
            const char* env = std::getenv("FEISHU_APP_ID");
            if (env && *env) has_config = true;
        }
        if (!has_config) return {};
        return {"send", "react", "delete", "edit"};
    };
    actions.supports_buttons = [](const core::Config& /*cfg*/) -> bool { return true; };
    actions.supports_cards   = [](const core::Config& /*cfg*/) -> bool { return true; };
    actions.handle_action = [](const ChannelMessageActionContext& ctx) -> core::Result<nlohmann::json> {
        return core::Result<nlohmann::json>::Err("Action '" + ctx.action + "' not yet implemented for Feishu");
    };
    p.actions = actions;

    return p;
}

// Static registration — aligned with discord/telegram/slack pattern
static const ChannelPlugin FEISHU_PLUGIN = make_feishu_plugin();

static struct FeishuPluginRegistrar {
    FeishuPluginRegistrar() {
        ChannelPluginRegistry::instance().register_plugin(FEISHU_PLUGIN, 0);
    }
} _feishu_registrar;

} // namespace channels
} // namespace hunterclaw
