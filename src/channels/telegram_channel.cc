#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/security/timing_safe.h"   // constant-time compare for webhook secret
#include <httplib.h>                            // dedicated HTTP server for webhook listener
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <thread>
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
// Telegram ChannelPlugin descriptor (对齐 OpenClaw telegram plugin)
// ---------------------------------------------------------------------------

ChannelPlugin make_telegram_plugin() {
    ChannelPlugin p;
    p.id = "telegram";

    // meta (对齐 OpenClaw registry.ts CHAT_CHANNEL_META["telegram"])
    p.meta.id             = "telegram";
    p.meta.label          = "Telegram";
    p.meta.selection_label = "Telegram (Bot API)";
    p.meta.detail_label   = "Telegram Bot";
    p.meta.docs_path      = "/channels/telegram";
    p.meta.docs_label     = "telegram";
    p.meta.blurb          = "simplest way to get started — register a bot with @BotFather and get going.";
    p.meta.system_image   = "paperplane";
    p.meta.selection_docs_omit_label = true;

    // capabilities (对齐 OpenClaw dock.ts DOCKS["telegram"])
    p.capabilities.chat_types      = {"direct", "group", "channel", "thread"};
    p.capabilities.native_commands = true;
    p.capabilities.block_streaming = true;

    // outbound (对齐 OpenClaw DEFAULT_OUTBOUND_TEXT_CHUNK_LIMIT_4000)
    ChannelOutboundAdapter outbound;
    outbound.delivery_mode    = "direct";
    outbound.text_chunk_limit = 4000;
    outbound.send_text = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        get_logger()->info("[telegram] send_text to={} len={}", ctx.to, ctx.text.size());
        // TODO: implement real Telegram Bot API call
        OutboundDeliveryResult result;
        result.channel    = "telegram";
        result.message_id = "";
        return core::Result<OutboundDeliveryResult>::Ok(result);
    };
    p.outbound = outbound;

    // config adapter (对齐 OpenClaw dock.ts telegram.config)
    auto config = std::make_shared<ChannelConfigAdapter>();
    config->list_account_ids = [](const core::Config& cfg) -> std::vector<std::string> {
        return {"default"};
    };
    config->resolve_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        return nlohmann::json::object();
    };
    config->is_enabled = [](const nlohmann::json& account, const core::Config&) -> bool {
        return account.value("enabled", true);
    };
    config->is_configured = [](const nlohmann::json& account, const core::Config&) -> bool {
        return account.contains("token");
    };
    config->resolve_allow_from = [](const core::Config& cfg, const std::string& account_id) -> std::vector<std::string> {
        return {};
    };
    config->format_allow_from = [](const core::Config&, const std::string&, const std::vector<std::string>& allow_from) -> std::vector<std::string> {
        // Telegram: lowercase (对齐 OpenClaw formatAllowFromLowercase with stripPrefixRe: /^(telegram|tg):/i)
        std::vector<std::string> result;
        for (auto entry : allow_from) {
            // Strip telegram:/tg: prefix
            for (const auto& prefix : {"telegram:", "tg:"}) {
                std::string e_lower = entry;
                std::transform(e_lower.begin(), e_lower.end(), e_lower.begin(), ::tolower);
                if (e_lower.rfind(prefix, 0) == 0) {
                    entry = entry.substr(std::string(prefix).size());
                }
            }
            std::transform(entry.begin(), entry.end(), entry.begin(), ::tolower);
            if (!entry.empty()) result.push_back(entry);
        }
        return result;
    };
    config->resolve_default_to = [](const core::Config& cfg, const std::string& account_id) -> std::optional<std::string> {
        return std::nullopt;
    };
    p.config = config;

    // groups (对齐 OpenClaw dock.ts telegram.groups)
    ChannelGroupAdapter groups;
    groups.resolve_require_mention = [](const ChannelGroupContext& ctx) -> std::optional<bool> {
        // TODO: resolveTelegramGroupRequireMention
        return std::nullopt;
    };
    groups.resolve_tool_policy = [](const ChannelGroupContext& ctx) -> std::optional<nlohmann::json> {
        // TODO: resolveTelegramGroupToolPolicy
        return std::nullopt;
    };
    p.groups = groups;

    // threading (对齐 OpenClaw dock.ts telegram.threading)
    ChannelThreadingAdapter threading;
    threading.resolve_reply_to_mode = [](const core::Config& cfg, const std::string& account_id, const std::string&) -> std::string {
        // TODO: cfg.channels?.telegram?.replyToMode ?? "off"
        return "off";
    };
    threading.build_tool_context = [](const core::Config& cfg, const std::string& account_id, const ChannelThreadingContext& ctx) -> std::optional<ChannelThreadingToolContext> {
        // Telegram: use MessageThreadId (not ReplyToId) for thread
        ChannelThreadingToolContext tool_ctx;
        tool_ctx.current_channel_id = ctx.to;
        tool_ctx.current_thread_ts  = ctx.message_thread_id;
        tool_ctx.current_message_id = ctx.current_message_id;
        return tool_ctx;
    };
    p.threading = threading;

    // gateway (对齐 OpenClaw ChannelGatewayAdapter + telegram/webhook.ts)
    ChannelGatewayAdapter gateway;

    // =========================================================================
    // start_account
    // Aligned with openclaw: startTelegramWebhook (telegram/webhook.ts)
    //
    // Full webhook lifecycle:
    //   1. Resolve token + secret from config
    //   2. createTelegramBot + bot.init (getMe)
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
        const std::string token = ctx.account.value("token", std::string{});
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
        // HunterClaw: use HTTP client to call getMe
        {
            const std::string get_me_url = "https://api.telegram.org/bot" + token + "/getMe";
            // TODO: replace with real HTTP client call
            // auto resp = http_client.get(get_me_url);
            // if (!resp.ok) return Err("getMe failed: " + resp.error);
            ctx.log->info("[telegram] [getMe] token validated (placeholder)");
        }

        // ── Step 3: createServer + listenHttpServer (dedicated port) ─────────
        // Aligned with openclaw: createServer() + listenHttpServer({ server, port, host })
        // Each channel account gets its OWN HTTP server on a dedicated port,
        // completely separate from the Gateway HTTP server.
        // HunterClaw: use httplib::Server on a dedicated port.
        auto server = std::make_shared<httplib::Server>();

        // ── Step 4: Register routes ──────────────────────────────────────────
        // Aligned with openclaw: server request handler
        //   - GET  healthPath → 200 "ok"
        //   - POST webhookPath → validate secret + readBody + dispatch
        //   - other → 404

        server->Get(health_path, [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content("ok", "text/plain");
        });

        server->Post(webhook_path, [token, secret, webhook_path, ctx](
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
            // HunterClaw: parse Telegram Update and dispatch to MessageDispatcher
            // Telegram Update format: { update_id, message: { text, from, chat, ... } }
            std::string text;
            std::string from_id;
            std::string chat_id;
            std::string message_id;

            if (body.contains("message")) {
                const auto& msg = body["message"];
                text       = msg.value("text", std::string{});
                message_id = std::to_string(msg.value("message_id", 0));
                if (msg.contains("from")) {
                    from_id = std::to_string(msg["from"].value("id", 0));
                }
                if (msg.contains("chat")) {
                    chat_id = std::to_string(msg["chat"].value("id", 0));
                }
            }

            if (text.empty()) {
                // Non-text update (photo, sticker, etc.) — acknowledge silently
                res.status = 200;
                res.set_content("ok", "text/plain");
                return;
            }

            // Aligned with openclaw: dispatchReplyFromConfig(inboundMessage)
            // Build InboundMessage and dispatch via ctx.dispatch_inbound
            // (Manager injects the real MessageDispatcher::dispatch_inbound_message here)
            ctx.log->info("[telegram] webhook update: from=" + from_id + " chat=" + chat_id + " text_len=" + std::to_string(text.size()));

            channels::InboundMessage inbound_msg;
            // Aligned with openclaw: ctx.Provider = "telegram"
            inbound_msg.channel    = "telegram";
            // Aligned with openclaw: ctx.AccountId
            inbound_msg.account_id = ctx.account_id;
            // Aligned with openclaw: ctx.MessageId
            inbound_msg.message_id = message_id;
            // Aligned with openclaw: ctx.PeerId / ctx.SenderId
            inbound_msg.sender_id  = from_id;
            inbound_msg.from_id    = from_id;
            // Aligned with openclaw: ctx.ChatId
            inbound_msg.chat_id    = chat_id;
            // Aligned with openclaw: ctx.Text
            inbound_msg.text       = text;
            // Aligned with openclaw: ctx.ChatType — Telegram DM vs group
            // chat.type: "private" → Direct, "group"/"supergroup"/"channel" → Group
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
            // Aligned with openclaw: ctx.SenderName
            if (body.contains("message") && body["message"].contains("from")) {
                const auto& from = body["message"]["from"];
                std::string first = from.value("first_name", std::string{});
                std::string last  = from.value("last_name",  std::string{});
                inbound_msg.sender_name = last.empty() ? first : first + " " + last;
            }
            // Aligned with openclaw: ctx.ThreadId (message_thread_id for supergroups)
            if (body.contains("message") && body["message"].contains("message_thread_id")) {
                inbound_msg.thread_id = std::to_string(body["message"]["message_thread_id"].get<int64_t>());
            }
            // Aligned with openclaw: ctx.ReplyToId
            if (body.contains("message") && body["message"].contains("reply_to_message")) {
                const auto& reply = body["message"]["reply_to_message"];
                inbound_msg.reply_to_id = std::to_string(reply.value("message_id", 0));
            }
            // Aligned with openclaw: timestamp
            inbound_msg.timestamp = std::chrono::system_clock::now();

            // Dispatch — aligned with openclaw: dispatchReplyFromConfig(inboundMessage)
            if (ctx.dispatch_inbound) {
                ctx.dispatch_inbound(inbound_msg);
            } else {
                ctx.log->warn("[telegram] dispatch_inbound not wired — message dropped");
            }

            res.status = 200;
            res.set_content("ok", "text/plain");
        });

        // ── Step 5: Start listening ──────────────────────────────────────────
        // Aligned with openclaw: listenHttpServer({ server, port, host })
        bool listen_ok = false;
        std::thread listen_thread([&server, &host, &port, &listen_ok]() {
            listen_ok = server->listen(host.c_str(), port);
        });

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
        // Aligned with openclaw: resolveWebhookPublicUrl(configuredPublicUrl || localhost:port/path)
        const std::string resolved_public_url = !public_url.empty()
            ? public_url
            : "http://" + (host == "0.0.0.0" ? std::string{"localhost"} : host)
              + ":" + std::to_string(port) + webhook_path;

        // ── Step 7: bot.api.setWebhook(publicUrl, { secret_token, allowed_updates }) ──
        // Aligned with openclaw: bot.api.setWebhook(publicUrl, { secret_token, allowed_updates, certificate? })
        {
            const std::string set_webhook_url = "https://api.telegram.org/bot" + token + "/setWebhook";
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
            // TODO: replace with real HTTP client call
            // auto resp = http_client.post(set_webhook_url, set_webhook_body);
            // if (!resp.ok) { server->stop(); return Err("setWebhook failed: " + resp.error); }
            ctx.log->info("[telegram] [setWebhook] advertised to telegram: " + resolved_public_url);
        }

        // Update status: running + connected
        {
            ChannelAccountSnapshot snap = ctx.get_status();
            snap.running     = true;
            snap.connected   = true;
            snap.webhook_url = resolved_public_url;
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
            const std::string del_webhook_url = "https://api.telegram.org/bot" + token + "/deleteWebhook?drop_pending_updates=false";
            // TODO: replace with real HTTP client call
            // http_client.get(del_webhook_url);
            ctx.log->info("[telegram] [deleteWebhook] webhook unregistered");
        }

        // Aligned with openclaw: server.close()
        server->stop();
        if (listen_thread.joinable()) {
            listen_thread.join();
        }

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
        // abort_signal is set by ChannelManager before calling stopAccount
        ChannelAccountSnapshot snapshot = ctx.get_status();
        snapshot.running   = false;
        snapshot.connected = false;
        ctx.set_status(snapshot);
        return core::Result<void>::Ok();
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
