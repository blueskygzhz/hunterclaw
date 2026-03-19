#include "hunterclaw/channels/plugin.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

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

    // gateway (对齐 OpenClaw ChannelGatewayAdapter)
    ChannelGatewayAdapter gateway;
    gateway.start_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        ctx.log->info("[telegram] start_account account_id=" + ctx.account_id);
        // TODO: implement Telegram Bot API polling/webhook
        ChannelAccountSnapshot snapshot;
        snapshot.account_id = ctx.account_id;
        snapshot.running    = true;
        snapshot.connected  = false;
        ctx.set_status(snapshot);
        return core::Result<void>::Ok();
    };
    gateway.stop_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        ctx.log->info("[telegram] stop_account account_id=" + ctx.account_id);
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
