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
// Slack ChannelPlugin descriptor (对齐 OpenClaw slack plugin)
// ---------------------------------------------------------------------------

ChannelPlugin make_slack_plugin() {
    ChannelPlugin p;
    p.id = "slack";

    // meta (对齐 OpenClaw registry.ts CHAT_CHANNEL_META["slack"])
    p.meta.id             = "slack";
    p.meta.label          = "Slack";
    p.meta.selection_label = "Slack (Socket Mode)";
    p.meta.detail_label   = "Slack Bot";
    p.meta.docs_path      = "/channels/slack";
    p.meta.docs_label     = "slack";
    p.meta.blurb          = "supported (Socket Mode).";
    p.meta.system_image   = "number";

    // capabilities (对齐 OpenClaw dock.ts DOCKS["slack"])
    p.capabilities.chat_types     = {"direct", "channel", "thread"};
    p.capabilities.reactions      = true;
    p.capabilities.media          = true;
    p.capabilities.native_commands = true;
    p.capabilities.threads        = true;

    // outbound (对齐 OpenClaw DEFAULT_OUTBOUND_TEXT_CHUNK_LIMIT_4000)
    ChannelOutboundAdapter outbound;
    outbound.delivery_mode    = "direct";
    outbound.text_chunk_limit = 4000;
    outbound.send_text = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        get_logger()->info("[slack] send_text to={} len={}", ctx.to, ctx.text.size());
        // TODO: implement real Slack Web API call
        OutboundDeliveryResult result;
        result.channel    = "slack";
        result.message_id = "";
        return core::Result<OutboundDeliveryResult>::Ok(result);
    };
    p.outbound = outbound;

    // streaming (对齐 OpenClaw DEFAULT_BLOCK_STREAMING_COALESCE)
    ChannelStreamingAdapter streaming;
    streaming.block_streaming_coalesce_defaults = {1500, 1000};
    p.streaming = streaming;

    // config adapter (对齐 OpenClaw dock.ts slack.config)
    auto config = std::make_shared<ChannelConfigAdapter>();
    config->list_account_ids = [](const core::Config& cfg) -> std::vector<std::string> {
        // TODO: read cfg.channels.slack.accounts
        return {"default"};
    };
    config->resolve_account = [](const core::Config& cfg, const std::string& account_id) -> nlohmann::json {
        // TODO: inspect slack account from config
        return nlohmann::json::object();
    };
    config->is_enabled = [](const nlohmann::json& account, const core::Config&) -> bool {
        return account.value("enabled", true);
    };
    config->is_configured = [](const nlohmann::json& account, const core::Config&) -> bool {
        // Configured if botToken or token is present
        return account.contains("botToken") || account.contains("token");
    };
    config->resolve_allow_from = [](const core::Config& cfg, const std::string& account_id) -> std::vector<std::string> {
        // TODO: read cfg.channels.slack.allowFrom
        return {};
    };
    config->format_allow_from = [](const core::Config&, const std::string&, const std::vector<std::string>& allow_from) -> std::vector<std::string> {
        // Slack: lowercase (对齐 OpenClaw formatAllowFromLowercase)
        std::vector<std::string> result;
        for (const auto& entry : allow_from) {
            std::string lower = entry;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (!lower.empty()) result.push_back(lower);
        }
        return result;
    };
    config->resolve_default_to = [](const core::Config& cfg, const std::string& account_id) -> std::optional<std::string> {
        // TODO: read cfg.channels.slack.defaultTo
        return std::nullopt;
    };
    p.config = config;

    // groups (对齐 OpenClaw dock.ts slack.groups)
    ChannelGroupAdapter groups;
    groups.resolve_require_mention = [](const ChannelGroupContext& ctx) -> std::optional<bool> {
        // TODO: resolveSlackGroupRequireMention
        return std::nullopt;
    };
    groups.resolve_tool_policy = [](const ChannelGroupContext& ctx) -> std::optional<nlohmann::json> {
        // TODO: resolveSlackGroupToolPolicy
        return std::nullopt;
    };
    p.groups = groups;

    // mentions (对齐 OpenClaw dock.ts slack.mentions)
    ChannelMentionAdapter mentions;
    mentions.strip_patterns = [](const nlohmann::json&, const core::Config&, const std::string&) -> std::vector<std::string> {
        return {"<@[^>]+>"};
    };
    p.mentions = mentions;

    // threading (对齐 OpenClaw dock.ts slack.threading)
    ChannelThreadingAdapter threading;
    threading.allow_explicit_reply_tags_when_off = false;
    threading.resolve_reply_to_mode = [](const core::Config& cfg, const std::string& account_id, const std::string& chat_type) -> std::string {
        // TODO: resolveSlackReplyToMode
        return "off";
    };
    threading.build_tool_context = [](const core::Config& cfg, const std::string& account_id, const ChannelThreadingContext& ctx) -> std::optional<ChannelThreadingToolContext> {
        // TODO: buildSlackThreadingToolContext
        ChannelThreadingToolContext tool_ctx;
        tool_ctx.current_channel_id = ctx.to;
        tool_ctx.current_thread_ts  = ctx.reply_to_id;
        return tool_ctx;
    };
    p.threading = threading;

    // gateway (对齐 OpenClaw ChannelGatewayAdapter)
    ChannelGatewayAdapter gateway;
    gateway.start_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        ctx.log->info("[slack] start_account account_id=" + ctx.account_id);
        // TODO: implement Slack Socket Mode connection
        ChannelAccountSnapshot snapshot;
        snapshot.account_id = ctx.account_id;
        snapshot.running    = true;
        snapshot.connected  = false;
        ctx.set_status(snapshot);
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
