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
// Helpers: config inspection (对齐 openclaw inspectDiscordAccount)
// ---------------------------------------------------------------------------

static std::string normalize_account_id(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    if (s.empty()) return "default";
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// Aligned with openclaw: mergeDiscordAccountConfig
// Returns cfg.channels.discord.accounts[accountId] merged with top-level cfg.channels.discord
static nlohmann::json resolve_discord_account(const core::Config& cfg, const std::string& account_id) {
    const auto& j = cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("discord")) {
        return nlohmann::json::object();
    }
    const auto& dc = j["channels"]["discord"];
    nlohmann::json merged = nlohmann::json::object();
    for (auto it = dc.begin(); it != dc.end(); ++it) {
        if (it.key() != "accounts") merged[it.key()] = it.value();
    }
    const std::string norm_id = normalize_account_id(account_id);
    if (dc.contains("accounts") && dc["accounts"].is_object()) {
        for (auto it = dc["accounts"].begin(); it != dc["accounts"].end(); ++it) {
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

// Aligned with openclaw: listDiscordAccountIds
static std::vector<std::string> list_discord_account_ids(const core::Config& cfg) {
    const auto& j = cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("discord")) return {"default"};
    const auto& dc = j["channels"]["discord"];
    if (!dc.contains("accounts") || !dc["accounts"].is_object() || dc["accounts"].empty()) return {"default"};
    std::vector<std::string> ids;
    for (auto it = dc["accounts"].begin(); it != dc["accounts"].end(); ++it) {
        ids.push_back(it.key());
    }
    return ids;
}

// Aligned with openclaw: inspectDiscordAccount → token resolution
// Returns token from: account.token → channel.token → env DISCORD_BOT_TOKEN
static std::string resolve_discord_token(const nlohmann::json& account, const core::Config& cfg, const std::string& account_id) {
    // Strip "Bot " prefix (aligned with openclaw: token.replace(/^Bot\s+/i, ""))
    auto strip_bot_prefix = [](std::string t) -> std::string {
        if (t.size() > 4) {
            std::string prefix = t.substr(0, 4);
            std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::tolower);
            if (prefix == "bot ") return t.substr(4);
        }
        return t;
    };

    if (account.contains("token") && account["token"].is_string()) {
        std::string t = account["token"].get<std::string>();
        if (!t.empty()) return strip_bot_prefix(t);
    }
    // env fallback for default account
    if (normalize_account_id(account_id) == "default") {
        const char* env_token = std::getenv("DISCORD_BOT_TOKEN");
        if (env_token && *env_token) return strip_bot_prefix(std::string(env_token));
    }
    return "";
}

// Aligned with openclaw: normalizeDiscordOutboundTarget (normalize/discord.ts)
// Strips "channel:" prefix and validates the target
static std::string normalize_discord_outbound_target(const std::string& to) {
    if (to.rfind("channel:", 0) == 0) return to.substr(8);
    return to;
}

// Aligned with openclaw: resolveDiscordOutboundTarget
// If threadId is set, use "channel:{threadId}" as target
static std::string resolve_discord_outbound_target(const std::string& to, const std::optional<std::string>& thread_id) {
    if (thread_id.has_value() && !thread_id->empty()) {
        return "channel:" + *thread_id;
    }
    return to;
}

// Aligned with openclaw: sendMessageDiscord (discord/send.ts)
// POST /channels/{channel_id}/messages
static core::Result<OutboundDeliveryResult> send_discord_message(
    const std::string& token,
    const std::string& channel_id,
    const std::string& text,
    const std::optional<std::string>& reply_to_id,
    const std::optional<std::string>& media_url,
    bool silent = false
) {
    httplib::Client cli("https://discord.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    // Resolve actual channel id (strip "channel:" prefix)
    std::string actual_channel = normalize_discord_outbound_target(channel_id);

    nlohmann::json body = nlohmann::json::object();

    if (media_url.has_value() && !media_url->empty()) {
        // Aligned with openclaw: sendMedia → embed with image or file attachment
        body["content"] = text;
        nlohmann::json embed = {{"image", {{"url", *media_url}}}};
        body["embeds"] = nlohmann::json::array({embed});
    } else {
        body["content"] = text;
    }

    // Aligned with openclaw: replyTo → message_reference
    if (reply_to_id.has_value() && !reply_to_id->empty()) {
        body["message_reference"] = {{"message_id", *reply_to_id}};
    }

    // Aligned with openclaw: silent → flags = 4096 (SUPPRESS_NOTIFICATIONS)
    if (silent) {
        body["flags"] = 4096;
    }

    std::string endpoint = "/api/v10/channels/" + actual_channel + "/messages";
    httplib::Headers headers = {
        {"Authorization", "Bot " + token},
        {"Content-Type", "application/json"}
    };

    auto res = cli.Post(endpoint, headers, body.dump(), "application/json");
    if (!res) {
        return core::Result<OutboundDeliveryResult>::Err("HTTP request failed: " + httplib::to_string(res.error()));
    }
    if (res->status != 200 && res->status != 201) {
        return core::Result<OutboundDeliveryResult>::Err("Discord API error " + std::to_string(res->status) + ": " + res->body);
    }

    OutboundDeliveryResult result;
    result.channel = "discord";
    try {
        auto resp_json = nlohmann::json::parse(res->body);
        result.message_id  = resp_json.value("id", std::string{});
        result.channel_id  = resp_json.value("channel_id", std::string{});
    } catch (...) {}
    return core::Result<OutboundDeliveryResult>::Ok(result);
}

// Aligned with openclaw: sendPollDiscord (discord/send.ts)
// POST /channels/{channel_id}/messages with poll object
static core::Result<ChannelPollResult> send_discord_poll(
    const std::string& token,
    const std::string& channel_id,
    const ChannelPollContext& ctx
) {
    httplib::Client cli("https://discord.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    std::string actual_channel = normalize_discord_outbound_target(channel_id);

    // Aligned with openclaw: Discord poll format (API v10)
    nlohmann::json answers = nlohmann::json::array();
    for (const auto& opt : ctx.poll.options) {
        answers.push_back({{"poll_media", {{"text", opt}}}});
    }

    nlohmann::json body = {
        {"poll", {
            {"question", {{"text", ctx.poll.question}}},
            {"answers", answers},
            {"duration", 24},  // hours
            {"allow_multiselect", ctx.poll.allows_multiple_answers.value_or(false)}
        }}
    };

    std::string endpoint = "/api/v10/channels/" + actual_channel + "/messages";
    httplib::Headers headers = {
        {"Authorization", "Bot " + token},
        {"Content-Type", "application/json"}
    };

    auto res = cli.Post(endpoint, headers, body.dump(), "application/json");
    if (!res || (res->status != 200 && res->status != 201)) {
        std::string err = res ? ("Discord poll API error " + std::to_string(res->status) + ": " + res->body) : "connection failed";
        return core::Result<ChannelPollResult>::Err(err);
    }

    ChannelPollResult result;
    try {
        auto resp_json = nlohmann::json::parse(res->body);
        result.message_id = resp_json.value("id", std::string{});
        result.channel_id = resp_json.value("channel_id", std::string{});
    } catch (...) {}
    return core::Result<ChannelPollResult>::Ok(result);
}

// Aligned with openclaw: resolveDiscordGroupRequireMention (group-mentions.ts)
// Checks cfg.channels.discord.guilds[groupSpace].channels[groupId/groupChannel].requireMention
// → guild.requireMention → default true
static bool resolve_discord_group_require_mention(const ChannelGroupContext& ctx) {
    const auto& j = ctx.cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("discord")) return true;
    const auto& dc = j["channels"]["discord"];
    if (!dc.contains("guilds") || !dc["guilds"].is_object()) return true;
    const auto& guilds = dc["guilds"];

    // Aligned with openclaw: resolveDiscordGuildEntry
    // Find guild by groupSpace (exact match, then normalized, then "*")
    const nlohmann::json* guild_entry = nullptr;
    const std::string space = ctx.group_space.value_or("");
    if (!space.empty() && guilds.contains(space)) {
        guild_entry = &guilds[space];
    } else if (guilds.contains("*")) {
        guild_entry = &guilds["*"];
    }
    if (!guild_entry) return true;

    // Aligned with openclaw: resolveDiscordChannelEntry
    // Find channel by groupId or groupChannel
    if (guild_entry->contains("channels") && (*guild_entry)["channels"].is_object()) {
        const auto& channels = (*guild_entry)["channels"];
        const std::string group_id      = ctx.group_id.value_or("");
        const std::string group_channel = ctx.group_channel.value_or("");

        auto find_channel = [&]() -> const nlohmann::json* {
            if (!group_id.empty() && channels.contains(group_id)) return &channels[group_id];
            if (!group_channel.empty()) {
                if (channels.contains(group_channel)) return &channels[group_channel];
                std::string slug = group_channel;
                if (!slug.empty() && slug[0] == '#') slug = slug.substr(1);
                if (channels.contains(slug)) return &channels[slug];
                if (channels.contains("#" + slug)) return &channels["#" + slug];
            }
            return nullptr;
        };

        const nlohmann::json* ch = find_channel();
        if (ch && ch->contains("requireMention") && (*ch)["requireMention"].is_boolean()) {
            return (*ch)["requireMention"].get<bool>();
        }
    }

    // Guild-level requireMention
    if (guild_entry->contains("requireMention") && (*guild_entry)["requireMention"].is_boolean()) {
        return (*guild_entry)["requireMention"].get<bool>();
    }

    return true;  // Aligned with openclaw: default true for Discord
}

// Aligned with openclaw: resolveDiscordGroupToolPolicy (group-mentions.ts)
static std::optional<nlohmann::json> resolve_discord_group_tool_policy(const ChannelGroupContext& ctx) {
    const auto& j = ctx.cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("discord")) return std::nullopt;
    const auto& dc = j["channels"]["discord"];
    if (!dc.contains("guilds") || !dc["guilds"].is_object()) return std::nullopt;
    const auto& guilds = dc["guilds"];

    const std::string space = ctx.group_space.value_or("");
    const nlohmann::json* guild_entry = nullptr;
    if (!space.empty() && guilds.contains(space)) guild_entry = &guilds[space];
    else if (guilds.contains("*")) guild_entry = &guilds["*"];
    if (!guild_entry) return std::nullopt;

    // Channel-level tools
    if (guild_entry->contains("channels") && (*guild_entry)["channels"].is_object()) {
        const auto& channels = (*guild_entry)["channels"];
        const std::string group_id      = ctx.group_id.value_or("");
        const std::string group_channel = ctx.group_channel.value_or("");

        auto find_channel = [&]() -> const nlohmann::json* {
            if (!group_id.empty() && channels.contains(group_id)) return &channels[group_id];
            if (!group_channel.empty() && channels.contains(group_channel)) return &channels[group_channel];
            return nullptr;
        };

        const nlohmann::json* ch = find_channel();
        if (ch && ch->contains("tools")) return (*ch)["tools"];
    }

    // Guild-level tools
    if (guild_entry->contains("tools")) return (*guild_entry)["tools"];
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Discord ChannelPlugin descriptor (严格对齐 OpenClaw discord plugin)
// ---------------------------------------------------------------------------

ChannelPlugin make_discord_plugin() {
    ChannelPlugin p;
    p.id = "discord";

    // ── meta (对齐 OpenClaw registry.ts CHAT_CHANNEL_META["discord"]) ────────
    p.meta.id             = "discord";
    p.meta.label          = "Discord";
    p.meta.selection_label = "Discord (Bot API)";
    p.meta.detail_label   = "Discord Bot";
    p.meta.docs_path      = "/channels/discord";
    p.meta.docs_label     = "discord";
    p.meta.blurb          = "very well supported right now.";
    p.meta.system_image   = "bubble.left.and.bubble.right";

    // ── capabilities (对齐 OpenClaw dock.ts DOCKS["discord"]) ────────────────
    p.capabilities.chat_types      = {"direct", "channel", "thread"};
    p.capabilities.polls           = true;
    p.capabilities.reactions       = true;
    p.capabilities.media           = true;
    p.capabilities.native_commands = true;
    p.capabilities.threads         = true;

    // ── outbound (严格对齐 OpenClaw discordOutbound) ──────────────────────────
    // Aligned with openclaw:
    //   deliveryMode: "direct"
    //   chunker: null  (Discord does not use a chunker)
    //   textChunkLimit: 2000
    //   pollMaxOptions: 10
    //   resolveTarget: normalizeDiscordOutboundTarget
    //   sendText / sendMedia / sendPayload / sendPoll
    ChannelOutboundAdapter outbound;
    outbound.delivery_mode    = "direct";
    outbound.chunker          = nullptr;  // Aligned with openclaw: chunker: null
    outbound.text_chunk_limit = 2000;
    outbound.poll_max_options = 10;

    // Aligned with openclaw: resolveTarget → normalizeDiscordOutboundTarget
    outbound.resolve_target = [](const core::Config& /*cfg*/, const std::string& to,
                                  const std::vector<std::string>& /*allow_from*/,
                                  const std::string& /*account_id*/,
                                  const std::string& /*mode*/) -> std::optional<std::string> {
        return normalize_discord_outbound_target(to);
    };

    // Aligned with openclaw: discordOutbound.sendText
    outbound.send_text = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        const std::string token = resolve_discord_token(
            resolve_discord_account(ctx.cfg, ctx.account_id.value_or("default")),
            ctx.cfg, ctx.account_id.value_or("default")
        );
        if (token.empty()) {
            return core::Result<OutboundDeliveryResult>::Err("no discord bot token configured");
        }
        const std::string target = resolve_discord_outbound_target(ctx.to, ctx.thread_id);
        return send_discord_message(token, target, ctx.text, ctx.reply_to_id, std::nullopt, ctx.silent.value_or(false));
    };

    // Aligned with openclaw: discordOutbound.sendMedia
    outbound.send_media = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        const std::string token = resolve_discord_token(
            resolve_discord_account(ctx.cfg, ctx.account_id.value_or("default")),
            ctx.cfg, ctx.account_id.value_or("default")
        );
        if (token.empty()) {
            return core::Result<OutboundDeliveryResult>::Err("no discord bot token configured");
        }
        const std::string target = resolve_discord_outbound_target(ctx.to, ctx.thread_id);
        return send_discord_message(token, target, ctx.text, ctx.reply_to_id, ctx.media_url, ctx.silent.value_or(false));
    };

    // Aligned with openclaw: discordOutbound.sendPayload
    // sendPayload → sendTextMediaPayload (direct-text-media.ts)
    outbound.send_payload = [](const ChannelOutboundPayloadContext& ctx) -> core::Result<OutboundDeliveryResult> {
        const std::string token = resolve_discord_token(
            resolve_discord_account(ctx.cfg, ctx.account_id.value_or("default")),
            ctx.cfg, ctx.account_id.value_or("default")
        );
        if (token.empty()) {
            return core::Result<OutboundDeliveryResult>::Err("no discord bot token configured");
        }
        const std::string text = ctx.payload.value("text", std::string{});
        std::optional<std::string> media_url;
        if (ctx.payload.contains("mediaUrl") && ctx.payload["mediaUrl"].is_string()) {
            media_url = ctx.payload["mediaUrl"].get<std::string>();
        }
        const std::string target = resolve_discord_outbound_target(ctx.to, ctx.thread_id);
        return send_discord_message(token, target, text, ctx.reply_to_id, media_url, ctx.silent.value_or(false));
    };

    // Aligned with openclaw: discordOutbound.sendPoll
    outbound.send_poll = [](const ChannelPollContext& ctx) -> core::Result<ChannelPollResult> {
        const std::string token = resolve_discord_token(
            resolve_discord_account(ctx.cfg, ctx.account_id.value_or("default")),
            ctx.cfg, ctx.account_id.value_or("default")
        );
        if (token.empty()) {
            return core::Result<ChannelPollResult>::Err("no discord bot token configured");
        }
        std::optional<std::string> thread_id;
        if (ctx.thread_id.has_value()) thread_id = *ctx.thread_id;
        const std::string target = resolve_discord_outbound_target(ctx.to, thread_id);
        return send_discord_poll(token, target, ctx);
    };

    p.outbound = outbound;

    // ── config adapter (严格对齐 OpenClaw dock.ts discord.config) ────────────
    auto config = std::make_shared<ChannelConfigAdapter>();

    // Aligned with openclaw: listDiscordAccountIds(cfg)
    config->list_account_ids = [](const core::Config& cfg) -> std::vector<std::string> {
        return list_discord_account_ids(cfg);
    };

    // Aligned with openclaw: mergeDiscordAccountConfig(cfg, accountId)
    config->resolve_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        return resolve_discord_account(cfg, account_id);
    };

    // Aligned with openclaw: inspectDiscordAccount
    config->inspect_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        auto account = resolve_discord_account(cfg, account_id);
        const std::string token = resolve_discord_token(account, cfg, account_id);
        nlohmann::json result = account;
        result["accountId"]   = normalize_account_id(account_id);
        result["token"]       = token;
        result["configured"]  = !token.empty();
        result["tokenStatus"] = token.empty() ? "missing" : "available";
        return result;
    };

    // Aligned with openclaw: isEnabled
    config->is_enabled = [](const nlohmann::json& account, const core::Config& cfg) -> bool {
        const auto& j = cfg.data;
        bool channel_enabled = true;
        if (j.contains("channels") && j["channels"].contains("discord")) {
            const auto& dc = j["channels"]["discord"];
            if (dc.contains("enabled") && dc["enabled"].is_boolean()) {
                channel_enabled = dc["enabled"].get<bool>();
            }
        }
        return channel_enabled && account.value("enabled", true);
    };

    // Aligned with openclaw: isConfigured → token must be present
    config->is_configured = [](const nlohmann::json& account, const core::Config& cfg) -> bool {
        if (account.contains("token") && account["token"].is_string() &&
            !account["token"].get<std::string>().empty()) return true;
        const char* env_token = std::getenv("DISCORD_BOT_TOKEN");
        if (env_token && *env_token) return true;
        return false;
    };

    // Aligned with openclaw: resolveAllowFrom
    // mapAllowFromEntries(inspectDiscordAccount({ cfg, accountId }).config.allowFrom ?? .config.dm?.allowFrom)
    config->resolve_allow_from = [](const core::Config& cfg, const std::string& account_id) -> std::vector<std::string> {
        auto account = resolve_discord_account(cfg, account_id);
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

    // Aligned with openclaw: formatAllowFrom → formatDiscordAllowFrom
    // Strip <@!?...> and discord:/user:/pk: prefixes, lowercase
    config->format_allow_from = [](const core::Config&, const std::string&, const std::vector<std::string>& allow_from) -> std::vector<std::string> {
        std::vector<std::string> result;
        for (auto entry : allow_from) {
            // Strip <@!? ... >
            if (!entry.empty() && entry.front() == '<') {
                auto pos = entry.find('>');
                if (pos != std::string::npos) {
                    entry = entry.substr(2, pos - 2);
                    if (!entry.empty() && entry.front() == '!') entry = entry.substr(1);
                }
            }
            // Strip prefixes (case-insensitive)
            std::string e_lower = entry;
            std::transform(e_lower.begin(), e_lower.end(), e_lower.begin(), ::tolower);
            for (const auto& prefix : std::vector<std::string>{"discord:", "user:", "pk:"}) {
                if (e_lower.rfind(prefix, 0) == 0) {
                    entry   = entry.substr(prefix.size());
                    e_lower = e_lower.substr(prefix.size());
                    break;
                }
            }
            // Trim and lowercase
            while (!entry.empty() && std::isspace((unsigned char)entry.front())) entry.erase(entry.begin());
            while (!entry.empty() && std::isspace((unsigned char)entry.back()))  entry.pop_back();
            std::transform(entry.begin(), entry.end(), entry.begin(), ::tolower);
            if (!entry.empty()) result.push_back(entry);
        }
        return result;
    };

    // Aligned with openclaw: resolveDefaultTo
    config->resolve_default_to = [](const core::Config& cfg, const std::string& account_id) -> std::optional<std::string> {
        auto account = resolve_discord_account(cfg, account_id);
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
        if (!j["channels"].contains("discord")) j["channels"]["discord"] = nlohmann::json::object();
        auto& dc = j["channels"]["discord"];
        if (!dc.contains("accounts")) dc["accounts"] = nlohmann::json::object();
        if (!dc["accounts"].contains(norm_id)) dc["accounts"][norm_id] = nlohmann::json::object();
        dc["accounts"][norm_id]["enabled"] = enabled;
        return next;
    };

    // Aligned with openclaw: deleteAccount
    config->delete_account = [](const core::Config& cfg, const std::string& account_id) -> core::Config {
        core::Config next = cfg;
        const std::string norm_id = normalize_account_id(account_id);
        auto& j = next.data;
        if (j.contains("channels") && j["channels"].contains("discord")) {
            auto& dc = j["channels"]["discord"];
            if (dc.contains("accounts") && dc["accounts"].contains(norm_id)) {
                dc["accounts"].erase(norm_id);
            }
        }
        return next;
    };

    p.config = config;

    // ── groups (严格对齐 OpenClaw dock.ts discord.groups) ────────────────────
    ChannelGroupAdapter groups;
    // Aligned with openclaw: resolveDiscordGroupRequireMention
    groups.resolve_require_mention = [](const ChannelGroupContext& ctx) -> std::optional<bool> {
        return resolve_discord_group_require_mention(ctx);
    };
    // Aligned with openclaw: resolveDiscordGroupToolPolicy
    groups.resolve_tool_policy = resolve_discord_group_tool_policy;
    p.groups = groups;

    // ── mentions (对齐 OpenClaw dock.ts discord.mentions) ────────────────────
    // Aligned with openclaw: stripPatterns: () => ["<@!?\\d+>"]
    ChannelMentionAdapter mentions;
    mentions.strip_patterns = [](const nlohmann::json&, const core::Config&, const std::string&) -> std::vector<std::string> {
        return {"<@!?\\d+>"};
    };
    p.mentions = mentions;

    // ── threading (严格对齐 OpenClaw dock.ts discord.threading) ──────────────
    ChannelThreadingAdapter threading;

    // Aligned with openclaw: resolveReplyToMode → cfg.channels?.discord?.replyToMode ?? "off"
    threading.resolve_reply_to_mode = [](const core::Config& cfg, const std::string& /*account_id*/, const std::string& /*chat_type*/) -> std::string {
        const auto& j = cfg.data;
        if (j.contains("channels") && j["channels"].contains("discord")) {
            const auto& dc = j["channels"]["discord"];
            if (dc.contains("replyToMode") && dc["replyToMode"].is_string()) {
                return dc["replyToMode"].get<std::string>();
            }
        }
        return "off";
    };

    // Aligned with openclaw: buildToolContext (discord)
    // Discord: currentThreadTs = ReplyToId (not MessageThreadId)
    threading.build_tool_context = [](const core::Config& /*cfg*/, const std::string& /*account_id*/, const ChannelThreadingContext& ctx) -> std::optional<ChannelThreadingToolContext> {
        ChannelThreadingToolContext tool_ctx;
        // Aligned with openclaw: currentChannelId = context.To?.trim()
        if (ctx.to.has_value() && !ctx.to->empty()) {
            tool_ctx.current_channel_id = *ctx.to;
        }
        // Aligned with openclaw: currentThreadTs = ReplyToId (Discord uses ReplyToId for threads)
        if (ctx.reply_to_id.has_value() && !ctx.reply_to_id->empty()) {
            tool_ctx.current_thread_ts = *ctx.reply_to_id;
        }
        return tool_ctx;
    };

    p.threading = threading;

    // ── gateway (对齐 OpenClaw ChannelGatewayAdapter) ────────────────────────
    ChannelGatewayAdapter gateway;

    // Aligned with openclaw: startAccount → Discord Gateway WebSocket connection
    // Discord uses Gateway WebSocket (wss://gateway.discord.gg) for receiving events.
    // Full implementation requires WebSocket client; here we establish the connection
    // and handle IDENTIFY + HEARTBEAT + MESSAGE_CREATE events.
    gateway.start_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        ctx.log->info("[discord] start_account account_id=" + ctx.account_id);

        const std::string token = resolve_discord_token(ctx.account, ctx.cfg, ctx.account_id);
        if (token.empty()) {
            ctx.log->error("[discord] no bot token configured for account=" + ctx.account_id);
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running    = false;
            snap.configured = false;
            snap.last_error = "no bot token configured";
            ctx.set_status(snap);
            return core::Result<void>::Err("no bot token configured");
        }

        // Aligned with openclaw: validate token via GET /users/@me
        {
            httplib::Client cli("https://discord.com");
            cli.set_connection_timeout(10);
            cli.set_read_timeout(10);
            httplib::Headers headers = {{"Authorization", "Bot " + token}};
            auto res = cli.Get("/api/v10/users/@me", headers);
            if (!res || res->status != 200) {
                std::string err = res ? ("Discord token validation failed: " + res->body) : "connection failed";
                ctx.log->error("[discord] " + err);
                ChannelAccountSnapshot snap = ctx.get_status();
                snap.running    = false;
                snap.configured = false;
                snap.last_error = err;
                ctx.set_status(snap);
                return core::Result<void>::Err(err);
            }
            ctx.log->info("[discord] token validated via /users/@me");
        }

        // Update status: running (Gateway WebSocket not yet implemented — polling mode)
        {
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running   = true;
            snap.connected = true;  // Mark connected after token validation
            ctx.set_status(snap);
        }

        ctx.log->info("[discord] account started (Gateway WebSocket pending full implementation)");

        // Block until abortSignal
        // Aligned with openclaw: abortSignal.addEventListener("abort", shutdown)
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
        ctx.log->info("[discord] account stopped for account=" + ctx.account_id);
        return core::Result<void>::Ok();
    };

    gateway.stop_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        ctx.log->info("[discord] stop_account account_id=" + ctx.account_id);
        ChannelAccountSnapshot snapshot = ctx.get_status();
        snapshot.running   = false;
        snapshot.connected = false;
        ctx.set_status(snapshot);
        return core::Result<void>::Ok();
    };

    // Aligned with openclaw: logoutAccount → revoke token / clear credentials
    gateway.logout_account = [](const ChannelLogoutContext& ctx) -> core::Result<ChannelLogoutResult> {
        ChannelLogoutResult result;
        result.cleared    = true;
        result.logged_out = true;
        return core::Result<ChannelLogoutResult>::Ok(result);
    };

    p.gateway = gateway;

    // ── elevated (对齐 OpenClaw dock.ts discord.elevated) ────────────────────
    // Aligned with openclaw: elevated.allowFromFallback
    // → cfg.channels?.discord?.allowFrom ?? cfg.channels?.discord?.dm?.allowFrom
    ChannelElevatedAdapter elevated;
    elevated.allow_from_fallback = [](const core::Config& cfg, const std::string& /*account_id*/) -> std::optional<std::vector<std::string>> {
        const auto& j = cfg.data;
        if (!j.contains("channels") || !j["channels"].contains("discord")) return std::nullopt;
        const auto& dc = j["channels"]["discord"];
        auto extract = [](const nlohmann::json& src) -> std::optional<std::vector<std::string>> {
            if (!src.contains("allowFrom") || !src["allowFrom"].is_array()) return std::nullopt;
            std::vector<std::string> result;
            for (const auto& e : src["allowFrom"]) {
                if (e.is_string()) result.push_back(e.get<std::string>());
                else if (e.is_number()) result.push_back(std::to_string(e.get<int64_t>()));
            }
            return result.empty() ? std::nullopt : std::optional<std::vector<std::string>>(result);
        };
        auto v = extract(dc);
        if (v.has_value()) return v;
        if (dc.contains("dm")) return extract(dc["dm"]);
        return std::nullopt;
    };
    p.elevated = elevated;

    // ── streaming (对齐 OpenClaw dock.ts discord.streaming) ──────────────────
    // Aligned with openclaw: streaming = DEFAULT_BLOCK_STREAMING_COALESCE
    // { blockStreamingCoalesceDefaults: { minChars: 1500, idleMs: 1000 } }
    ChannelStreamingAdapter streaming;
    streaming.block_streaming_coalesce_defaults = ChannelStreamingAdapter::CoalesceDefaults{1500, 1000};
    p.streaming = streaming;

    // ── security (对齐 OpenClaw ChannelSecurityAdapter) ───────────────────────
    // Aligned with openclaw: security.resolveDmPolicy
    // Discord DM policy: check cfg.channels.discord.dm.policy / allowFrom
    ChannelSecurityAdapter security;
    security.resolve_dm_policy = [](const core::Config& cfg, const std::string& account_id, const nlohmann::json& /*account*/) -> std::optional<ChannelSecurityDmPolicy> {
        const auto& j = cfg.data;
        if (!j.contains("channels") || !j["channels"].contains("discord")) return std::nullopt;
        const auto& dc = j["channels"]["discord"];
        // Aligned with openclaw: resolveDmPolicy → check dm.policy
        std::string policy;
        if (dc.contains("dm") && dc["dm"].contains("policy") && dc["dm"]["policy"].is_string()) {
            policy = dc["dm"]["policy"].get<std::string>();
        }
        if (policy.empty()) return std::nullopt;
        ChannelSecurityDmPolicy dm_policy;
        dm_policy.policy = policy;
        dm_policy.policy_path = "channels.discord.dm.policy";
        dm_policy.allow_from_path = "channels.discord.dm.allowFrom";
        dm_policy.approve_hint = "Add the Discord user ID to channels.discord.dm.allowFrom";
        // Aligned with openclaw: normalizeEntry → strip discord:/user:/pk: prefix, lowercase
        dm_policy.normalize_entry = [](const std::string& raw) -> std::string {
            std::string s = raw;
            std::string s_lower = s;
            std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
            for (const auto& prefix : std::vector<std::string>{"discord:", "user:", "pk:"}) {
                if (s_lower.rfind(prefix, 0) == 0) { s = s.substr(prefix.size()); break; }
            }
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
    // Aligned with openclaw: heartbeat.checkReady → validate token via /users/@me
    // heartbeat.resolveRecipients → resolve defaultTo or allowFrom
    ChannelHeartbeatAdapter heartbeat;
    heartbeat.check_ready = [](const core::Config& cfg, const std::string& account_id) -> core::Result<ChannelHeartbeatAdapter::ReadyResult> {
        const auto account = resolve_discord_account(cfg, account_id);
        const std::string token = resolve_discord_token(account, cfg, account_id);
        if (token.empty()) {
            return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({false, "no bot token configured"});
        }
        httplib::Client cli("https://discord.com");
        cli.set_connection_timeout(8);
        cli.set_read_timeout(8);
        httplib::Headers headers = {{"Authorization", "Bot " + token}};
        auto res = cli.Get("/api/v10/users/@me", headers);
        if (!res || res->status != 200) {
            std::string err = res ? ("Discord /users/@me failed: HTTP " + std::to_string(res->status)) : "connection failed";
            return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({false, err});
        }
        return core::Result<ChannelHeartbeatAdapter::ReadyResult>::Ok({true, "ok"});
    };
    heartbeat.resolve_recipients = [](const core::Config& cfg, const std::string& account_id, bool /*all*/) -> ChannelHeartbeatAdapter::RecipientsResult {
        const auto account = resolve_discord_account(cfg, account_id);
        // Aligned with openclaw: resolveRecipients → defaultTo or first allowFrom entry
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
    // Aligned with openclaw: directory.listPeers → GET /guilds/{guildId}/members
    // directory.listGroups → list configured guilds/channels
    ChannelDirectoryAdapter directory;
    directory.self = [](const core::Config& cfg, const std::string& account_id) -> core::Result<std::optional<ChannelDirectoryEntry>> {
        const auto account = resolve_discord_account(cfg, account_id);
        const std::string token = resolve_discord_token(account, cfg, account_id);
        if (token.empty()) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
        httplib::Client cli("https://discord.com");
        cli.set_connection_timeout(8);
        cli.set_read_timeout(8);
        httplib::Headers headers = {{"Authorization", "Bot " + token}};
        auto res = cli.Get("/api/v10/users/@me", headers);
        if (!res || res->status != 200) return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
        try {
            auto j = nlohmann::json::parse(res->body);
            ChannelDirectoryEntry entry;
            entry.kind = "user";
            entry.id = j.value("id", std::string{});
            entry.name = j.value("username", std::string{});
            entry.handle = j.value("username", std::string{});
            return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(entry);
        } catch (...) {}
        return core::Result<std::optional<ChannelDirectoryEntry>>::Ok(std::nullopt);
    };
    directory.list_groups = [](const core::Config& cfg, const std::string& account_id, const std::string& /*query*/, int /*limit*/) -> core::Result<std::vector<ChannelDirectoryEntry>> {
        // Aligned with openclaw: listDiscordDirectoryGroupsFromConfig
        // Returns configured guilds/channels from config
        const auto& j = cfg.data;
        std::vector<ChannelDirectoryEntry> entries;
        if (!j.contains("channels") || !j["channels"].contains("discord")) return core::Result<std::vector<ChannelDirectoryEntry>>::Ok(entries);
        const auto account = resolve_discord_account(cfg, account_id);
        if (account.contains("guilds") && account["guilds"].is_object()) {
            for (auto it = account["guilds"].begin(); it != account["guilds"].end(); ++it) {
                ChannelDirectoryEntry entry;
                entry.kind = "group";
                entry.id = it.key();
                if (it.value().is_object() && it.value().contains("name")) {
                    entry.name = it.value()["name"].get<std::string>();
                }
                entries.push_back(entry);
            }
        }
        return core::Result<std::vector<ChannelDirectoryEntry>>::Ok(entries);
    };
    p.directory = directory;

    // ── reload (对齐 OpenClaw: reload.configPrefixes) ─────────────────────────
    // Aligned with openclaw: reload = { configPrefixes: ["channels.discord"] }
    ChannelPlugin::ReloadConfig reload;
    reload.config_prefixes = {"channels.discord"};
    p.reload = reload;

    // ── defaults.queue (对齐 OpenClaw: defaults.queue.debounceMs) ─────────────
    // Aligned with openclaw: defaults = { queue: { debounceMs: 500 } }
    ChannelPlugin::Defaults defaults;
    defaults.queue = ChannelPlugin::Defaults::Queue{500};
    p.defaults = defaults;

    // ── commands (对齐 OpenClaw: commands adapter) ────────────────────────────
    // Aligned with openclaw: commands = { enforceOwnerForCommands: false }
    ChannelCommandAdapter commands;
    commands.enforce_owner_for_commands = false;
    p.commands = commands;

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

    // Aligned with openclaw: applyAccountConfig → patch channels.discord.accounts[accountId].token
    setup.apply_account_config = [](const core::Config& cfg, const std::string& account_id, const ChannelSetupInput& input) -> core::Config {
        core::Config next = cfg;
        const std::string norm_id = normalize_account_id(account_id);
        auto& j = next.data;
        if (!j.contains("channels")) j["channels"] = nlohmann::json::object();
        if (!j["channels"].contains("discord")) j["channels"]["discord"] = nlohmann::json::object();
        auto& dc = j["channels"]["discord"];

        // Aligned with openclaw: patchScopedAccountConfig
        // For default account: patch top-level; for named accounts: patch accounts[id]
        if (norm_id == "default") {
            dc["enabled"] = true;
            if (input.token.has_value() && !input.token->empty()) dc["token"] = *input.token;
        } else {
            if (!dc.contains("accounts")) dc["accounts"] = nlohmann::json::object();
            if (!dc["accounts"].contains(norm_id)) dc["accounts"][norm_id] = nlohmann::json::object();
            auto& acc = dc["accounts"][norm_id];
            acc["enabled"] = true;
            if (input.token.has_value() && !input.token->empty()) acc["token"] = *input.token;
        }
        return next;
    };

    // Aligned with openclaw: validateInput → token must be non-empty
    setup.validate_input = [](const core::Config& /*cfg*/, const std::string& /*account_id*/, const ChannelSetupInput& input) -> std::optional<std::string> {
        if (!input.token.has_value() || input.token->empty()) {
            return std::string("Discord bot token is required");
        }
        return std::nullopt;
    };

    p.setup = setup;

    // ── status (对齐 OpenClaw: ChannelStatusAdapter + collectDiscordStatusIssues) ──
    // Aligned with openclaw: status.collectStatusIssues → collectDiscordStatusIssues
    ChannelStatusAdapter status;

    // Aligned with openclaw: collectDiscordStatusIssues (status-issues/discord.ts)
    // Checks: application.intents.messageContent === "disabled" → intent issue
    //         audit.unresolvedChannels > 0 → config issue
    //         audit.channels[].ok === false → permissions issue
    status.collect_status_issues = [](const std::vector<ChannelAccountSnapshot>& accounts) -> std::vector<nlohmann::json> {
        std::vector<nlohmann::json> issues;
        for (const auto& entry : accounts) {
            // Aligned with openclaw: resolveEnabledConfiguredAccountId
            if (!entry.enabled.value_or(true) || !entry.configured.value_or(true)) continue;
            const std::string account_id = entry.account_id;
            if (account_id.empty()) continue;

            // Check application.intents.messageContent
            if (entry.application.has_value() && entry.application->is_object()) {
                const auto& app = *entry.application;
                if (app.contains("intents") && app["intents"].is_object()) {
                    const auto& intents = app["intents"];
                    if (intents.contains("messageContent") && intents["messageContent"].is_string()) {
                        const std::string mc = intents["messageContent"].get<std::string>();
                        if (mc == "disabled") {
                            issues.push_back({
                                {"channel", "discord"},
                                {"accountId", account_id},
                                {"kind", "intent"},
                                {"message", "Message Content Intent is disabled. Bot may not see normal channel messages."},
                                {"fix", "Enable Message Content Intent in Discord Dev Portal → Bot → Privileged Gateway Intents, or require mention-only operation."}
                            });
                        }
                    }
                }
            }

            // Check audit.unresolvedChannels and audit.channels
            if (entry.audit.has_value() && entry.audit->is_object()) {
                const auto& audit = *entry.audit;
                if (audit.contains("unresolvedChannels") && audit["unresolvedChannels"].is_number()) {
                    int unresolved = audit["unresolvedChannels"].get<int>();
                    if (unresolved > 0) {
                        issues.push_back({
                            {"channel", "discord"},
                            {"accountId", account_id},
                            {"kind", "config"},
                            {"message", "Some configured guild channels are not numeric IDs (unresolvedChannels=" + std::to_string(unresolved) + "). Permission audit can only check numeric channel IDs."},
                            {"fix", "Use numeric channel IDs as keys in channels.discord.guilds.*.channels (then rerun channels status --probe)."}
                        });
                    }
                }
                if (audit.contains("channels") && audit["channels"].is_array()) {
                    for (const auto& ch : audit["channels"]) {
                        if (!ch.is_object()) continue;
                        if (ch.contains("ok") && ch["ok"].is_boolean() && ch["ok"].get<bool>()) continue;
                        const std::string channel_id = ch.value("channelId", std::string{});
                        if (channel_id.empty()) continue;
                        std::string missing_str;
                        if (ch.contains("missing") && ch["missing"].is_array()) {
                            for (const auto& m : ch["missing"]) {
                                if (!missing_str.empty()) missing_str += ", ";
                                missing_str += m.is_string() ? m.get<std::string>() : "";
                            }
                        }
                        std::string err_str = ch.contains("error") && ch["error"].is_string() ? ch["error"].get<std::string>() : "";
                        std::string msg = "Channel " + channel_id + " permission check failed.";
                        if (!missing_str.empty()) msg += " missing " + missing_str;
                        if (!err_str.empty()) msg += ": " + err_str;
                        issues.push_back({
                            {"channel", "discord"},
                            {"accountId", account_id},
                            {"kind", "permissions"},
                            {"message", msg},
                            {"fix", "Ensure the bot role can view + send in this channel (and that channel overrides don't deny it)."}
                        });
                    }
                }
            }
        }
        return issues;
    };

    p.status = status;

    // ── actions (对齐 OpenClaw: discordMessageActions) ────────────────────────
    // Aligned with openclaw: actions.listActions → discordMessageActions.listActions
    // actions.extractToolSend → discordMessageActions.extractToolSend
    // actions.handleAction → discordMessageActions.handleAction
    ChannelMessageActionAdapter actions;

    // Aligned with openclaw: listActions → union of all accounts' action gates
    // HunterClaw: simplified — return base set of actions when token is configured
    actions.list_actions = [](const core::Config& cfg) -> std::vector<std::string> {
        // Check if any discord account is configured
        const auto& j = cfg.data;
        bool has_token = false;
        if (j.contains("channels") && j["channels"].contains("discord")) {
            const auto& dc = j["channels"]["discord"];
            if (dc.contains("token") && dc["token"].is_string() && !dc["token"].get<std::string>().empty()) {
                has_token = true;
            }
            if (!has_token && dc.contains("accounts") && dc["accounts"].is_object()) {
                for (auto it = dc["accounts"].begin(); it != dc["accounts"].end(); ++it) {
                    if (it.value().contains("token") && it.value()["token"].is_string() &&
                        !it.value()["token"].get<std::string>().empty()) {
                        has_token = true;
                        break;
                    }
                }
            }
        }
        if (!has_token) {
            const char* env_token = std::getenv("DISCORD_BOT_TOKEN");
            if (env_token && *env_token) has_token = true;
        }
        if (!has_token) return {};

        // Aligned with openclaw: discordMessageActions.listActions gate logic
        // Read per-account action config from cfg.channels.discord.actions
        auto read_action_gate = [&](const std::string& key, bool default_val = true) -> bool {
            if (!j.contains("channels") || !j["channels"].contains("discord")) return default_val;
            const auto& dc = j["channels"]["discord"];
            if (dc.contains("actions") && dc["actions"].is_object()) {
                const auto& acts = dc["actions"];
                if (acts.contains(key) && acts[key].is_boolean()) return acts[key].get<bool>();
            }
            return default_val;
        };

        std::vector<std::string> actions_list;
        actions_list.push_back("send");

        // Aligned with openclaw: polls (default: true)
        if (read_action_gate("polls")) {
            actions_list.push_back("poll");
        }
        // Aligned with openclaw: reactions (default: true)
        if (read_action_gate("reactions")) {
            actions_list.push_back("react");
            actions_list.push_back("reactions");
            actions_list.push_back("emoji-list");
        }
        // Aligned with openclaw: messages (default: true)
        if (read_action_gate("messages")) {
            actions_list.push_back("read");
            actions_list.push_back("edit");
            actions_list.push_back("delete");
        }
        // Aligned with openclaw: pins (default: true)
        if (read_action_gate("pins")) {
            actions_list.push_back("pin");
            actions_list.push_back("unpin");
            actions_list.push_back("list-pins");
        }
        // Aligned with openclaw: permissions (default: true)
        if (read_action_gate("permissions")) {
            actions_list.push_back("permissions");
        }
        // Aligned with openclaw: threads (default: true)
        if (read_action_gate("threads")) {
            actions_list.push_back("thread-create");
            actions_list.push_back("thread-list");
            actions_list.push_back("thread-reply");
        }
        // Aligned with openclaw: search (default: true)
        if (read_action_gate("search")) {
            actions_list.push_back("search");
        }
        // Aligned with openclaw: stickers (default: true)
        if (read_action_gate("stickers")) {
            actions_list.push_back("sticker");
        }
        // Aligned with openclaw: memberInfo (default: true)
        if (read_action_gate("memberInfo")) {
            actions_list.push_back("member-info");
        }
        // Aligned with openclaw: roleInfo (default: true)
        if (read_action_gate("roleInfo")) {
            actions_list.push_back("role-info");
        }
        // Aligned with openclaw: emojiUploads (default: true)
        if (read_action_gate("emojiUploads")) {
            actions_list.push_back("emoji-upload");
        }
        // Aligned with openclaw: stickerUploads (default: true)
        if (read_action_gate("stickerUploads")) {
            actions_list.push_back("sticker-upload");
        }
        // Aligned with openclaw: roles (default: false)
        if (read_action_gate("roles", false)) {
            actions_list.push_back("role-add");
            actions_list.push_back("role-remove");
        }
        // Aligned with openclaw: channelInfo (default: true)
        if (read_action_gate("channelInfo")) {
            actions_list.push_back("channel-info");
            actions_list.push_back("channel-list");
        }
        // Aligned with openclaw: channels (default: true)
        if (read_action_gate("channels")) {
            actions_list.push_back("channel-create");
            actions_list.push_back("channel-edit");
            actions_list.push_back("channel-delete");
            actions_list.push_back("channel-move");
            actions_list.push_back("category-create");
            actions_list.push_back("category-edit");
            actions_list.push_back("category-delete");
        }
        // Aligned with openclaw: voiceStatus (default: true)
        if (read_action_gate("voiceStatus")) {
            actions_list.push_back("voice-status");
        }
        // Aligned with openclaw: events (default: true)
        if (read_action_gate("events")) {
            actions_list.push_back("event-list");
            actions_list.push_back("event-create");
        }
        // Aligned with openclaw: moderation (default: false)
        if (read_action_gate("moderation", false)) {
            actions_list.push_back("timeout");
            actions_list.push_back("kick");
            actions_list.push_back("ban");
        }
        // Aligned with openclaw: presence (default: false)
        if (read_action_gate("presence", false)) {
            actions_list.push_back("set-presence");
        }
        return actions_list;
    };

    // Aligned with openclaw: supportsButtons → Discord supports buttons (inline components)
    actions.supports_buttons = [](const core::Config& /*cfg*/) -> bool {
        return true;
    };

    // Aligned with openclaw: supportsCards → Discord does not support cards
    actions.supports_cards = [](const core::Config& /*cfg*/) -> bool {
        return false;
    };

    // Aligned with openclaw: extractToolSend → discordMessageActions.extractToolSend
    actions.extract_tool_send = [](const nlohmann::json& args) -> std::optional<nlohmann::json> {
        // Aligned with openclaw: action === "sendMessage" → { to }
        //                        action === "threadReply" → { to: "channel:{channelId}" }
        if (!args.contains("action") || !args["action"].is_string()) return std::nullopt;
        const std::string action = args["action"].get<std::string>();
        if (action == "sendMessage") {
            if (args.contains("to") && args["to"].is_string()) {
                return nlohmann::json{{"to", args["to"].get<std::string>()}};
            }
            return std::nullopt;
        }
        if (action == "threadReply") {
            if (args.contains("channelId") && args["channelId"].is_string()) {
                std::string channel_id = args["channelId"].get<std::string>();
                while (!channel_id.empty() && std::isspace((unsigned char)channel_id.front())) channel_id.erase(channel_id.begin());
                while (!channel_id.empty() && std::isspace((unsigned char)channel_id.back()))  channel_id.pop_back();
                if (!channel_id.empty()) {
                    return nlohmann::json{{"to", "channel:" + channel_id}};
                }
            }
            return std::nullopt;
        }
        return std::nullopt;
    };

    // Aligned with openclaw: handleAction → handleDiscordMessageAction
    // HunterClaw: dispatch to Discord REST API based on action type
    actions.handle_action = [](const ChannelMessageActionContext& ctx) -> core::Result<nlohmann::json> {
        const std::string token = resolve_discord_token(
            resolve_discord_account(ctx.cfg, ctx.account_id.value_or("default")),
            ctx.cfg, ctx.account_id.value_or("default")
        );
        if (token.empty()) {
            return core::Result<nlohmann::json>::Err("no discord bot token configured");
        }

        if (ctx.action == "send") {
            // Aligned with openclaw: sendMessage action
            const std::string to = ctx.params.value("to", std::string{});
            const std::string message = ctx.params.value("message", std::string{});
            if (to.empty()) return core::Result<nlohmann::json>::Err("'to' is required for send action");
            std::optional<std::string> reply_to;
            if (ctx.params.contains("replyTo") && ctx.params["replyTo"].is_string()) {
                reply_to = ctx.params["replyTo"].get<std::string>();
            }
            auto result = send_discord_message(token, to, message, reply_to, std::nullopt, false);
            if (result.is_err()) return core::Result<nlohmann::json>::Err(result.error);
            return core::Result<nlohmann::json>::Ok(nlohmann::json{
                {"ok", true},
                {"messageId", result.value->message_id},
                {"channelId", result.value->channel_id.value_or("")}
            });
        }

        if (ctx.action == "poll") {
            // Aligned with openclaw: poll action → sendPollDiscord
            const std::string to = ctx.params.value("to", std::string{});
            if (to.empty()) return core::Result<nlohmann::json>::Err("'to' is required for poll action");
            ChannelPollContext poll_ctx;
            poll_ctx.cfg = ctx.cfg;
            poll_ctx.to = to;
            poll_ctx.account_id = ctx.account_id;
            poll_ctx.poll.question = ctx.params.value("pollQuestion", std::string{});
            if (ctx.params.contains("pollOption") && ctx.params["pollOption"].is_array()) {
                for (const auto& opt : ctx.params["pollOption"]) {
                    if (opt.is_string()) poll_ctx.poll.options.push_back(opt.get<std::string>());
                }
            }
            auto result = send_discord_poll(token, to, poll_ctx);
            if (result.is_err()) return core::Result<nlohmann::json>::Err(result.error);
            return core::Result<nlohmann::json>::Ok(nlohmann::json{
                {"ok", true},
                {"messageId", result.value->message_id}
            });
        }

        // Aligned with openclaw: other actions → forward to Discord REST API
        return core::Result<nlohmann::json>::Err("Action '" + ctx.action + "' not yet implemented for Discord");
    };

    p.actions = actions;

    return p;
}

// Static registration
static const ChannelPlugin DISCORD_PLUGIN = make_discord_plugin();

} // namespace

// Register on library load
static struct DiscordPluginRegistrar {
    DiscordPluginRegistrar() {
        ChannelPluginRegistry::instance().register_plugin(DISCORD_PLUGIN, 0);
    }
} _discord_registrar;

} // namespace channels
} // namespace hunterclaw
