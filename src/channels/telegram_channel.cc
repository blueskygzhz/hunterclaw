#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/security/timing_safe.h"   // constant-time compare for webhook secret
#include <httplib.h>                            // dedicated HTTP server for webhook listener
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <thread>
#include <chrono>
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
// Helpers: config inspection (对齐 openclaw inspectTelegramAccount)
// ---------------------------------------------------------------------------

// Aligned with openclaw: normalizeAccountId (routing/session-key.ts)
// Trims and lowercases the account id; empty → "default"
static std::string normalize_account_id(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    if (s.empty()) return "default";
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// Aligned with openclaw: resolveTelegramAccountConfig
// Returns cfg.channels.telegram.accounts[accountId] merged with top-level cfg.channels.telegram
static nlohmann::json resolve_telegram_account(const core::Config& cfg, const std::string& account_id) {
    const auto& j = cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("telegram")) {
        return nlohmann::json::object();
    }
    const auto& tg = j["channels"]["telegram"];
    // Start with top-level telegram config (minus "accounts")
    nlohmann::json merged = nlohmann::json::object();
    for (auto it = tg.begin(); it != tg.end(); ++it) {
        if (it.key() != "accounts") {
            merged[it.key()] = it.value();
        }
    }
    // Merge per-account config on top
    const std::string norm_id = normalize_account_id(account_id);
    if (tg.contains("accounts") && tg["accounts"].is_object()) {
        const auto& accounts = tg["accounts"];
        // Case-insensitive lookup
        for (auto it = accounts.begin(); it != accounts.end(); ++it) {
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

// Aligned with openclaw: listTelegramAccountIds
// Returns all account IDs from cfg.channels.telegram.accounts, or ["default"] if none
static std::vector<std::string> list_telegram_account_ids(const core::Config& cfg) {
    const auto& j = cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("telegram")) {
        return {"default"};
    }
    const auto& tg = j["channels"]["telegram"];
    if (!tg.contains("accounts") || !tg["accounts"].is_object() || tg["accounts"].empty()) {
        return {"default"};
    }
    std::vector<std::string> ids;
    for (auto it = tg["accounts"].begin(); it != tg["accounts"].end(); ++it) {
        ids.push_back(it.key());
    }
    return ids;
}

// Aligned with openclaw: inspectTelegramAccount → token resolution
// Returns the bot token from: account.botToken → channel.botToken → env TELEGRAM_BOT_TOKEN
static std::string resolve_telegram_token(const nlohmann::json& account, const core::Config& cfg, const std::string& account_id) {
    // 1. account-level botToken
    if (account.contains("botToken") && account["botToken"].is_string()) {
        std::string t = account["botToken"].get<std::string>();
        if (!t.empty()) return t;
    }
    // 2. account-level tokenFile
    if (account.contains("tokenFile") && account["tokenFile"].is_string()) {
        // tokenFile reading not implemented here; return empty to signal "configured_unavailable"
    }
    // 3. channel-level botToken (already merged into account by resolve_telegram_account)
    // 4. env TELEGRAM_BOT_TOKEN (only for default account)
    if (normalize_account_id(account_id) == "default") {
        const char* env_token = std::getenv("TELEGRAM_BOT_TOKEN");
        if (env_token && *env_token) return std::string(env_token);
    }
    return "";
}

// Aligned with openclaw: resolveTelegramGroupRequireMention (group-mentions.ts)
// Parses groupId as "chatId:topic:topicId" or "chatId:topicId" or plain "chatId"
static std::optional<bool> resolve_telegram_group_require_mention(const ChannelGroupContext& ctx) {
    if (!ctx.group_id.has_value() || ctx.group_id->empty()) {
        return std::nullopt;
    }
    const std::string& group_id = *ctx.group_id;
    const auto& j = ctx.cfg.data;

    // Parse groupId: "chatId:topic:topicId" | "chatId:topicId" | "chatId"
    std::string chat_id;
    std::string topic_id;
    {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : group_id) {
            if (c == ':') { parts.push_back(cur); cur.clear(); }
            else cur += c;
        }
        parts.push_back(cur);

        auto is_numeric = [](const std::string& s) {
            if (s.empty()) return false;
            for (char c : s) if (!std::isdigit((unsigned char)c) && c != '-') return false;
            return true;
        };

        if (parts.size() >= 3 && parts[1] == "topic" && is_numeric(parts[0]) && is_numeric(parts[2])) {
            chat_id  = parts[0];
            topic_id = parts[2];
        } else if (parts.size() >= 2 && is_numeric(parts[0]) && is_numeric(parts[1])) {
            chat_id  = parts[0];
            topic_id = parts[1];
        } else {
            chat_id = group_id;
        }
    }

    if (chat_id.empty()) return std::nullopt;

    // Aligned with openclaw: resolveTelegramRequireMention
    // Check cfg.channels.telegram.groups[chatId].topics[topicId].requireMention
    // → cfg.channels.telegram.groups["*"].topics[topicId].requireMention
    // → cfg.channels.telegram.groups[chatId].requireMention
    // → cfg.channels.telegram.groups["*"].requireMention
    if (j.contains("channels") && j["channels"].contains("telegram")) {
        const auto& tg = j["channels"]["telegram"];
        if (tg.contains("groups") && tg["groups"].is_object()) {
            const auto& groups = tg["groups"];

            auto get_require_mention = [](const nlohmann::json& entry) -> std::optional<bool> {
                if (entry.contains("requireMention") && entry["requireMention"].is_boolean()) {
                    return entry["requireMention"].get<bool>();
                }
                return std::nullopt;
            };

            // topic-level lookup
            if (!topic_id.empty()) {
                if (groups.contains(chat_id) && groups[chat_id].contains("topics")) {
                    const auto& topics = groups[chat_id]["topics"];
                    if (topics.contains(topic_id)) {
                        auto v = get_require_mention(topics[topic_id]);
                        if (v.has_value()) return v;
                    }
                }
                if (groups.contains("*") && groups["*"].contains("topics")) {
                    const auto& topics = groups["*"]["topics"];
                    if (topics.contains(topic_id)) {
                        auto v = get_require_mention(topics[topic_id]);
                        if (v.has_value()) return v;
                    }
                }
            }

            // group-level lookup
            if (groups.contains(chat_id)) {
                auto v = get_require_mention(groups[chat_id]);
                if (v.has_value()) return v;
            }
            if (groups.contains("*")) {
                auto v = get_require_mention(groups["*"]);
                if (v.has_value()) return v;
            }
        }
    }

    // Aligned with openclaw: fallback to resolveChannelGroupRequireMention
    // cfg.channels.telegram.groups[chatId].requireMention (already checked above)
    return std::nullopt;
}

// Aligned with openclaw: resolveTelegramGroupToolPolicy (group-mentions.ts)
static std::optional<nlohmann::json> resolve_telegram_group_tool_policy(const ChannelGroupContext& ctx) {
    if (!ctx.group_id.has_value() || ctx.group_id->empty()) {
        return std::nullopt;
    }
    // Parse chatId from groupId (same logic as above)
    const std::string& group_id = *ctx.group_id;
    std::string chat_id = group_id;
    {
        auto colon = group_id.find(':');
        if (colon != std::string::npos) {
            chat_id = group_id.substr(0, colon);
        }
    }

    const auto& j = ctx.cfg.data;
    if (!j.contains("channels") || !j["channels"].contains("telegram")) {
        return std::nullopt;
    }
    const auto& tg = j["channels"]["telegram"];
    if (!tg.contains("groups") || !tg["groups"].is_object()) {
        return std::nullopt;
    }
    const auto& groups = tg["groups"];

    // Aligned with openclaw: resolveChannelGroupToolsPolicy
    // Check cfg.channels.telegram.groups[chatId].tools
    auto get_tools = [](const nlohmann::json& entry) -> std::optional<nlohmann::json> {
        if (entry.contains("tools")) return entry["tools"];
        return std::nullopt;
    };

    if (groups.contains(chat_id)) {
        auto v = get_tools(groups[chat_id]);
        if (v.has_value()) return v;
    }
    if (groups.contains("*")) {
        auto v = get_tools(groups["*"]);
        if (v.has_value()) return v;
    }
    return std::nullopt;
}

// Aligned with openclaw: markdownToTelegramHtmlChunks (telegram/format.ts)
// Splits text into chunks of at most `limit` characters, respecting paragraph boundaries.
// HunterClaw: simplified implementation — split on double-newline first, then by length.
static std::vector<std::string> markdown_to_telegram_html_chunks(const std::string& text, int limit) {
    if ((int)text.size() <= limit) return {text};

    std::vector<std::string> chunks;
    // Split by paragraph (double newline)
    std::vector<std::string> paragraphs;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i + 1 < text.size() && text[i] == '\n' && text[i+1] == '\n') {
            paragraphs.push_back(cur);
            cur.clear();
            ++i; // skip second \n
        } else {
            cur += text[i];
        }
    }
    if (!cur.empty()) paragraphs.push_back(cur);

    std::string chunk;
    for (const auto& para : paragraphs) {
        std::string sep = chunk.empty() ? "" : "\n\n";
        if ((int)(chunk.size() + sep.size() + para.size()) <= limit) {
            chunk += sep + para;
        } else {
            if (!chunk.empty()) chunks.push_back(chunk);
            // If single paragraph exceeds limit, split by length
            if ((int)para.size() > limit) {
                for (size_t i = 0; i < para.size(); i += limit) {
                    chunks.push_back(para.substr(i, limit));
                }
                chunk.clear();
            } else {
                chunk = para;
            }
        }
    }
    if (!chunk.empty()) chunks.push_back(chunk);
    return chunks.empty() ? std::vector<std::string>{text} : chunks;
}

// Aligned with openclaw: sendMessageTelegram (telegram/send.ts)
// Sends a text message via Telegram Bot API (POST /sendMessage)
static core::Result<OutboundDeliveryResult> send_telegram_message(
    const std::string& token,
    const std::string& chat_id,
    const std::string& text,
    const std::optional<std::string>& reply_to_id,
    const std::optional<std::string>& thread_id,
    const std::optional<std::string>& media_url
) {
    // Aligned with openclaw: sendMessageTelegram → POST /sendMessage or /sendPhoto etc.
    httplib::Client cli("https://api.telegram.org");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    nlohmann::json body;
    body["chat_id"] = chat_id;
    body["parse_mode"] = "HTML";  // Aligned with openclaw: textMode: "html"

    // Aligned with openclaw: messageThreadId (for supergroup topics)
    if (thread_id.has_value() && !thread_id->empty()) {
        try { body["message_thread_id"] = std::stoll(*thread_id); } catch (...) {}
    }
    // Aligned with openclaw: replyToMessageId
    if (reply_to_id.has_value() && !reply_to_id->empty()) {
        try { body["reply_to_message_id"] = std::stoll(*reply_to_id); } catch (...) {}
    }

    std::string endpoint;
    if (media_url.has_value() && !media_url->empty()) {
        // Aligned with openclaw: sendMedia → /sendPhoto or /sendDocument
        // Detect image vs document by extension
        const std::string& url = *media_url;
        bool is_image = url.find(".jpg") != std::string::npos ||
                        url.find(".jpeg") != std::string::npos ||
                        url.find(".png") != std::string::npos ||
                        url.find(".gif") != std::string::npos ||
                        url.find(".webp") != std::string::npos;
        if (is_image) {
            endpoint = "/bot" + token + "/sendPhoto";
            body["photo"] = url;
            if (!text.empty()) body["caption"] = text;
        } else {
            endpoint = "/bot" + token + "/sendDocument";
            body["document"] = url;
            if (!text.empty()) body["caption"] = text;
        }
    } else {
        endpoint = "/bot" + token + "/sendMessage";
        body["text"] = text;
    }

    auto res = cli.Post(endpoint, body.dump(), "application/json");
    if (!res) {
        return core::Result<OutboundDeliveryResult>::Err("HTTP request failed: " + httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        return core::Result<OutboundDeliveryResult>::Err("Telegram API error " + std::to_string(res->status) + ": " + res->body);
    }

    OutboundDeliveryResult result;
    result.channel = "telegram";
    try {
        auto resp_json = nlohmann::json::parse(res->body);
        if (resp_json.contains("result")) {
            const auto& r = resp_json["result"];
            result.message_id = std::to_string(r.value("message_id", 0));
            result.chat_id    = std::to_string(r.contains("chat") ? r["chat"].value("id", 0LL) : 0LL);
        }
    } catch (...) {}
    return core::Result<OutboundDeliveryResult>::Ok(result);
}

// ---------------------------------------------------------------------------
// Telegram ChannelPlugin descriptor (严格对齐 OpenClaw telegram plugin)
// ---------------------------------------------------------------------------

ChannelPlugin make_telegram_plugin() {
    ChannelPlugin p;
    p.id = "telegram";

    // ── meta (对齐 OpenClaw registry.ts CHAT_CHANNEL_META["telegram"]) ──────
    p.meta.id             = "telegram";
    p.meta.label          = "Telegram";
    p.meta.selection_label = "Telegram (Bot API)";
    p.meta.detail_label   = "Telegram Bot";
    p.meta.docs_path      = "/channels/telegram";
    p.meta.docs_label     = "telegram";
    p.meta.blurb          = "simplest way to get started — register a bot with @BotFather and get going.";
    p.meta.system_image   = "paperplane";
    p.meta.selection_docs_omit_label = true;

    // ── capabilities (对齐 OpenClaw dock.ts DOCKS["telegram"]) ──────────────
    p.capabilities.chat_types      = {"direct", "group", "channel", "thread"};
    p.capabilities.native_commands = true;
    p.capabilities.block_streaming = true;

    // ── outbound (对齐 OpenClaw telegramOutbound) ────────────────────────────
    // Aligned with openclaw:
    //   deliveryMode: "direct"
    //   chunker: markdownToTelegramHtmlChunks
    //   chunkerMode: "markdown"
    //   textChunkLimit: 4000
    //   sendText / sendMedia / sendPayload
    ChannelOutboundAdapter outbound;
    outbound.delivery_mode    = "direct";
    outbound.chunker          = markdown_to_telegram_html_chunks;
    outbound.chunker_mode     = "markdown";
    outbound.text_chunk_limit = 4000;

    // Aligned with openclaw: telegramOutbound.sendText
    outbound.send_text = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        const nlohmann::json& account = ctx.cfg.data.value("channels", nlohmann::json::object())
                                                     .value("telegram", nlohmann::json::object());
        const std::string token = resolve_telegram_token(
            resolve_telegram_account(ctx.cfg, ctx.account_id.value_or("default")),
            ctx.cfg, ctx.account_id.value_or("default")
        );
        if (token.empty()) {
            return core::Result<OutboundDeliveryResult>::Err("no telegram bot token configured");
        }
        return send_telegram_message(token, ctx.to, ctx.text, ctx.reply_to_id, ctx.thread_id, std::nullopt);
    };

    // Aligned with openclaw: telegramOutbound.sendMedia
    outbound.send_media = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        const std::string token = resolve_telegram_token(
            resolve_telegram_account(ctx.cfg, ctx.account_id.value_or("default")),
            ctx.cfg, ctx.account_id.value_or("default")
        );
        if (token.empty()) {
            return core::Result<OutboundDeliveryResult>::Err("no telegram bot token configured");
        }
        return send_telegram_message(token, ctx.to, ctx.text, ctx.reply_to_id, ctx.thread_id, ctx.media_url);
    };

    // Aligned with openclaw: telegramOutbound.sendPayload
    // Handles payload.text + payload.mediaUrl + payload.channelData.telegram.buttons/quoteText
    outbound.send_payload = [](const ChannelOutboundPayloadContext& ctx) -> core::Result<OutboundDeliveryResult> {
        const std::string token = resolve_telegram_token(
            resolve_telegram_account(ctx.cfg, ctx.account_id.value_or("default")),
            ctx.cfg, ctx.account_id.value_or("default")
        );
        if (token.empty()) {
            return core::Result<OutboundDeliveryResult>::Err("no telegram bot token configured");
        }
        const std::string text = ctx.payload.value("text", std::string{});
        std::optional<std::string> media_url;
        if (ctx.payload.contains("mediaUrl") && ctx.payload["mediaUrl"].is_string()) {
            media_url = ctx.payload["mediaUrl"].get<std::string>();
        }
        // Aligned with openclaw: payload.channelData.telegram.buttons / quoteText
        // (buttons = inline keyboard, quoteText = reply quote — not yet implemented in C++)
        return send_telegram_message(token, ctx.to, text, ctx.reply_to_id, ctx.thread_id, media_url);
    };

    p.outbound = outbound;

    // ── config adapter (严格对齐 OpenClaw dock.ts telegram.config) ───────────
    auto config = std::make_shared<ChannelConfigAdapter>();

    // Aligned with openclaw: listTelegramAccountIds(cfg)
    config->list_account_ids = [](const core::Config& cfg) -> std::vector<std::string> {
        return list_telegram_account_ids(cfg);
    };

    // Aligned with openclaw: resolveTelegramAccount(cfg, accountId)
    config->resolve_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        return resolve_telegram_account(cfg, account_id);
    };

    // Aligned with openclaw: inspectTelegramAccount (for status/describe)
    config->inspect_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        auto account = resolve_telegram_account(cfg, account_id);
        const std::string token = resolve_telegram_token(account, cfg, account_id);
        nlohmann::json result = account;
        result["accountId"]   = normalize_account_id(account_id);
        result["token"]       = token;
        result["configured"]  = !token.empty();
        result["tokenStatus"] = token.empty() ? "missing" : "available";
        return result;
    };

    // Aligned with openclaw: isEnabled
    config->is_enabled = [](const nlohmann::json& account, const core::Config& cfg) -> bool {
        // cfg.channels.telegram.enabled !== false && account.enabled !== false
        const auto& j = cfg.data;
        bool channel_enabled = true;
        if (j.contains("channels") && j["channels"].contains("telegram")) {
            const auto& tg = j["channels"]["telegram"];
            if (tg.contains("enabled") && tg["enabled"].is_boolean()) {
                channel_enabled = tg["enabled"].get<bool>();
            }
        }
        bool account_enabled = account.value("enabled", true);
        return channel_enabled && account_enabled;
    };

    // Aligned with openclaw: isConfigured → token must be present
    config->is_configured = [](const nlohmann::json& account, const core::Config& cfg) -> bool {
        // Configured if botToken or tokenFile is present and non-empty
        if (account.contains("botToken") && account["botToken"].is_string() &&
            !account["botToken"].get<std::string>().empty()) {
            return true;
        }
        if (account.contains("tokenFile") && account["tokenFile"].is_string() &&
            !account["tokenFile"].get<std::string>().empty()) {
            return true;
        }
        // env fallback for default account
        const char* env_token = std::getenv("TELEGRAM_BOT_TOKEN");
        if (env_token && *env_token) return true;
        return false;
    };

    // Aligned with openclaw: resolveAllowFrom
    // mapAllowFromEntries(inspectTelegramAccount({ cfg, accountId }).config.allowFrom)
    config->resolve_allow_from = [](const core::Config& cfg, const std::string& account_id) -> std::vector<std::string> {
        auto account = resolve_telegram_account(cfg, account_id);
        if (!account.contains("allowFrom") || !account["allowFrom"].is_array()) {
            return {};
        }
        std::vector<std::string> result;
        for (const auto& entry : account["allowFrom"]) {
            if (entry.is_string()) result.push_back(entry.get<std::string>());
            else if (entry.is_number()) result.push_back(std::to_string(entry.get<int64_t>()));
        }
        return result;
    };

    // Aligned with openclaw: formatAllowFrom → formatAllowFromLowercase with stripPrefixRe: /^(telegram|tg):/i
    config->format_allow_from = [](const core::Config&, const std::string&, const std::vector<std::string>& allow_from) -> std::vector<std::string> {
        std::vector<std::string> result;
        for (auto entry : allow_from) {
            // Strip telegram:/tg: prefix (case-insensitive)
            std::string e_lower = entry;
            std::transform(e_lower.begin(), e_lower.end(), e_lower.begin(), ::tolower);
            for (const auto& prefix : std::vector<std::string>{"telegram:", "tg:"}) {
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
    // resolveOptionalConfigString(inspectTelegramAccount({ cfg, accountId }).config.defaultTo)
    config->resolve_default_to = [](const core::Config& cfg, const std::string& account_id) -> std::optional<std::string> {
        auto account = resolve_telegram_account(cfg, account_id);
        if (account.contains("defaultTo") && account["defaultTo"].is_string()) {
            std::string v = account["defaultTo"].get<std::string>();
            // Trim
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
        if (!j["channels"].contains("telegram")) j["channels"]["telegram"] = nlohmann::json::object();
        auto& tg = j["channels"]["telegram"];
        if (!tg.contains("accounts")) tg["accounts"] = nlohmann::json::object();
        if (!tg["accounts"].contains(norm_id)) tg["accounts"][norm_id] = nlohmann::json::object();
        tg["accounts"][norm_id]["enabled"] = enabled;
        return next;
    };

    // Aligned with openclaw: deleteAccount
    config->delete_account = [](const core::Config& cfg, const std::string& account_id) -> core::Config {
        core::Config next = cfg;
        const std::string norm_id = normalize_account_id(account_id);
        auto& j = next.data;
        if (j.contains("channels") && j["channels"].contains("telegram")) {
            auto& tg = j["channels"]["telegram"];
            if (tg.contains("accounts") && tg["accounts"].contains(norm_id)) {
                tg["accounts"].erase(norm_id);
            }
        }
        return next;
    };

    p.config = config;

    // ── groups (严格对齐 OpenClaw dock.ts telegram.groups) ───────────────────
    ChannelGroupAdapter groups;
    // Aligned with openclaw: resolveTelegramGroupRequireMention
    groups.resolve_require_mention = resolve_telegram_group_require_mention;
    // Aligned with openclaw: resolveTelegramGroupToolPolicy
    groups.resolve_tool_policy = resolve_telegram_group_tool_policy;
    p.groups = groups;

    // ── threading (严格对齐 OpenClaw dock.ts telegram.threading) ─────────────
    ChannelThreadingAdapter threading;

    // Aligned with openclaw: resolveReplyToMode → cfg.channels?.telegram?.replyToMode ?? "off"
    threading.resolve_reply_to_mode = [](const core::Config& cfg, const std::string& /*account_id*/, const std::string& /*chat_type*/) -> std::string {
        const auto& j = cfg.data;
        if (j.contains("channels") && j["channels"].contains("telegram")) {
            const auto& tg = j["channels"]["telegram"];
            if (tg.contains("replyToMode") && tg["replyToMode"].is_string()) {
                return tg["replyToMode"].get<std::string>();
            }
        }
        return "off";
    };

    // Aligned with openclaw: buildToolContext (telegram)
    // Telegram: use MessageThreadId (NOT ReplyToId) for thread — ReplyToId is a message ID
    // and causes invalid message_thread_id in DMs.
    threading.build_tool_context = [](const core::Config& /*cfg*/, const std::string& /*account_id*/, const ChannelThreadingContext& ctx) -> std::optional<ChannelThreadingToolContext> {
        ChannelThreadingToolContext tool_ctx;
        // Aligned with openclaw: currentChannelId = context.To?.trim()
        if (ctx.to.has_value() && !ctx.to->empty()) {
            tool_ctx.current_channel_id = *ctx.to;
        }
        // Aligned with openclaw: currentThreadTs = MessageThreadId (not ReplyToId)
        if (ctx.message_thread_id.has_value() && !ctx.message_thread_id->empty()) {
            tool_ctx.current_thread_ts = *ctx.message_thread_id;
        }
        // Aligned with openclaw: currentMessageId = CurrentMessageId
        if (ctx.current_message_id.has_value() && !ctx.current_message_id->empty()) {
            tool_ctx.current_message_id = *ctx.current_message_id;
        }
        return tool_ctx;
    };

    p.threading = threading;

    // ── gateway (对齐 OpenClaw ChannelGatewayAdapter + telegram/webhook.ts) ──
    ChannelGatewayAdapter gateway;

    // =========================================================================
    // start_account
    // Aligned with openclaw: startTelegramWebhook (telegram/webhook.ts)
    //
    // Full webhook lifecycle:
    //   1. Resolve token + secret from config
    //   2. bot.init (getMe) — validate token
    //   3. createServer + listenHttpServer (dedicated port, separate from Gateway)
    //   4. bot.api.setWebhook(publicUrl, { secret_token, allowed_updates })
    //   5. Serve incoming POST requests:
    //      a. Validate X-Telegram-Bot-Api-Secret-Token (constant-time compare)
    //      b. readJsonBodyWithLimit (1MB, 30s timeout)
    //      c. webhookCallback → dispatchReplyFromConfig
    //   6. On abortSignal: deleteWebhook + server.close + bot.stop
    // =========================================================================
    gateway.start_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        ctx.log->info("[telegram] start_account account_id=" + ctx.account_id);

        // ── Step 1: Resolve token + webhook config from account ──────────────
        // Aligned with openclaw: resolveTelegramAccount({ cfg, accountId })
        const std::string token = resolve_telegram_token(ctx.account, ctx.cfg, ctx.account_id);
        if (token.empty()) {
            ctx.log->error("[telegram] no bot token configured for account=" + ctx.account_id);
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running    = false;
            snap.configured = false;
            snap.last_error = "no bot token configured";
            ctx.set_status(snap);
            return core::Result<void>::Err("no bot token configured");
        }

        // Aligned with openclaw: opts.secret (webhookSecret) — required, non-empty
        const std::string secret = ctx.account.value("webhookSecret", std::string{});
        if (secret.empty()) {
            ctx.log->error("[telegram] webhookSecret is required for webhook mode");
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running    = false;
            snap.last_error = "webhookSecret required";
            ctx.set_status(snap);
            return core::Result<void>::Err(
                "Telegram webhook mode requires a non-empty webhookSecret. "
                "Set channels.telegram.webhookSecret in your config."
            );
        }

        // Aligned with openclaw: opts.path / opts.port / opts.host / opts.publicUrl
        const std::string webhook_path = ctx.account.value("webhookPath", std::string{"/telegram-webhook"});
        const std::string health_path  = ctx.account.value("healthPath",  std::string{"/healthz"});
        const int         port         = ctx.account.value("webhookPort", 8787);
        const std::string host         = ctx.account.value("webhookHost", std::string{"127.0.0.1"});
        const std::string public_url   = ctx.account.value("webhookPublicUrl", std::string{});

        ctx.log->info("[telegram] starting webhook listener on " + host + ":" + std::to_string(port) + webhook_path);

        // ── Step 2: bot.init (getMe) — validate token ────────────────────────
        // Aligned with openclaw: initializeTelegramWebhookBot → bot.init(abortSignal)
        {
            httplib::Client cli("https://api.telegram.org");
            cli.set_connection_timeout(10);
            cli.set_read_timeout(10);
            auto res = cli.Get("/bot" + token + "/getMe");
            if (!res || res->status != 200) {
                std::string err = res ? ("getMe failed: " + res->body) : "getMe: connection failed";
                ctx.log->error("[telegram] " + err);
                ChannelAccountSnapshot snap = ctx.get_status();
                snap.running    = false;
                snap.configured = false;
                snap.last_error = err;
                ctx.set_status(snap);
                return core::Result<void>::Err(err);
            }
            ctx.log->info("[telegram] [getMe] token validated");
        }

        // ── Step 3: createServer + listenHttpServer (dedicated port) ─────────
        // Aligned with openclaw: createServer() + listenHttpServer({ server, port, host })
        // Each channel account gets its OWN HTTP server on a dedicated port,
        // completely separate from the Gateway HTTP server.
        auto server = std::make_shared<httplib::Server>();

        // ── Step 4: Register routes ──────────────────────────────────────────
        server->Get(health_path, [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content("ok", "text/plain");
        });

        server->Post(webhook_path, [token, secret, ctx](
            const httplib::Request& req, httplib::Response& res
        ) {
            // Aligned with openclaw: hasValidTelegramWebhookSecret (constant-time compare)
            const std::string secret_header = req.get_header_value("X-Telegram-Bot-Api-Secret-Token");
            const bool valid_secret = !secret_header.empty() &&
                hunterclaw::security::timing_safe_equal(secret_header, secret);
            if (!valid_secret) {
                // Aligned with openclaw: res.shouldKeepAlive = false; respondText(401)
                res.set_header("Connection", "close");
                res.status = 401;
                res.set_content("unauthorized", "text/plain");
                return;
            }

            // Aligned with openclaw: readJsonBodyWithLimit(req, { maxBytes: 1MB, timeoutMs: 30s })
            if (req.body.size() > 1024 * 1024) {
                res.status = 413;
                res.set_content("payload too large", "text/plain");
                return;
            }

            nlohmann::json body;
            try {
                body = nlohmann::json::parse(req.body);
            } catch (...) {
                res.status = 400;
                res.set_content("invalid json", "text/plain");
                return;
            }

            // Aligned with openclaw: webhookCallback(bot) → handler(body, reply, secretHeader)
            // Parse Telegram Update and dispatch to MessageDispatcher
            std::string text;
            std::string from_id;
            std::string chat_id;
            std::string message_id;

            if (body.contains("message")) {
                const auto& msg = body["message"];
                text       = msg.value("text", std::string{});
                message_id = std::to_string(msg.value("message_id", 0));
                if (msg.contains("from")) {
                    from_id = std::to_string(msg["from"].value("id", 0LL));
                }
                if (msg.contains("chat")) {
                    chat_id = std::to_string(msg["chat"].value("id", 0LL));
                }
            }

            if (text.empty()) {
                // Non-text update — acknowledge silently
                res.status = 200;
                res.set_content("ok", "text/plain");
                return;
            }

            ctx.log->info("[telegram] webhook update: from=" + from_id + " chat=" + chat_id + " text_len=" + std::to_string(text.size()));

            // Aligned with openclaw: dispatchReplyFromConfig(inboundMessage)
            channels::InboundMessage inbound_msg;
            inbound_msg.channel    = "telegram";
            inbound_msg.account_id = ctx.account_id;
            inbound_msg.message_id = message_id;
            inbound_msg.sender_id  = from_id;
            inbound_msg.from_id    = from_id;
            inbound_msg.chat_id    = chat_id;
            inbound_msg.text       = text;

            // Aligned with openclaw: ChatType — Telegram DM vs group
            {
                const std::string chat_type_str = body.contains("message") && body["message"].contains("chat")
                    ? body["message"]["chat"].value("type", std::string{"private"})
                    : "private";
                if (chat_type_str == "private") {
                    inbound_msg.chat_type = routing::ChatType::Direct;
                } else {
                    inbound_msg.chat_type = routing::ChatType::Group;
                }
            }

            // Aligned with openclaw: SenderName
            if (body.contains("message") && body["message"].contains("from")) {
                const auto& from = body["message"]["from"];
                std::string first = from.value("first_name", std::string{});
                std::string last  = from.value("last_name",  std::string{});
                inbound_msg.sender_name = last.empty() ? first : first + " " + last;
            }

            // Aligned with openclaw: ThreadId (message_thread_id for supergroups)
            if (body.contains("message") && body["message"].contains("message_thread_id")) {
                inbound_msg.thread_id = std::to_string(body["message"]["message_thread_id"].get<int64_t>());
            }

            // Aligned with openclaw: ReplyToId
            if (body.contains("message") && body["message"].contains("reply_to_message")) {
                const auto& reply = body["message"]["reply_to_message"];
                inbound_msg.reply_to_id = std::to_string(reply.value("message_id", 0));
            }

            inbound_msg.timestamp = std::chrono::system_clock::now();

            if (ctx.dispatch_inbound) {
                ctx.dispatch_inbound(inbound_msg);
            } else {
                ctx.log->warn("[telegram] dispatch_inbound not wired — message dropped");
            }

            res.status = 200;
            res.set_content("ok", "text/plain");
        });

        // ── Step 5: Start listening ──────────────────────────────────────────
        std::thread listen_thread([server, host, port]() {
            server->listen(host.c_str(), port);
        });
        listen_thread.detach();

        // Wait briefly for server to start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (!server->is_running()) {
            ctx.log->error("[telegram] failed to start webhook HTTP server on " + host + ":" + std::to_string(port));
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running    = false;
            snap.last_error = "failed to start webhook HTTP server";
            ctx.set_status(snap);
            return core::Result<void>::Err("failed to start webhook HTTP server");
        }

        // ── Step 6: Resolve public URL ───────────────────────────────────────
        const std::string resolved_public_url = !public_url.empty()
            ? public_url
            : "http://" + (host == "0.0.0.0" ? std::string{"localhost"} : host)
              + ":" + std::to_string(port) + webhook_path;

        // ── Step 7: setWebhook ───────────────────────────────────────────────
        // Aligned with openclaw: bot.api.setWebhook(publicUrl, { secret_token, allowed_updates })
        {
            httplib::Client cli("https://api.telegram.org");
            cli.set_connection_timeout(10);
            cli.set_read_timeout(30);
            nlohmann::json set_webhook_body = {
                {"url",          resolved_public_url},
                {"secret_token", secret},
                // Aligned with openclaw: resolveTelegramAllowedUpdates()
                {"allowed_updates", nlohmann::json::array({
                    "message", "edited_message", "channel_post", "edited_channel_post",
                    "callback_query", "inline_query", "chosen_inline_result",
                    "my_chat_member", "chat_member", "chat_join_request"
                })}
            };
            auto res = cli.Post("/bot" + token + "/setWebhook", set_webhook_body.dump(), "application/json");
            if (!res || res->status != 200) {
                server->stop();
                std::string err = res ? ("setWebhook failed: " + res->body) : "setWebhook: connection failed";
                ctx.log->error("[telegram] " + err);
                ChannelAccountSnapshot snap = ctx.get_status();
                snap.running    = false;
                snap.last_error = err;
                ctx.set_status(snap);
                return core::Result<void>::Err(err);
            }
            ctx.log->info("[telegram] [setWebhook] advertised to telegram: " + resolved_public_url);
        }

        // Update status: running + connected
        {
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running     = true;
            snap.connected   = true;
            snap.webhook_url  = resolved_public_url;
            snap.webhook_path = webhook_path;
            ctx.set_status(snap);
        }

        ctx.log->info("[telegram] webhook local listener on http://" + host + ":" + std::to_string(port) + webhook_path);
        ctx.log->info("[telegram] webhook advertised to telegram on " + resolved_public_url);

        // ── Step 8: Block until abortSignal ─────────────────────────────────
        // Aligned with openclaw: abortSignal.addEventListener("abort", shutdown)
        while (!ctx.abort_signal->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // ── Step 9: Shutdown ─────────────────────────────────────────────────
        // Aligned with openclaw: shutdown() → deleteWebhook + server.close + bot.stop
        ctx.log->info("[telegram] shutting down webhook for account=" + ctx.account_id);

        // Aligned with openclaw: bot.api.deleteWebhook({ drop_pending_updates: false })
        {
            httplib::Client cli("https://api.telegram.org");
            cli.set_connection_timeout(5);
            cli.set_read_timeout(10);
            cli.Get("/bot" + token + "/deleteWebhook?drop_pending_updates=false");
            ctx.log->info("[telegram] [deleteWebhook] webhook unregistered");
        }

        // Aligned with openclaw: server.close()
        server->stop();

        // Update status: stopped
        {
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running   = false;
            snap.connected = false;
            ctx.set_status(snap);
        }

        ctx.log->info("[telegram] webhook stopped for account=" + ctx.account_id);
        return core::Result<void>::Ok();
    };

    gateway.stop_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        // Aligned with openclaw: stopAccount — signal abort, webhook shutdown handled in startAccount
        ctx.log->info("[telegram] stop_account account_id=" + ctx.account_id);
        ChannelAccountSnapshot snapshot = ctx.get_status();
        snapshot.running   = false;
        snapshot.connected = false;
        ctx.set_status(snapshot);
        return core::Result<void>::Ok();
    };

    // Aligned with openclaw: logoutAccount → deleteWebhook + clear token
    gateway.logout_account = [](const ChannelLogoutContext& ctx) -> core::Result<ChannelLogoutResult> {
        const std::string token = ctx.account.value("token", std::string{});
        if (!token.empty()) {
            httplib::Client cli("https://api.telegram.org");
            cli.set_connection_timeout(5);
            cli.set_read_timeout(10);
            cli.Get("/bot" + token + "/deleteWebhook?drop_pending_updates=false");
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
static const ChannelPlugin TELEGRAM_PLUGIN = make_telegram_plugin();

} // namespace

// Register on library load
static struct TelegramPluginRegistrar {
    TelegramPluginRegistrar() {
        ChannelPluginRegistry::instance().register_plugin(TELEGRAM_PLUGIN, 0);
    }
} _telegram_registrar;

} // namespace channels
} // namespace hunterclaw
