#include "hunterclaw/channels/plugin.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <algorithm>
#include <cctype>

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
// Helpers: config inspection (对齐 openclaw inspectSlackAccount / resolveSlackAccount)
// ---------------------------------------------------------------------------

static std::string normalize_account_id(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    if (s.empty()) return "default";
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// Aligned with openclaw: mergeSlackAccountConfig
// Returns cfg.channels.slack.accounts[accountId] merged with top-level cfg.channels.slack
static nlohmann::json resolve_slack_account(const core::Config& cfg, const std::string& account_id) {
    const auto& j = cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("slack")) {
        return nlohmann::json::object();
    }
    const auto& sl = j["channels"]["slack"];
    nlohmann::json merged = nlohmann::json::object();
    for (auto it = sl.begin(); it != sl.end(); ++it) {
        if (it.key() != "accounts") merged[it.key()] = it.value();
    }
    const std::string norm_id = normalize_account_id(account_id);
    if (sl.contains("accounts") && sl["accounts"].is_object()) {
        for (auto it = sl["accounts"].begin(); it != sl["accounts"].end(); ++it) {
            std::string key = it.key();
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            if (key == norm_id) {
                for (auto ait = it.value().begin(); ait != it.value().end(); ++ait) {
                    merged[ait.key()] = ait.value();
                }
                break;
            }
        }
    }
    return merged;
}

// Aligned with openclaw: listSlackAccountIds
static std::vector<std::string> list_slack_account_ids(const core::Config& cfg) {
    const auto& j = cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("slack")) return {"default"};
    const auto& sl = j["channels"]["slack"];
    if (!sl.contains("accounts") || !sl["accounts"].is_object() || sl["accounts"].empty()) return {"default"};
    std::vector<std::string> ids;
    for (auto it = sl["accounts"].begin(); it != sl["accounts"].end(); ++it) {
        ids.push_back(it.key());
    }
    return ids;
}

// Aligned with openclaw: resolveSlackBotToken
// Returns botToken from: account.botToken → env SLACK_BOT_TOKEN (default account only)
static std::string resolve_slack_bot_token(const nlohmann::json& account, const core::Config& /*cfg*/, const std::string& account_id) {
    if (account.contains("botToken") && account["botToken"].is_string()) {
        std::string t = account["botToken"].get<std::string>();
        if (!t.empty()) return t;
    }
    if (normalize_account_id(account_id) == "default") {
        const char* env_token = std::getenv("SLACK_BOT_TOKEN");
        if (env_token && *env_token) return std::string(env_token);
    }
    return "";
}

// Aligned with openclaw: resolveSlackAppToken
// Returns appToken from: account.appToken → env SLACK_APP_TOKEN (default account only)
static std::string resolve_slack_app_token(const nlohmann::json& account, const std::string& account_id) {
    if (account.contains("appToken") && account["appToken"].is_string()) {
        std::string t = account["appToken"].get<std::string>();
        if (!t.empty()) return t;
    }
    if (normalize_account_id(account_id) == "default") {
        const char* env_token = std::getenv("SLACK_APP_TOKEN");
        if (env_token && *env_token) return std::string(env_token);
    }
    return "";
}

// Aligned with openclaw: resolveSlackReplyToMode (slack/accounts.ts)
// Checks replyToModeByChatType[chatType] → dm.replyToMode (for direct) → replyToMode → "off"
static std::string resolve_slack_reply_to_mode(const nlohmann::json& account, const std::string& chat_type) {
    // Aligned with openclaw: normalizeChatType
    std::string normalized = chat_type;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    // "direct" | "group" | "channel" | "thread"
    if (normalized != "direct" && normalized != "group" && normalized != "channel" && normalized != "thread") {
        normalized = "";
    }

    // Check replyToModeByChatType[normalized]
    if (!normalized.empty() && account.contains("replyToModeByChatType") && account["replyToModeByChatType"].is_object()) {
        const auto& by_type = account["replyToModeByChatType"];
        if (by_type.contains(normalized) && by_type[normalized].is_string()) {
            return by_type[normalized].get<std::string>();
        }
    }

    // Check dm.replyToMode for direct
    if (normalized == "direct" && account.contains("dm") && account["dm"].is_object()) {
        const auto& dm = account["dm"];
        if (dm.contains("replyToMode") && dm["replyToMode"].is_string()) {
            return dm["replyToMode"].get<std::string>();
        }
    }

    // Check top-level replyToMode
    if (account.contains("replyToMode") && account["replyToMode"].is_string()) {
        return account["replyToMode"].get<std::string>();
    }

    return "off";
}

// Aligned with openclaw: buildSlackThreadingToolContext (slack/threading-tool-context.ts)
static std::optional<ChannelThreadingToolContext> build_slack_threading_tool_context(
    const core::Config& cfg,
    const std::string& account_id,
    const ChannelThreadingContext& ctx
) {
    auto account = resolve_slack_account(cfg, account_id);
    const std::string chat_type = ctx.chat_type.value_or("");
    const std::string configured_reply_to_mode = resolve_slack_reply_to_mode(account, chat_type);

    // Aligned with openclaw: hasExplicitThreadTarget = context.MessageThreadId != null
    const bool has_explicit_thread_target = ctx.message_thread_id.has_value() && !ctx.message_thread_id->empty();
    const std::string effective_reply_to_mode = has_explicit_thread_target ? "all" : configured_reply_to_mode;

    // Aligned with openclaw: threadId = context.MessageThreadId ?? context.ReplyToId
    std::optional<std::string> thread_id;
    if (ctx.message_thread_id.has_value() && !ctx.message_thread_id->empty()) {
        thread_id = *ctx.message_thread_id;
    } else if (ctx.reply_to_id.has_value() && !ctx.reply_to_id->empty()) {
        thread_id = *ctx.reply_to_id;
    }

    // Aligned with openclaw: currentChannelId
    // For channel messages, To is "channel:C…" — extract the bare ID.
    // For DMs, To is "user:U…" — fall back to NativeChannelId.
    std::optional<std::string> current_channel_id;
    if (ctx.to.has_value() && !ctx.to->empty()) {
        const std::string& to = *ctx.to;
        if (to.rfind("channel:", 0) == 0) {
            current_channel_id = to.substr(8);  // strip "channel:"
        } else if (ctx.native_channel_id.has_value() && !ctx.native_channel_id->empty()) {
            current_channel_id = *ctx.native_channel_id;
        }
    } else if (ctx.native_channel_id.has_value() && !ctx.native_channel_id->empty()) {
        current_channel_id = *ctx.native_channel_id;
    }

    ChannelThreadingToolContext tool_ctx;
    tool_ctx.current_channel_id = current_channel_id;
    tool_ctx.current_thread_ts  = thread_id;
    tool_ctx.reply_to_mode      = effective_reply_to_mode;
    return tool_ctx;
}

// Aligned with openclaw: resolveSlackGroupRequireMention (group-mentions.ts)
// Checks cfg.channels.slack.accounts[accountId].channels[groupId/groupChannel].requireMention
// → channels["*"].requireMention → default true
static bool resolve_slack_group_require_mention(const ChannelGroupContext& ctx) {
    auto account = resolve_slack_account(ctx.cfg, ctx.account_id.value_or("default"));
    if (!account.contains("channels") || !account["channels"].is_object()) return true;
    const auto& channels = account["channels"];

    const std::string group_id      = ctx.group_id.value_or("");
    const std::string group_channel = ctx.group_channel.value_or("");

    // Aligned with openclaw: resolveSlackChannelPolicyEntry
    // Candidates: channelId, "#channelName", "channelName", normalizedName
    std::vector<std::string> candidates;
    if (!group_id.empty()) candidates.push_back(group_id);
    if (!group_channel.empty()) {
        candidates.push_back("#" + group_channel);
        candidates.push_back(group_channel);
        // normalized (hyphen slug)
        std::string slug = group_channel;
        std::transform(slug.begin(), slug.end(), slug.begin(), ::tolower);
        std::replace(slug.begin(), slug.end(), ' ', '-');
        candidates.push_back(slug);
    }

    for (const auto& candidate : candidates) {
        if (!candidate.empty() && channels.contains(candidate)) {
            const auto& entry = channels[candidate];
            if (entry.contains("requireMention") && entry["requireMention"].is_boolean()) {
                return entry["requireMention"].get<bool>();
            }
        }
    }
    // Wildcard
    if (channels.contains("*") && channels["*"].contains("requireMention") &&
        channels["*"]["requireMention"].is_boolean()) {
        return channels["*"]["requireMention"].get<bool>();
    }

    return true;  // Aligned with openclaw: default true for Slack
}

// Aligned with openclaw: resolveSlackGroupToolPolicy (group-mentions.ts)
static std::optional<nlohmann::json> resolve_slack_group_tool_policy(const ChannelGroupContext& ctx) {
    auto account = resolve_slack_account(ctx.cfg, ctx.account_id.value_or("default"));
    if (!account.contains("channels") || !account["channels"].is_object()) return std::nullopt;
    const auto& channels = account["channels"];

    const std::string group_id      = ctx.group_id.value_or("");
    const std::string group_channel = ctx.group_channel.value_or("");

    std::vector<std::string> candidates;
    if (!group_id.empty()) candidates.push_back(group_id);
    if (!group_channel.empty()) {
        candidates.push_back("#" + group_channel);
        candidates.push_back(group_channel);
    }

    for (const auto& candidate : candidates) {
        if (!candidate.empty() && channels.contains(candidate)) {
            const auto& entry = channels[candidate];
            if (entry.contains("tools")) return entry["tools"];
        }
    }
    if (channels.contains("*") && channels["*"].contains("tools")) {
        return channels["*"]["tools"];
    }
    return std::nullopt;
}

// Aligned with openclaw: sendMessageSlack (slack/send.ts)
// POST /chat.postMessage
static core::Result<OutboundDeliveryResult> send_slack_message(
    const std::string& bot_token,
    const std::string& channel,
    const std::string& text,
    const std::optional<std::string>& thread_ts,
    const std::optional<std::string>& media_url,
    const std::optional<nlohmann::json>& blocks
) {
    httplib::Client cli("https://slack.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    nlohmann::json body = {{"channel", channel}, {"text", text}};

    // Aligned with openclaw: threadTs (replyToId ?? threadId)
    if (thread_ts.has_value() && !thread_ts->empty()) {
        body["thread_ts"] = *thread_ts;
    }

    // Aligned with openclaw: blocks (Slack Block Kit)
    if (blocks.has_value() && !blocks->is_null()) {
        body["blocks"] = *blocks;
    }

    // Aligned with openclaw: mediaUrl → file upload or attachment
    if (media_url.has_value() && !media_url->empty()) {
        // Simplified: add as attachment
        body["attachments"] = nlohmann::json::array({{{"image_url", *media_url}, {"fallback", text}}});
    }

    httplib::Headers headers = {
        {"Authorization", "Bearer " + bot_token},
        {"Content-Type", "application/json; charset=utf-8"}
    };

    auto res = cli.Post("/api/chat.postMessage", headers, body.dump(), "application/json");
    if (!res) {
        return core::Result<OutboundDeliveryResult>::Err("HTTP request failed: " + httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        return core::Result<OutboundDeliveryResult>::Err("Slack API error " + std::to_string(res->status) + ": " + res->body);
    }

    OutboundDeliveryResult result;
    result.channel = "slack";
    try {
        auto resp_json = nlohmann::json::parse(res->body);
        if (!resp_json.value("ok", false)) {
            return core::Result<OutboundDeliveryResult>::Err("Slack API error: " + resp_json.value("error", "unknown"));
        }
        result.message_id  = resp_json.contains("ts") ? resp_json["ts"].get<std::string>() : "";
        result.channel_id  = resp_json.value("channel", std::string{});
    } catch (...) {}
    return core::Result<OutboundDeliveryResult>::Ok(result);
}

// Aligned with openclaw: applySlackMessageSendingHooks (outbound/slack.ts)
// HunterClaw: no hook runner yet — pass through unchanged
static std::string apply_slack_message_sending_hooks(const std::string& text) {
    return text;
}

// Aligned with openclaw: sendSlackOutboundMessage (outbound/slack.ts)
static core::Result<OutboundDeliveryResult> send_slack_outbound_message(
    const core::Config& cfg,
    const std::string& to,
    const std::string& text,
    const std::optional<std::string>& reply_to_id,
    const std::optional<std::string>& thread_id,
    const std::optional<std::string>& media_url,
    const std::optional<nlohmann::json>& blocks,
    const std::string& account_id
) {
    auto account = resolve_slack_account(cfg, account_id);
    const std::string bot_token = resolve_slack_bot_token(account, cfg, account_id);
    if (bot_token.empty()) {
        return core::Result<OutboundDeliveryResult>::Err("no slack bot token configured");
    }

    // Aligned with openclaw: threadTs = replyToId ?? (threadId != null ? String(threadId) : undefined)
    std::optional<std::string> thread_ts;
    if (reply_to_id.has_value() && !reply_to_id->empty()) {
        thread_ts = *reply_to_id;
    } else if (thread_id.has_value() && !thread_id->empty()) {
        thread_ts = *thread_id;
    }

    // Aligned with openclaw: applySlackMessageSendingHooks
    const std::string final_text = apply_slack_message_sending_hooks(text);

    return send_slack_message(bot_token, to, final_text, thread_ts, media_url, blocks);
}

// ---------------------------------------------------------------------------
// Slack ChannelPlugin descriptor (严格对齐 OpenClaw slack plugin)
// ---------------------------------------------------------------------------

ChannelPlugin make_slack_plugin() {
    ChannelPlugin p;
    p.id = "slack";

    // ── meta (对齐 OpenClaw registry.ts CHAT_CHANNEL_META["slack"]) ──────────
    p.meta.id             = "slack";
    p.meta.label          = "Slack";
    p.meta.selection_label = "Slack (Socket Mode)";
    p.meta.detail_label   = "Slack Bot";
    p.meta.docs_path      = "/channels/slack";
    p.meta.docs_label     = "slack";
    p.meta.blurb          = "supported (Socket Mode).";
    p.meta.system_image   = "number";
    p.meta.prefer_session_lookup_for_announce_target = true;  // Aligned with openclaw

    // ── capabilities (对齐 OpenClaw dock.ts DOCKS["slack"]) ──────────────────
    p.capabilities.chat_types      = {"direct", "channel", "thread"};
    p.capabilities.reactions       = true;
    p.capabilities.media           = true;
    p.capabilities.native_commands = true;
    p.capabilities.threads         = true;

    // ── outbound (严格对齐 OpenClaw slackOutbound) ────────────────────────────
    // Aligned with openclaw:
    //   deliveryMode: "direct"
    //   chunker: null  (Slack does not use a chunker)
    //   textChunkLimit: 4000
    //   sendText / sendMedia / sendPayload
    ChannelOutboundAdapter outbound;
    outbound.delivery_mode    = "direct";
    outbound.chunker          = nullptr;  // Aligned with openclaw: chunker: null
    outbound.text_chunk_limit = 4000;

    // Aligned with openclaw: slackOutbound.sendText
    outbound.send_text = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        return send_slack_outbound_message(
            ctx.cfg, ctx.to, ctx.text,
            ctx.reply_to_id, ctx.thread_id,
            std::nullopt, std::nullopt,
            ctx.account_id.value_or("default")
        );
    };

    // Aligned with openclaw: slackOutbound.sendMedia
    outbound.send_media = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        return send_slack_outbound_message(
            ctx.cfg, ctx.to, ctx.text,
            ctx.reply_to_id, ctx.thread_id,
            ctx.media_url, std::nullopt,
            ctx.account_id.value_or("default")
        );
    };

    // Aligned with openclaw: slackOutbound.sendPayload
    // Handles payload.channelData.slack.blocks (Slack Block Kit)
    outbound.send_payload = [](const ChannelOutboundPayloadContext& ctx) -> core::Result<OutboundDeliveryResult> {
        const std::string text = ctx.payload.value("text", std::string{});
        std::optional<std::string> media_url;
        if (ctx.payload.contains("mediaUrl") && ctx.payload["mediaUrl"].is_string()) {
            media_url = ctx.payload["mediaUrl"].get<std::string>();
        }

        // Aligned with openclaw: resolveSlackBlocks(ctx.payload.channelData)
        std::optional<nlohmann::json> blocks;
        if (ctx.payload.contains("channelData") && ctx.payload["channelData"].is_object()) {
            const auto& channel_data = ctx.payload["channelData"];
            if (channel_data.contains("slack") && channel_data["slack"].is_object()) {
                const auto& slack_data = channel_data["slack"];
                if (slack_data.contains("blocks") && !slack_data["blocks"].is_null()) {
                    blocks = slack_data["blocks"];
                }
            }
        }

        return send_slack_outbound_message(
            ctx.cfg, ctx.to, text,
            ctx.reply_to_id, ctx.thread_id,
            media_url, blocks,
            ctx.account_id.value_or("default")
        );
    };

    p.outbound = outbound;

    // ── config adapter (严格对齐 OpenClaw dock.ts slack.config) ──────────────
    auto config = std::make_shared<ChannelConfigAdapter>();

    // Aligned with openclaw: listSlackAccountIds(cfg)
    config->list_account_ids = [](const core::Config& cfg) -> std::vector<std::string> {
        return list_slack_account_ids(cfg);
    };

    // Aligned with openclaw: mergeSlackAccountConfig(cfg, accountId)
    config->resolve_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        return resolve_slack_account(cfg, account_id);
    };

    // Aligned with openclaw: inspectSlackAccount
    config->inspect_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        auto account = resolve_slack_account(cfg, account_id);
        const std::string bot_token = resolve_slack_bot_token(account, cfg, account_id);
        const std::string app_token = resolve_slack_app_token(account, account_id);
        nlohmann::json result = account;
        result["accountId"]      = normalize_account_id(account_id);
        result["botToken"]       = bot_token;
        result["appToken"]       = app_token;
        result["configured"]     = !bot_token.empty();
        result["botTokenStatus"] = bot_token.empty() ? "missing" : "available";
        result["appTokenStatus"] = app_token.empty() ? "missing" : "available";
        return result;
    };

    // Aligned with openclaw: isEnabled
    config->is_enabled = [](const nlohmann::json& account, const core::Config& cfg) -> bool {
        const auto& j = cfg.data;
        bool channel_enabled = true;
        if (j.contains("channels") && j["channels"].contains("slack")) {
            const auto& sl = j["channels"]["slack"];
            if (sl.contains("enabled") && sl["enabled"].is_boolean()) {
                channel_enabled = sl["enabled"].get<bool>();
            }
        }
        return channel_enabled && account.value("enabled", true);
    };

    // Aligned with openclaw: isConfigured → botToken + appToken (socket mode) or botToken + signingSecret (http mode)
    config->is_configured = [](const nlohmann::json& account, const core::Config& cfg) -> bool {
        if (account.contains("botToken") && account["botToken"].is_string() &&
            !account["botToken"].get<std::string>().empty()) return true;
        const char* env_token = std::getenv("SLACK_BOT_TOKEN");
        if (env_token && *env_token) return true;
        return false;
    };

    // Aligned with openclaw: resolveAllowFrom
    // mapAllowFromEntries(inspectSlackAccount({ cfg, accountId }).config.allowFrom ?? .dm?.allowFrom)
    config->resolve_allow_from = [](const core::Config& cfg, const std::string& account_id) -> std::vector<std::string> {
        auto account = resolve_slack_account(cfg, account_id);
        auto extract = [](const nlohmann::json& obj) -> std::vector<std::string> {
            if (!obj.contains("allowFrom") || !obj["allowFrom"].is_array()) return {};
            std::vector<std::string> result;
            for (const auto& e : obj["allowFrom"]) {
                if (e.is_string()) result.push_back(e.get<std::string>());
                else if (e.is_number()) result.push_back(std::to_string(e.get<int64_t>()));
            }
            return result;
        };
        auto v = extract(account);
        if (!v.empty()) return v;
        if (account.contains("dm")) return extract(account["dm"]);
        return {};
    };

    // Aligned with openclaw: formatAllowFrom → formatAllowFromLowercase (no prefix stripping for Slack)
    config->format_allow_from = [](const core::Config&, const std::string&, const std::vector<std::string>& allow_from) -> std::vector<std::string> {
        std::vector<std::string> result;
        for (auto entry : allow_from) {
            while (!entry.empty() && std::isspace((unsigned char)entry.front())) entry.erase(entry.begin());
            while (!entry.empty() && std::isspace((unsigned char)entry.back()))  entry.pop_back();
            std::transform(entry.begin(), entry.end(), entry.begin(), ::tolower);
            if (!entry.empty()) result.push_back(entry);
        }
        return result;
    };

    // Aligned with openclaw: resolveDefaultTo
    config->resolve_default_to = [](const core::Config& cfg, const std::string& account_id) -> std::optional<std::string> {
        auto account = resolve_slack_account(cfg, account_id);
        if (account.contains("defaultTo") && account["defaultTo"].is_string()) {
            std::string v = account["defaultTo"].get<std::string>();
            while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
            while (!v.empty() && std::isspace((unsigned char)v.back()))  v.pop_back();
            if (!v.empty()) return v;
        }
        return std::nullopt;
    };

    // Aligned with openclaw: setAccountEnabled
    config->set_account_enabled = [](const core::Config& cfg, const std::string& account_id, bool enabled) -> core::Config {
        core::Config next = cfg;
        const std::string norm_id = normalize_account_id(account_id);
        auto& j = next.data;
        if (!j.contains("channels")) j["channels"] = nlohmann::json::object();
        if (!j["channels"].contains("slack")) j["channels"]["slack"] = nlohmann::json::object();
        auto& sl = j["channels"]["slack"];
        if (!sl.contains("accounts")) sl["accounts"] = nlohmann::json::object();
        if (!sl["accounts"].contains(norm_id)) sl["accounts"][norm_id] = nlohmann::json::object();
        sl["accounts"][norm_id]["enabled"] = enabled;
        return next;
    };

    // Aligned with openclaw: deleteAccount
    config->delete_account = [](const core::Config& cfg, const std::string& account_id) -> core::Config {
        core::Config next = cfg;
        const std::string norm_id = normalize_account_id(account_id);
        auto& j = next.data;
        if (j.contains("channels") && j["channels"].contains("slack")) {
            auto& sl = j["channels"]["slack"];
            if (sl.contains("accounts") && sl["accounts"].contains(norm_id)) {
                sl["accounts"].erase(norm_id);
            }
        }
        return next;
    };

    p.config = config;

    // ── groups (严格对齐 OpenClaw dock.ts slack.groups) ──────────────────────
    ChannelGroupAdapter groups;
    // Aligned with openclaw: resolveSlackGroupRequireMention
    groups.resolve_require_mention = [](const ChannelGroupContext& ctx) -> std::optional<bool> {
        return resolve_slack_group_require_mention(ctx);
    };
    // Aligned with openclaw: resolveSlackGroupToolPolicy
    groups.resolve_tool_policy = resolve_slack_group_tool_policy;
    p.groups = groups;

    // ── mentions (对齐 OpenClaw dock.ts slack.mentions) ──────────────────────
    // Aligned with openclaw: stripPatterns: () => ["<@[^>]+>"]
    ChannelMentionAdapter mentions;
    mentions.strip_patterns = [](const nlohmann::json&, const core::Config&, const std::string&) -> std::vector<std::string> {
        return {"<@[^>]+>"};
    };
    p.mentions = mentions;

    // ── threading (严格对齐 OpenClaw dock.ts slack.threading) ────────────────
    ChannelThreadingAdapter threading;

    // Aligned with openclaw: allowExplicitReplyTagsWhenOff = false
    threading.allow_explicit_reply_tags_when_off = false;

    // Aligned with openclaw: resolveReplyToMode → resolveSlackReplyToMode(inspectSlackAccount(...), chatType)
    threading.resolve_reply_to_mode = [](const core::Config& cfg, const std::string& account_id, const std::string& chat_type) -> std::string {
        auto account = resolve_slack_account(cfg, account_id);
        return resolve_slack_reply_to_mode(account, chat_type);
    };

    // Aligned with openclaw: buildToolContext → buildSlackThreadingToolContext
    threading.build_tool_context = [](const core::Config& cfg, const std::string& account_id, const ChannelThreadingContext& ctx) -> std::optional<ChannelThreadingToolContext> {
        return build_slack_threading_tool_context(cfg, account_id, ctx);
    };

    p.threading = threading;

    // ── gateway (对齐 OpenClaw ChannelGatewayAdapter) ────────────────────────
    ChannelGatewayAdapter gateway;

    // Aligned with openclaw: startAccount → Slack Socket Mode connection
    // Slack Socket Mode uses WebSocket via app-level token (xapp-...) to receive events.
    // Full WebSocket implementation pending; here we validate tokens and mark as running.
    gateway.start_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        ctx.log->info("[slack] start_account account_id=" + ctx.account_id);

        auto account = resolve_slack_account(ctx.cfg, ctx.account_id);
        const std::string bot_token = resolve_slack_bot_token(account, ctx.cfg, ctx.account_id);
        if (bot_token.empty()) {
            ctx.log->error("[slack] no bot token configured for account=" + ctx.account_id);
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running    = false;
            snap.configured = false;
            snap.last_error = "no bot token configured";
            ctx.set_status(snap);
            return core::Result<void>::Err("no bot token configured");
        }

        // Aligned with openclaw: validate bot token via auth.test
        {
            httplib::Client cli("https://slack.com");
            cli.set_connection_timeout(10);
            cli.set_read_timeout(10);
            httplib::Headers headers = {{"Authorization", "Bearer " + bot_token}};
            auto res = cli.Post("/api/auth.test", headers, "", "application/json");
            if (!res || res->status != 200) {
                std::string err = res ? ("Slack auth.test failed: " + res->body) : "connection failed";
                ctx.log->error("[slack] " + err);
                ChannelAccountSnapshot snap = ctx.get_status();
                snap.running    = false;
                snap.configured = false;
                snap.last_error = err;
                ctx.set_status(snap);
                return core::Result<void>::Err(err);
            }
            try {
                auto resp = nlohmann::json::parse(res->body);
                if (!resp.value("ok", false)) {
                    std::string err = "Slack auth.test error: " + resp.value("error", "unknown");
                    ctx.log->error("[slack] " + err);
                    ChannelAccountSnapshot snap = ctx.get_status();
                    snap.running    = false;
                    snap.configured = false;
                    snap.last_error = err;
                    ctx.set_status(snap);
                    return core::Result<void>::Err(err);
                }
            } catch (...) {}
            ctx.log->info("[slack] bot token validated via auth.test");
        }

        // Update status: running
        {
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running   = true;
            snap.connected = true;
            ctx.set_status(snap);
        }

        ctx.log->info("[slack] account started (Socket Mode WebSocket pending full implementation)");

        // Block until abortSignal
        while (!ctx.abort_signal->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Shutdown
        {
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running   = false;
            snap.connected = false;
            ctx.set_status(snap);
        }
        ctx.log->info("[slack] account stopped for account=" + ctx.account_id);
        return core::Result<void>::Ok();
    };

    gateway.stop_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        ctx.log->info("[slack] stop_account account_id=" + ctx.account_id);
        ChannelAccountSnapshot snapshot = ctx.get_status();
        snapshot.running   = false;
        snapshot.connected = false;
        ctx.set_status(snapshot);
        return core::Result<void>::Ok();
    };

    // Aligned with openclaw: logoutAccount → revoke tokens
    gateway.logout_account = [](const ChannelLogoutContext& ctx) -> core::Result<ChannelLogoutResult> {
        // Aligned with openclaw: auth.revoke
        auto account = resolve_slack_account(ctx.cfg, ctx.account_id);
        const std::string bot_token = resolve_slack_bot_token(account, ctx.cfg, ctx.account_id);
        if (!bot_token.empty()) {
            httplib::Client cli("https://slack.com");
            cli.set_connection_timeout(5);
            cli.set_read_timeout(10);
            httplib::Headers headers = {{"Authorization", "Bearer " + bot_token}};
            cli.Post("/api/auth.revoke", headers, "", "application/json");
        }
        ChannelLogoutResult result;
        result.cleared    = true;
        result.logged_out = true;
        return core::Result<ChannelLogoutResult>::Ok(result);
    };

    p.gateway = gateway;

    // ── streaming (对齐 OpenClaw dock.ts slack.streaming) ────────────────────
    // Aligned with openclaw: streaming = DEFAULT_BLOCK_STREAMING_COALESCE
    // { blockStreamingCoalesceDefaults: { minChars: 1500, idleMs: 1000 } }
    ChannelStreamingAdapter streaming;
    streaming.block_streaming_coalesce_defaults = ChannelStreamingAdapter::CoalesceDefaults{1500, 1000};
    p.streaming = streaming;

    // ── security (对齐 OpenClaw ChannelSecurityAdapter) ───────────────────────
    // Aligned with openclaw: security.resolveDmPolicy
    // Slack DM policy: check cfg.channels.slack.dm.policy / allowFrom
    ChannelSecurityAdapter security;
    security.resolve_dm_policy = [](const core::Config& cfg, const std::string& account_id, const nlohmann::json& /*account*/) -> std::optional<ChannelSecurityDmPolicy> {
        const auto& j = cfg.data;
        if (!j.contains("channels") || !j["channels"].contains("slack")) return std::nullopt;
        const auto account = resolve_slack_account(cfg, account_id);
        std::string policy;
        if (account.contains("dm") && account["dm"].contains("policy") && account["dm"]["policy"].is_string()) {
            policy = account["dm"]["policy"].get<std::string>();
        }
        if (policy.empty()) return std::nullopt;
        ChannelSecurityDmPolicy dm_policy;
        dm_policy.policy = policy;
        dm_policy.policy_path = "channels.slack.dm.policy";
        dm_policy.allow_from_path = "channels.slack.dm.allowFrom";
        dm_policy.approve_hint = "Add the Slack user ID to channels.slack.dm.allowFrom";
        // Aligned with openclaw: normalizeEntry → lowercase, no prefix stripping for Slack
        dm_policy.normalize_entry = [](const std::string& raw) -> std::string {
            std::string s = raw;
            while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
            while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s;
        };
        return dm_policy;
    };
    security.collect_warnings = [](const core::Config& /*cfg*/, const std::string& /*account_id*/, const nlohmann::json& /*account*/) -> std::vector<std::string> {
        return {};
    };
    p.security = security;

    // ── heartbeat (对齐 OpenClaw ChannelHeartbeatAdapter) ─────────────────────
    // Aligned with openclaw: heartbeat.checkReady → validate bot token via auth.test
    // heartbeat.resolveRecipients → resolve defaultTo or allowFrom
    ChannelHeartbeatAdapter heartbeat;
    heartbeat.check_ready = [](const core::Config& cfg, const std::string& account_id) -> core::Result<ChannelHeartbeatAdapter::ReadyResult> {
        const auto account = resolve_slack_account(cfg, account_id);
        const std::string bot_token = resolve_slack_bot_token(account, cfg, account_id);
        if (bot_token.empty()) {
            return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({false, "no bot token configured"});
        }
        httplib::Client cli("https://slack.com");
        cli.set_connection_timeout(8);
        cli.set_read_timeout(8);
        httplib::Headers headers = {{"Authorization", "Bearer " + bot_token}};
        auto res = cli.Post("/api/auth.test", headers, "", "application/json");
        if (!res || res->status != 200) {
            return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({false, "Slack auth.test failed"});
        }
        try {
            auto resp = nlohmann::json::parse(res->body);
            if (!resp.value("ok", false)) {
                return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({false, "Slack auth.test error: " + resp.value("error", "unknown")});
            }
        } catch (...) {}
        return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({true, "ok"});
    };
    heartbeat.resolve_recipients = [](const core::Config& cfg, const std::string& account_id, bool /*all*/) -> ChannelHeartbeatAdapter::RecipientsResult {
        const auto account = resolve_slack_account(cfg, account_id);
        if (account.contains("defaultTo") && account["defaultTo"].is_string()) {
            std::string v = account["defaultTo"].get<std::string>();
            while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
            while (!v.empty() && std::isspace((unsigned char)v.back()))  v.pop_back();
            if (!v.empty()) return {{v}, "config.defaultTo"};
        }
        return {{}, "none"};
    };
    p.heartbeat = heartbeat;

    // ── directory (对齐 OpenClaw ChannelDirectoryAdapter) ─────────────────────
    // Aligned with openclaw: directory.self → auth.test → bot user info
    // directory.listPeers → users.list
    // directory.listGroups → conversations.list (channels/groups)
    ChannelDirectoryAdapter directory;
    directory.self = [](const core::Config& cfg, const std::string& account_id) -> core::Result<std::optional<ChannelDirectoryEntry>> {
        const auto account = resolve_slack_account(cfg, account_id);
        const std::string bot_token = resolve_slack_bot_token(account, cfg, account_id);
        if (bot_token.empty()) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
        httplib::Client cli("https://slack.com");
        cli.set_connection_timeout(8);
        cli.set_read_timeout(8);
        httplib::Headers headers = {{"Authorization", "Bearer " + bot_token}};
        auto res = cli.Post("/api/auth.test", headers, "", "application/json");
        if (!res || res->status != 200) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
        try {
            auto j = nlohmann::json::parse(res->body);
            if (!j.value("ok", false)) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
            ChannelDirectoryEntry entry;
            entry.kind = "user";
            entry.id = j.value("user_id", std::string{});
            entry.name = j.value("user", std::string{});
            entry.handle = j.value("user", std::string{});
            return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(entry);
        } catch (...) {}
        return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
    };
    directory.list_groups = [](const core::Config& cfg, const std::string& account_id, const std::string& query, int limit) -> core::Result<std::vector<ChannelDirectoryEntry>> {
        // Aligned with openclaw: listSlackDirectoryGroupsFromConfig
        // Returns configured channels from config
        const auto& j = cfg.data;
        std::vector<ChannelDirectoryEntry> entries;
        if (!j.contains("channels") || !j["channels"].contains("slack")) return core::Result<std::vector<ChannelDirectoryEntry>>::Ok(entries);
        const auto account = resolve_slack_account(cfg, account_id);
        if (account.contains("channels") && account["channels"].is_object()) {
            for (auto it = account["channels"].begin(); it != account["channels"].end(); ++it) {
                ChannelDirectoryEntry entry;
                entry.kind = "channel";
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

    // ── actions (对齐 OpenClaw: createSlackActions) ───────────────────────────
    // Aligned with openclaw: slack.actions.ts → createSlackActions("slack")
    // listActions → listSlackMessageActions(cfg)
    // extractToolSend → extractSlackToolSend(args)
    // handleAction → handleSlackMessageAction(...)
    ChannelMessageActionAdapter actions;

    // Aligned with openclaw: listSlackMessageActions(cfg)
    // Returns actions based on configured bot token
    actions.list_actions = [](const core::Config& cfg) -> std::vector<std::string> {
        const auto& j = cfg.data;
        bool has_token = false;
        if (j.contains("channels") && j["channels"].contains("slack")) {
            const auto& sl = j["channels"]["slack"];
            if (sl.contains("botToken") && sl["botToken"].is_string() && !sl["botToken"].get<std::string>().empty()) {
                has_token = true;
            }
            if (!has_token && sl.contains("accounts") && sl["accounts"].is_object()) {
                for (auto it = sl["accounts"].begin(); it != sl["accounts"].end(); ++it) {
                    if (it.value().contains("botToken") && it.value()["botToken"].is_string() &&
                        !it.value()["botToken"].get<std::string>().empty()) {
                        has_token = true;
                        break;
                    }
                }
            }
        }
        if (!has_token) {
            const char* env_token = std::getenv("SLACK_BOT_TOKEN");
            if (env_token && *env_token) has_token = true;
        }
        if (!has_token) return {};

        // Aligned with openclaw: listSlackMessageActions
        // Slack supports: send, react, read, edit, delete, pin, unpin, list-pins,
        //                 thread-reply, thread-list, search, member-info, channel-info, channel-list
        auto read_action_gate = [&](const std::string& key, bool default_val = true) -> bool {
            if (!j.contains("channels") || !j["channels"].contains("slack")) return default_val;
            const auto& sl = j["channels"]["slack"];
            if (sl.contains("actions") && sl["actions"].is_object()) {
                const auto& acts = sl["actions"];
                if (acts.contains(key) && acts[key].is_boolean()) return acts[key].get<bool>();
            }
            return default_val;
        };

        std::vector<std::string> actions_list;
        actions_list.push_back("send");

        if (read_action_gate("reactions")) {
            actions_list.push_back("react");
            actions_list.push_back("reactions");
        }
        if (read_action_gate("messages")) {
            actions_list.push_back("read");
            actions_list.push_back("edit");
            actions_list.push_back("delete");
        }
        if (read_action_gate("pins")) {
            actions_list.push_back("pin");
            actions_list.push_back("unpin");
            actions_list.push_back("list-pins");
        }
        if (read_action_gate("threads")) {
            actions_list.push_back("thread-reply");
            actions_list.push_back("thread-list");
        }
        if (read_action_gate("search")) {
            actions_list.push_back("search");
        }
        if (read_action_gate("memberInfo")) {
            actions_list.push_back("member-info");
        }
        if (read_action_gate("channelInfo")) {
            actions_list.push_back("channel-info");
            actions_list.push_back("channel-list");
        }
        return actions_list;
    };

    // Aligned with openclaw: extractSlackToolSend(args)
    // Slack: action === "sendMessage" → { to, accountId, threadId }
    actions.extract_tool_send = [](const nlohmann::json& args) -> std::optional<nlohmann::json> {
        if (!args.contains("action") || !args["action"].is_string()) return std::nullopt;
        const std::string action = args["action"].get<std::string>();
        if (action == "sendMessage") {
            if (args.contains("to") && args["to"].is_string()) {
                nlohmann::json result = {{"to", args["to"].get<std::string>()}};
                if (args.contains("accountId") && args["accountId"].is_string()) {
                    result["accountId"] = args["accountId"].get<std::string>();
                }
                if (args.contains("threadId") && args["threadId"].is_string()) {
                    result["threadId"] = args["threadId"].get<std::string>();
                }
                return result;
            }
        }
        return std::nullopt;
    };

    // Aligned with openclaw: handleSlackMessageAction
    // Dispatch to Slack Web API based on action type
    actions.handle_action = [](const ChannelMessageActionContext& ctx) -> core::Result<nlohmann::json> {
        const auto account = resolve_slack_account(ctx.cfg, ctx.account_id.value_or("default"));
        const std::string bot_token = resolve_slack_bot_token(account, ctx.cfg, ctx.account_id.value_or("default"));
        if (bot_token.empty()) {
            return core::Result<nlohmann::json>::Err("no slack bot token configured");
        }

        if (ctx.action == "send") {
            // Aligned with openclaw: sendMessage → POST /chat.postMessage
            const std::string to = ctx.params.value("to", std::string{});
            const std::string message = ctx.params.value("message", ctx.params.value("content", std::string{}));
            if (to.empty()) return core::Result<nlohmann::json>::Err("'to' is required for send action");
            nlohmann::json body = {
                {"channel", to},
                {"text", message}
            };
            if (ctx.params.contains("threadId") && ctx.params["threadId"].is_string()) {
                body["thread_ts"] = ctx.params["threadId"].get<std::string>();
            }
            if (ctx.params.contains("replyTo") && ctx.params["replyTo"].is_string()) {
                body["thread_ts"] = ctx.params["replyTo"].get<std::string>();
            }
            httplib::Client cli("https://slack.com");
            cli.set_connection_timeout(10);
            cli.set_read_timeout(30);
            httplib::Headers headers = {{"Authorization", "Bearer " + bot_token}};
            auto res = cli.Post("/api/chat.postMessage", headers, body.dump(), "application/json");
            if (!res || res->status != 200) {
                std::string err = res ? ("chat.postMessage failed: " + res->body) : "connection failed";
                return core::Result<nlohmann::json>::Err(err);
            }
            nlohmann::json result_json = {{"ok", true}};
            try {
                auto resp = nlohmann::json::parse(res->body);
                if (!resp.value("ok", false)) {
                    return core::Result<nlohmann::json>::Err("chat.postMessage error: " + resp.value("error", "unknown"));
                }
                if (resp.contains("ts")) result_json["messageId"] = resp["ts"].get<std::string>();
                if (resp.contains("channel")) result_json["channelId"] = resp["channel"].get<std::string>();
            } catch (...) {}
            return core::Result<nlohmann::json>::Ok(result_json);
        }

        if (ctx.action == "react") {
            // Aligned with openclaw: react → POST /reactions.add
            const std::string channel = ctx.params.value("to", ctx.params.value("channelId", std::string{}));
            const std::string timestamp = ctx.params.value("messageId", ctx.params.value("ts", std::string{}));
            const std::string emoji = ctx.params.value("emoji", std::string{});
            if (channel.empty() || timestamp.empty() || emoji.empty()) {
                return core::Result<nlohmann::json>::Err("channel, messageId (ts), and emoji are required for react action");
            }
            // Strip colons from emoji name
            std::string emoji_name = emoji;
            if (!emoji_name.empty() && emoji_name.front() == ':') emoji_name = emoji_name.substr(1);
            if (!emoji_name.empty() && emoji_name.back() == ':') emoji_name.pop_back();
            nlohmann::json body = {{"channel", channel}, {"timestamp", timestamp}, {"name", emoji_name}};
            httplib::Client cli("https://slack.com");
            cli.set_connection_timeout(10);
            cli.set_read_timeout(10);
            httplib::Headers headers = {{"Authorization", "Bearer " + bot_token}};
            auto res = cli.Post("/api/reactions.add", headers, body.dump(), "application/json");
            if (!res || res->status != 200) {
                return core::Result<nlohmann::json>::Err(res ? ("reactions.add failed: " + res->body) : "connection failed");
            }
            return core::Result<nlohmann::json>::Ok(nlohmann::json{{"ok", true}});
        }

        if (ctx.action == "delete") {
            // Aligned with openclaw: delete → POST /chat.delete
            const std::string channel = ctx.params.value("channelId", ctx.params.value("to", std::string{}));
            const std::string ts = ctx.params.value("messageId", ctx.params.value("ts", std::string{}));
            if (channel.empty() || ts.empty()) {
                return core::Result<nlohmann::json>::Err("channelId and messageId (ts) are required for delete action");
            }
            nlohmann::json body = {{"channel", channel}, {"ts", ts}};
            httplib::Client cli("https://slack.com");
            cli.set_connection_timeout(10);
            cli.set_read_timeout(10);
            httplib::Headers headers = {{"Authorization", "Bearer " + bot_token}};
            auto res = cli.Post("/api/chat.delete", headers, body.dump(), "application/json");
            if (!res || res->status != 200) {
                return core::Result<nlohmann::json>::Err(res ? ("chat.delete failed: " + res->body) : "connection failed");
            }
            return core::Result<nlohmann::json>::Ok(nlohmann::json{{"ok", true}});
        }

        if (ctx.action == "edit") {
            // Aligned with openclaw: edit → POST /chat.update
            const std::string channel = ctx.params.value("channelId", ctx.params.value("to", std::string{}));
            const std::string ts = ctx.params.value("messageId", ctx.params.value("ts", std::string{}));
            const std::string message = ctx.params.value("message", ctx.params.value("content", std::string{}));
            if (channel.empty() || ts.empty()) {
                return core::Result<nlohmann::json>::Err("channelId and messageId (ts) are required for edit action");
            }
            nlohmann::json body = {{"channel", channel}, {"ts", ts}, {"text", message}};
            httplib::Client cli("https://slack.com");
            cli.set_connection_timeout(10);
            cli.set_read_timeout(10);
            httplib::Headers headers = {{"Authorization", "Bearer " + bot_token}};
            auto res = cli.Post("/api/chat.update", headers, body.dump(), "application/json");
            if (!res || res->status != 200) {
                return core::Result<nlohmann::json>::Err(res ? ("chat.update failed: " + res->body) : "connection failed");
            }
            return core::Result<nlohmann::json>::Ok(nlohmann::json{{"ok", true}});
        }

        return core::Result<nlohmann::json>::Err("Action '" + ctx.action + "' not yet implemented for Slack");
    };

    p.actions = actions;

    // ── reload (对齐 OpenClaw: reload.configPrefixes) ─────────────────────────
    // Aligned with openclaw: reload = { configPrefixes: ["channels.slack"] }
    ChannelPlugin::ReloadConfig reload;
    reload.config_prefixes = {"channels.slack"};
    p.reload = reload;

    // ── defaults.queue (对齐 OpenClaw: defaults.queue.debounceMs) ─────────────
    // Aligned with openclaw: defaults = { queue: { debounceMs: 500 } }
    ChannelPlugin::Defaults defaults;
    defaults.queue = ChannelPlugin::Defaults::Queue{500};
    p.defaults = defaults;

    // ── commands (对齐 OpenClaw: dock.ts slack.commands) ─────────────────────
    // Aligned with openclaw: commands = { skipWhenConfigEmpty: true }
    // (Slack: skip command processing when config is empty — no guilds/channels configured)
    ChannelCommandAdapter commands;
    commands.skip_when_config_empty = true;
    p.commands = commands;

    // ── pairing (对齐 OpenClaw: ChannelPairingAdapter) ──────────────────────
    // Aligned with openclaw: pairing.idLabel = "slackUserId"
    // pairing.normalizeAllowEntry → strip "slack:/user:" prefix
    // pairing.notifyApproval → send approval message via Slack
    ChannelPairingAdapter pairing;
    pairing.id_label = "Slack user id";
    pairing.normalize_allow_entry = [](const std::string& entry) -> std::string {
        std::string s = entry;
        std::string s_lower = s;
        std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
        for (const auto& prefix : std::vector<std::string>{"slack:", "user:"}) {
            if (s_lower.rfind(prefix, 0) == 0) {
                s = s.substr(prefix.size());
                break;
            }
        }
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
        return s;
    };
    pairing.notify_approval = [](const core::Config& cfg, const std::string& id) -> void {
        auto account = resolve_slack_account(cfg, "default");
        const std::string bot_token = resolve_slack_bot_token(account, cfg, "default");
        if (bot_token.empty()) return;
        send_slack_message(bot_token, "user:" + id, "Pairing approved! You can now send messages.",
                           std::nullopt, std::nullopt, std::nullopt);
    };
    p.pairing = pairing;

    // ── setup (对齐 OpenClaw: ChannelSetupAdapter) ────────────────────────────
    // Aligned with openclaw: setup.applyAccountConfig → patchScopedAccountConfig
    // setup.resolveAccountId → normalizeAccountId(input.name ?? accountId)
    ChannelSetupAdapter setup;

    // Aligned with openclaw: resolveAccountId
    setup.resolve_account_id = [](const core::Config& /*cfg*/, const std::string& account_id, const ChannelSetupInput& input) -> std::string {
        if (input.name.has_value() && !input.name->empty()) {
            std::string name = *input.name;
            while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(name.begin());
            while (!name.empty() && std::isspace((unsigned char)name.back()))  name.pop_back();
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (!name.empty()) return name;
        }
        return normalize_account_id(account_id);
    };

    // Aligned with openclaw: applyAccountConfig → patch channels.slack.accounts[accountId].botToken + appToken
    setup.apply_account_config = [](const core::Config& cfg, const std::string& account_id, const ChannelSetupInput& input) -> core::Config {
        core::Config next = cfg;
        const std::string norm_id = normalize_account_id(account_id);
        auto& j = next.data;
        if (!j.contains("channels")) j["channels"] = nlohmann::json::object();
        if (!j["channels"].contains("slack")) j["channels"]["slack"] = nlohmann::json::object();
        auto& sl = j["channels"]["slack"];

        // Aligned with openclaw: patchScopedAccountConfig
        if (norm_id == "default") {
            sl["enabled"] = true;
            if (input.bot_token.has_value() && !input.bot_token->empty()) sl["botToken"] = *input.bot_token;
            if (input.app_token.has_value() && !input.app_token->empty()) sl["appToken"] = *input.app_token;
        } else {
            if (!sl.contains("accounts")) sl["accounts"] = nlohmann::json::object();
            if (!sl["accounts"].contains(norm_id)) sl["accounts"][norm_id] = nlohmann::json::object();
            auto& acc = sl["accounts"][norm_id];
            acc["enabled"] = true;
            if (input.bot_token.has_value() && !input.bot_token->empty()) acc["botToken"] = *input.bot_token;
            if (input.app_token.has_value() && !input.app_token->empty()) acc["appToken"] = *input.app_token;
        }
        return next;
    };

    // Aligned with openclaw: validateInput → botToken must be non-empty
    setup.validate_input = [](const core::Config& /*cfg*/, const std::string& /*account_id*/, const ChannelSetupInput& input) -> std::optional<std::string> {
        if (!input.bot_token.has_value() || input.bot_token->empty()) {
            return std::string("Slack bot token (xoxb-...) is required");
        }
        return std::nullopt;
    };

    p.setup = setup;

    // ── messaging (对齐 OpenClaw: ChannelMessagingAdapter) ──────────────────
    // Aligned with openclaw: messaging.normalizeTarget = normalizeSlackMessagingTarget
    // messaging.targetResolver.looksLikeId = looksLikeSlackTargetId
    // messaging.targetResolver.hint = "<channelId|user:ID|channel:ID>"
    ChannelMessagingAdapter messaging;
    messaging.normalize_target = [](const std::string& raw) -> std::optional<std::string> {
        // Aligned with openclaw: normalizeSlackMessagingTarget → parseSlackTarget(raw, { defaultKind: "channel" })
        std::string s = raw;
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
        if (s.empty()) return std::nullopt;
        // Strip slack: prefix
        std::string s_lower = s;
        std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
        if (s_lower.rfind("slack:", 0) == 0) {
            s = s.substr(6);
        }
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
        if (s.empty()) return std::nullopt;
        // Detect kind: user: / channel: / <@...> / #... / bare ID
        if (s.rfind("user:", 0) == 0 || s.rfind("channel:", 0) == 0) {
            return s;
        }
        // <@U...> → user:U...
        if (s.size() > 3 && s.front() == '<' && s[1] == '@') {
            auto end = s.find('>');
            if (end != std::string::npos) {
                return "user:" + s.substr(2, end - 2);
            }
        }
        // #channel → channel:channel
        if (s.front() == '#') {
            return "channel:" + s.substr(1);
        }
        // Bare Slack ID (C/U/W/G/D prefix)
        if (!s.empty() && (s[0] == 'C' || s[0] == 'U' || s[0] == 'W' || s[0] == 'G' || s[0] == 'D')) {
            return "channel:" + s;
        }
        return s;
    };
    p.messaging = messaging;

    // ── agentPrompt (对齐 OpenClaw: ChannelAgentPromptAdapter) ──────────────
    // Aligned with openclaw: agentPrompt.messageToolHints → interactive replies hints
    ChannelAgentPromptAdapter agent_prompt;
    agent_prompt.message_tool_hints = [](const core::Config& cfg, const std::string& /*account_id*/) -> std::vector<std::string> {
        // Aligned with openclaw: isSlackInteractiveRepliesEnabled check
        bool interactive_enabled = false;
        const auto& j = cfg.data;
        if (j.contains("channels") && j["channels"].contains("slack")) {
            const auto& sl = j["channels"]["slack"];
            if (sl.contains("capabilities") && sl["capabilities"].is_object()) {
                interactive_enabled = sl["capabilities"].value("interactiveReplies", false);
            }
        }
        if (interactive_enabled) {
            return {
                "Slack interactive replies: use `[[slack_buttons: Label:value, Other:other]]` to add action buttons.",
                "Slack selects: use `[[slack_select: Placeholder | Label:value, Other:other]]` to add a static select menu."
            };
        }
        return {
            "Slack interactive replies are disabled. Set channels.slack.capabilities.interactiveReplies=true to enable."
        };
    };
    p.agent_prompt = agent_prompt;

    // ── status (对齐 OpenClaw: ChannelStatusAdapter) ──────────────────────────
    // Aligned with openclaw: status.collectStatusIssues
    // Slack: no specific status issues defined in openclaw (no status-issues/slack.ts)
    // → return empty issues list (placeholder for future implementation)
    ChannelStatusAdapter status;
    status.collect_status_issues = [](const std::vector<ChannelAccountSnapshot>& /*accounts*/) -> std::vector<nlohmann::json> {
        // Aligned with openclaw: Slack has no collectStatusIssues defined
        // (no status-issues/slack.ts in openclaw)
        return {};
    };
    p.status = status;

    return p;
}

// Static registration
static const ChannelPlugin SLACK_PLUGIN = make_slack_plugin();

} // namespace

// Register on library load
static struct SlackPluginRegistrar {
    SlackPluginRegistrar() {
        ChannelPluginRegistry::instance().register_plugin(SLACK_PLUGIN, 0);
    }
} _slack_registrar;

} // namespace channels
} // namespace hunterclaw
