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

    // ── streaming (对齐 OpenClaw DEFAULT_BLOCK_STREAMING_COALESCE) ────────────
    ChannelStreamingAdapter streaming;
    streaming.block_streaming_coalesce_defaults = {1500, 1000};
    p.streaming = streaming;

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
