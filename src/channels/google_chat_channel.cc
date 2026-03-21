// ============================================================================
// Google Chat Channel Plugin — HunterClaw
// ============================================================================
//
// Implements make_google_chat_plugin() which returns a ChannelPlugin descriptor
// aligned with the HunterClaw ChannelPlugin architecture.
//
// Google Chat is a HunterClaw-specific channel (not in OpenClaw).
// Architecture follows the same pattern as discord/slack/telegram plugins.

#include "hunterclaw/channels/plugin.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>

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

// Resolve google_chat account config: merge top-level + per-account
static nlohmann::json resolve_google_chat_account(const core::Config& cfg, const std::string& account_id) {
    const auto& j = cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("googleChat")) {
        // Also try google_chat key
        if (!j.contains("channels") || !j["channels"].contains("google_chat")) {
            return nlohmann::json::object();
        }
    }
    const auto& gc = j["channels"].contains("googleChat") ? j["channels"]["googleChat"] : j["channels"]["google_chat"];
    nlohmann::json base = gc;
    if (base.contains("accounts")) base.erase("accounts");

    if (gc.contains("accounts") && gc["accounts"].is_object()) {
        std::string aid = account_id.empty() ? "default" : account_id;
        if (gc["accounts"].contains(aid) && gc["accounts"][aid].is_object()) {
            for (auto it = gc["accounts"][aid].begin(); it != gc["accounts"][aid].end(); ++it) {
                base[it.key()] = it.value();
            }
        }
    }
    return base;
}

// Resolve service account key JSON from account config or env
static std::string resolve_google_chat_service_account(const nlohmann::json& account) {
    if (account.contains("serviceAccountKey") && account["serviceAccountKey"].is_string()) {
        return account["serviceAccountKey"].get<std::string>();
    }
    if (account.contains("service_account_key") && account["service_account_key"].is_string()) {
        return account["service_account_key"].get<std::string>();
    }
    const char* env = std::getenv("GOOGLE_CHAT_SERVICE_ACCOUNT_KEY");
    if (env && *env) return env;
    return {};
}

// ---------------------------------------------------------------------------
// Outbound: send message via Google Chat API
// ---------------------------------------------------------------------------

static core::Result<OutboundDeliveryResult> send_google_chat_message(
    const core::Config& cfg,
    const std::string& to,
    const std::string& text,
    const std::optional<std::string>& media_url,
    const std::string& account_id
) {
    // Google Chat requires OAuth2 service account authentication.
    // For now, check if a webhook URL is configured (simpler path).
    const auto account = resolve_google_chat_account(cfg, account_id);

    // Path 1: Webhook URL (simplest)
    std::string webhook_url;
    if (account.contains("webhookUrl") && account["webhookUrl"].is_string()) {
        webhook_url = account["webhookUrl"].get<std::string>();
    } else if (account.contains("webhook_url") && account["webhook_url"].is_string()) {
        webhook_url = account["webhook_url"].get<std::string>();
    }
    const char* env_webhook = std::getenv("GOOGLE_CHAT_WEBHOOK_URL");
    if (webhook_url.empty() && env_webhook && *env_webhook) webhook_url = env_webhook;

    if (!webhook_url.empty()) {
        // Use webhook: POST to webhook URL
        // Parse host and path from webhook_url
        std::string host = "chat.googleapis.com";
        std::string path = webhook_url;
        // Simple URL parsing: https://chat.googleapis.com/v1/spaces/.../messages?key=...
        const std::string prefix = "https://chat.googleapis.com";
        if (webhook_url.rfind(prefix, 0) == 0) {
            path = webhook_url.substr(prefix.size());
        }

        httplib::Client cli("https://chat.googleapis.com");
        cli.set_connection_timeout(10);
        cli.set_read_timeout(30);

        nlohmann::json body;
        if (media_url.has_value()) {
            // Send card with image
            body["cardsV2"] = nlohmann::json::array();
            body["cardsV2"].push_back({
                {"cardId", "media-card"},
                {"card", {
                    {"sections", nlohmann::json::array({
                        {{"widgets", nlohmann::json::array({
                            {{"image", {{"imageUrl", media_url.value()}, {"altText", text}}}}
                        })}}
                    })}
                }}
            });
            if (!text.empty()) body["text"] = text;
        } else {
            body["text"] = text;
        }

        auto res = cli.Post(path, body.dump(), "application/json");
        if (!res || res->status < 200 || res->status >= 300) {
            std::string err = res ? ("Google Chat webhook failed: " + res->body) : "connection failed";
            return core::Result<OutboundDeliveryResult>::Err(err);
        }

        OutboundDeliveryResult result;
        result.channel = "google_chat";
        try {
            auto resp = nlohmann::json::parse(res->body);
            if (resp.contains("name")) result.message_id = resp["name"].get<std::string>();
        } catch (...) {}
        return core::Result<OutboundDeliveryResult>::Ok(result);
    }

    // Path 2: Service Account (requires OAuth2 JWT flow)
    const std::string sa_key = resolve_google_chat_service_account(account);
    if (sa_key.empty()) {
        return core::Result<OutboundDeliveryResult>::Err("Google Chat: no webhookUrl or serviceAccountKey configured");
    }

    // For service account path, 'to' should be a space name: "spaces/{space_id}"
    // Full OAuth2 JWT flow is complex; return a clear error for now
    return core::Result<OutboundDeliveryResult>::Err(
        "Google Chat service account auth not yet implemented. Please configure webhookUrl instead."
    );
}

} // namespace

// ---------------------------------------------------------------------------
// make_google_chat_plugin() — Google Chat ChannelPlugin descriptor
// ---------------------------------------------------------------------------

ChannelPlugin make_google_chat_plugin() {
    ChannelPlugin p;
    p.id = "google_chat";

    // ── meta ─────────────────────────────────────────────────────────────────
    p.meta.id              = "google_chat";
    p.meta.label           = "Google Chat";
    p.meta.selection_label = "Google Chat (Webhook / Bot)";
    p.meta.detail_label    = "Google Chat Bot";
    p.meta.docs_path       = "/channels/google-chat";
    p.meta.docs_label      = "google-chat";
    p.meta.blurb           = "Google Workspace Chat platform.";
    p.meta.system_image    = "message";
    p.meta.order           = 21;

    // ── capabilities ─────────────────────────────────────────────────────────
    p.capabilities.chat_types      = {"direct", "group", "thread"};
    p.capabilities.reactions       = false;
    p.capabilities.media           = true;
    p.capabilities.native_commands = false;
    p.capabilities.threads         = true;

    // ── defaults ─────────────────────────────────────────────────────────────
    ChannelPlugin::Defaults defaults;
    defaults.queue = ChannelPlugin::Defaults::Queue{500};
    p.defaults = defaults;

    // ── reload ────────────────────────────────────────────────────────────────
    ChannelPlugin::ReloadConfig reload;
    reload.config_prefixes = {"channels.googleChat", "channels.google_chat"};
    p.reload = reload;

    // ── outbound ─────────────────────────────────────────────────────────────
    ChannelOutboundAdapter outbound;
    outbound.delivery_mode    = "direct";
    outbound.chunker          = nullptr;
    outbound.text_chunk_limit = 4096;

    outbound.send_text = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        return send_google_chat_message(ctx.cfg, ctx.to, ctx.text, std::nullopt, ctx.account_id.value_or("default"));
    };
    outbound.send_media = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        return send_google_chat_message(ctx.cfg, ctx.to, ctx.text, ctx.media_url, ctx.account_id.value_or("default"));
    };
    outbound.send_payload = [](const ChannelOutboundPayloadContext& ctx) -> core::Result<OutboundDeliveryResult> {
        const std::string text = ctx.payload.value("text", std::string{});
        std::optional<std::string> media_url;
        if (ctx.payload.contains("mediaUrl") && ctx.payload["mediaUrl"].is_string()) {
            media_url = ctx.payload["mediaUrl"].get<std::string>();
        }
        return send_google_chat_message(ctx.cfg, ctx.to, text, media_url, ctx.account_id.value_or("default"));
    };
    p.outbound = outbound;

    // ── config ────────────────────────────────────────────────────────────────
    auto config = std::make_shared<ChannelConfigAdapter>();

    config->list_account_ids = [](const core::Config& cfg) -> std::vector<std::string> {
        const auto& j = cfg.data;
        if (!j.contains("channels")) return {};
        const nlohmann::json* gc_ptr = nullptr;
        if (j["channels"].contains("googleChat")) gc_ptr = &j["channels"]["googleChat"];
        else if (j["channels"].contains("google_chat")) gc_ptr = &j["channels"]["google_chat"];
        if (!gc_ptr) return {};
        const auto& gc = *gc_ptr;
        if (gc.contains("accounts") && gc["accounts"].is_object()) {
            std::vector<std::string> ids;
            for (auto it = gc["accounts"].begin(); it != gc["accounts"].end(); ++it) {
                ids.push_back(it.key());
            }
            return ids;
        }
        if (gc.contains("webhookUrl") || gc.contains("serviceAccountKey")) return {"default"};
        const char* env = std::getenv("GOOGLE_CHAT_WEBHOOK_URL");
        if (env && *env) return {"default"};
        return {};
    };

    config->resolve_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        return resolve_google_chat_account(cfg, account_id);
    };

    config->inspect_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        const auto account = resolve_google_chat_account(cfg, account_id);
        nlohmann::json result = account;
        if (result.contains("serviceAccountKey")) result["serviceAccountKey"] = "***";
        return result;
    };

    config->default_account_id = [](const core::Config& cfg) -> std::string {
        return "default";
    };

    config->is_configured = [](const nlohmann::json& account, const core::Config& cfg) -> bool {
        if (account.contains("webhookUrl") && account["webhookUrl"].is_string() && !account["webhookUrl"].get<std::string>().empty()) return true;
        if (account.contains("serviceAccountKey") && account["serviceAccountKey"].is_string() && !account["serviceAccountKey"].get<std::string>().empty()) return true;
        const char* env = std::getenv("GOOGLE_CHAT_WEBHOOK_URL");
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
        snap.configured = (account.contains("webhookUrl") && account["webhookUrl"].is_string() && !account["webhookUrl"].get<std::string>().empty()) ||
                          (account.contains("serviceAccountKey") && account["serviceAccountKey"].is_string() && !account["serviceAccountKey"].get<std::string>().empty());
        snap.enabled = account.contains("enabled") ? std::optional<bool>(account["enabled"].get<bool>()) : std::optional<bool>(true);
        return snap;
    };

    config->resolve_allow_from = [](const core::Config& cfg, const std::string& account_id) -> std::vector<std::string> {
        const auto account = resolve_google_chat_account(cfg, account_id);
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
        const auto account = resolve_google_chat_account(cfg, account_id);
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
        if (!j["channels"].contains("googleChat")) j["channels"]["googleChat"] = nlohmann::json::object();
        auto& gc = j["channels"]["googleChat"];
        if (!gc.contains("accounts")) gc["accounts"] = nlohmann::json::object();
        auto& acc = gc["accounts"][account_id];
        if (input.webhook_url.has_value()) acc["webhookUrl"] = *input.webhook_url;
        if (input.name.has_value()) acc["name"] = *input.name;
        return new_cfg;
    };
    p.setup = setup;

    // ── security ──────────────────────────────────────────────────────────────
    ChannelSecurityAdapter security;
    security.resolve_dm_policy = [](const core::Config& cfg, const std::string& account_id, const nlohmann::json& /*account*/) -> std::optional<ChannelSecurityDmPolicy> {
        const auto account = resolve_google_chat_account(cfg, account_id);
        std::string policy;
        if (account.contains("dm") && account["dm"].contains("policy") && account["dm"]["policy"].is_string()) {
            policy = account["dm"]["policy"].get<std::string>();
        }
        if (policy.empty()) return std::nullopt;
        ChannelSecurityDmPolicy dm_policy;
        dm_policy.policy = policy;
        dm_policy.policy_path = "channels.googleChat.dm.policy";
        dm_policy.allow_from_path = "channels.googleChat.dm.allowFrom";
        dm_policy.approve_hint = "Add the Google Chat user ID to channels.googleChat.dm.allowFrom";
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
        const auto account = resolve_google_chat_account(cfg, account_id);
        // Check webhook URL is configured
        std::string webhook_url;
        if (account.contains("webhookUrl") && account["webhookUrl"].is_string()) {
            webhook_url = account["webhookUrl"].get<std::string>();
        }
        const char* env = std::getenv("GOOGLE_CHAT_WEBHOOK_URL");
        if (webhook_url.empty() && env && *env) webhook_url = env;
        if (webhook_url.empty()) {
            const std::string sa = resolve_google_chat_service_account(account);
            if (sa.empty()) {
                return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({false, "no webhookUrl or serviceAccountKey configured"});
            }
        }
        return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({true, "ok"});
    };
    heartbeat.resolve_recipients = [](const core::Config& cfg, const std::string& account_id, bool /*all*/) -> ChannelHeartbeatAdapter::RecipientsResult {
        const auto account = resolve_google_chat_account(cfg, account_id);
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
        // Google Chat bot identity is tied to the service account; not easily queryable via webhook
        return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
    };
    directory.list_groups = [](const core::Config& cfg, const std::string& account_id, const std::string& query, int limit) -> core::Result<std::vector<ChannelDirectoryEntry>> {
        const auto& j = cfg.data;
        std::vector<ChannelDirectoryEntry> entries;
        const auto account = resolve_google_chat_account(cfg, account_id);
        if (account.contains("spaces") && account["spaces"].is_object()) {
            for (auto it = account["spaces"].begin(); it != account["spaces"].end(); ++it) {
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
                    {"channel", "google_chat"},
                    {"accountId", snap.account_id},
                    {"issue", "not_configured"},
                    {"message", "Google Chat webhookUrl or serviceAccountKey not configured"}
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
        if (j.contains("channels")) {
            const nlohmann::json* gc_ptr = nullptr;
            if (j["channels"].contains("googleChat")) gc_ptr = &j["channels"]["googleChat"];
            else if (j["channels"].contains("google_chat")) gc_ptr = &j["channels"]["google_chat"];
            if (gc_ptr) {
                const auto& gc = *gc_ptr;
                if ((gc.contains("webhookUrl") && gc["webhookUrl"].is_string() && !gc["webhookUrl"].get<std::string>().empty()) ||
                    (gc.contains("serviceAccountKey") && gc["serviceAccountKey"].is_string() && !gc["serviceAccountKey"].get<std::string>().empty())) {
                    has_config = true;
                }
            }
        }
        if (!has_config) {
            const char* env = std::getenv("GOOGLE_CHAT_WEBHOOK_URL");
            if (env && *env) has_config = true;
        }
        if (!has_config) return {};
        return {"send"};
    };
    actions.supports_buttons = [](const core::Config& /*cfg*/) -> bool { return false; };
    actions.supports_cards   = [](const core::Config& /*cfg*/) -> bool { return true; };
    actions.handle_action = [](const ChannelMessageActionContext& ctx) -> core::Result<nlohmann::json> {
        if (ctx.action == "send") {
            const std::string to = ctx.params.value("to", std::string{});
            const std::string message = ctx.params.value("message", ctx.params.value("content", std::string{}));
            if (to.empty()) return core::Result<nlohmann::json>::Err("'to' is required for send action");
            auto result = send_google_chat_message(ctx.cfg, to, message, std::nullopt, ctx.account_id.value_or("default"));
            if (!result.is_ok()) return core::Result<nlohmann::json>::Err(result.error);
            return core::Result<nlohmann::json>::Ok(nlohmann::json{{"ok", true}, {"messageId", result.value.message_id}});
        }
        return core::Result<nlohmann::json>::Err("Action '" + ctx.action + "' not yet implemented for Google Chat");
    };
    p.actions = actions;

    return p;
}

// Static registration — aligned with discord/telegram/slack pattern
static const ChannelPlugin GOOGLE_CHAT_PLUGIN = make_google_chat_plugin();

} // namespace

static struct GoogleChatPluginRegistrar {
    GoogleChatPluginRegistrar() {
        ChannelPluginRegistry::instance().register_plugin(GOOGLE_CHAT_PLUGIN, 0);
    }
} _google_chat_registrar;

} // namespace channels
} // namespace hunterclaw
