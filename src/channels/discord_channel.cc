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
// Discord ChannelPlugin descriptor (对齐 OpenClaw discord plugin)
// ---------------------------------------------------------------------------

ChannelPlugin make_discord_plugin() {
    ChannelPlugin p;
    p.id = "discord";

    // meta (对齐 OpenClaw registry.ts CHAT_CHANNEL_META["discord"])
    p.meta.id             = "discord";
    p.meta.label          = "Discord";
    p.meta.selection_label = "Discord (Bot API)";
    p.meta.detail_label   = "Discord Bot";
    p.meta.docs_path      = "/channels/discord";
    p.meta.docs_label     = "discord";
    p.meta.blurb          = "very well supported right now.";
    p.meta.system_image   = "bubble.left.and.bubble.right";

    // capabilities (对齐 OpenClaw dock.ts DOCKS["discord"])
    p.capabilities.chat_types      = {"direct", "channel", "thread"};
    p.capabilities.polls           = true;
    p.capabilities.reactions       = true;
    p.capabilities.media           = true;
    p.capabilities.native_commands = true;
    p.capabilities.threads         = true;

    // outbound (对齐 OpenClaw discord: textChunkLimit: 2000)
    ChannelOutboundAdapter outbound;
    outbound.delivery_mode    = "direct";
    outbound.text_chunk_limit = 2000;
    outbound.send_text = [](const ChannelOutboundContext& ctx) -> core::Result<OutboundDeliveryResult> {
        get_logger()->info("[discord] send_text to={} len={}", ctx.to, ctx.text.size());
        // TODO: implement real Discord API call
        OutboundDeliveryResult result;
        result.channel    = "discord";
        result.message_id = "";
        return core::Result<OutboundDeliveryResult>::Ok(result);
    };
    p.outbound = outbound;

    // streaming (对齐 OpenClaw DEFAULT_BLOCK_STREAMING_COALESCE)
    ChannelStreamingAdapter streaming;
    streaming.block_streaming_coalesce_defaults = {1500, 1000};
    p.streaming = streaming;

    // elevated (对齐 OpenClaw dock.ts discord.elevated)
    ChannelElevatedAdapter elevated;
    elevated.allow_from_fallback = [](const core::Config& cfg, const std::string& account_id) -> std::optional<std::vector<std::string>> {
        // TODO: cfg.channels?.discord?.allowFrom ?? cfg.channels?.discord?.dm?.allowFrom
        return std::nullopt;
    };
    p.elevated = elevated;

    // config adapter (对齐 OpenClaw dock.ts discord.config)
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
        return account.contains("token") || account.contains("botToken");
    };
    config->resolve_allow_from = [](const core::Config& cfg, const std::string& account_id) -> std::vector<std::string> {
        return {};
    };
    config->format_allow_from = [](const core::Config&, const std::string&, const std::vector<std::string>& allow_from) -> std::vector<std::string> {
        // Discord: strip <@!?...> and discord:/user:/pk: prefixes, lowercase
        // (对齐 OpenClaw formatDiscordAllowFrom)
        std::vector<std::string> result;
        for (auto entry : allow_from) {
            // Strip leading <@!? and trailing >
            if (!entry.empty() && entry.front() == '<') {
                auto pos = entry.find('>');
                if (pos != std::string::npos) {
                    entry = entry.substr(2, pos - 2);
                    if (!entry.empty() && entry.front() == '!') entry = entry.substr(1);
                }
            }
            // Strip prefixes
            for (const auto& prefix : {"discord:", "user:", "pk:"}) {
                std::string p_lower = prefix;
                std::string e_lower = entry;
                std::transform(e_lower.begin(), e_lower.end(), e_lower.begin(), ::tolower);
                if (e_lower.rfind(p_lower, 0) == 0) {
                    entry = entry.substr(std::string(prefix).size());
                }
            }
            // Trim and lowercase
            while (!entry.empty() && std::isspace(entry.front())) entry.erase(entry.begin());
            while (!entry.empty() && std::isspace(entry.back())) entry.pop_back();
            std::transform(entry.begin(), entry.end(), entry.begin(), ::tolower);
            if (!entry.empty()) result.push_back(entry);
        }
        return result;
    };
    config->resolve_default_to = [](const core::Config& cfg, const std::string& account_id) -> std::optional<std::string> {
        return std::nullopt;
    };
    p.config = config;

    // groups (对齐 OpenClaw dock.ts discord.groups)
    ChannelGroupAdapter groups;
    groups.resolve_require_mention = [](const ChannelGroupContext& ctx) -> std::optional<bool> {
        // TODO: resolveDiscordGroupRequireMention
        return std::nullopt;
    };
    groups.resolve_tool_policy = [](const ChannelGroupContext& ctx) -> std::optional<nlohmann::json> {
        // TODO: resolveDiscordGroupToolPolicy
        return std::nullopt;
    };
    p.groups = groups;

    // mentions (对齐 OpenClaw dock.ts discord.mentions)
    ChannelMentionAdapter mentions;
    mentions.strip_patterns = [](const nlohmann::json&, const core::Config&, const std::string&) -> std::vector<std::string> {
        return {"<@!?\\d+>"};
    };
    p.mentions = mentions;

    // threading (对齐 OpenClaw dock.ts discord.threading)
    ChannelThreadingAdapter threading;
    threading.resolve_reply_to_mode = [](const core::Config& cfg, const std::string& account_id, const std::string&) -> std::string {
        // TODO: cfg.channels?.discord?.replyToMode ?? "off"
        return "off";
    };
    threading.build_tool_context = [](const core::Config& cfg, const std::string& account_id, const ChannelThreadingContext& ctx) -> std::optional<ChannelThreadingToolContext> {
        ChannelThreadingToolContext tool_ctx;
        tool_ctx.current_channel_id = ctx.to;
        tool_ctx.current_thread_ts  = ctx.reply_to_id;
        return tool_ctx;
    };
    p.threading = threading;

    // gateway (对齐 OpenClaw ChannelGatewayAdapter)
    ChannelGatewayAdapter gateway;
    gateway.start_account = [](const ChannelGatewayContext& ctx) -> core::Result<void> {
        ctx.log->info("[discord] start_account account_id=" + ctx.account_id);
        // TODO: implement Discord Gateway connection
        ChannelAccountSnapshot snapshot;
        snapshot.account_id = ctx.account_id;
        snapshot.running    = true;
        snapshot.connected  = false;
        ctx.set_status(snapshot);
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
    p.gateway = gateway;

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
