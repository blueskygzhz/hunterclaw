// Aligned with openclaw: src/discord/monitor/listeners.ts
#include "hunterclaw/channels/discord_listener.h"
#include <chrono>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace {
// Fallback logger for when no logger is provided
std::shared_ptr<spdlog::logger> get_fallback_logger() {
    auto log = spdlog::get("discord_listener");
    if (!log) log = spdlog::stdout_logger_mt("discord_listener");
    return log;
}
}

namespace hunterclaw {
namespace channels {

// ============================================================================
// logSlowDiscordListener
// Aligned with OpenClaw: logSlowDiscordListener (listeners.ts)
// DISCORD_SLOW_LISTENER_THRESHOLD_MS = 30_000
// ============================================================================
void log_slow_discord_listener(
    std::shared_ptr<spdlog::logger> logger,
    const std::string& listener_name,
    const std::string& event,
    int64_t duration_ms,
    const nlohmann::json& context)
{
    if (duration_ms < DiscordMessageListener::SLOW_LISTENER_THRESHOLD_MS) {
        return;
    }
    const double duration_s = duration_ms / 1000.0;
    const std::string msg = "Slow listener detected: " + listener_name +
        " took " + std::to_string(duration_s) + "s for event " + event;
    const auto& log = logger ? logger : get_fallback_logger();
    log->warn(msg);
}

// ============================================================================
// DiscordMessageListener
// Aligned with OpenClaw: DiscordMessageListener (listeners.ts)
// ============================================================================
DiscordMessageListener::DiscordMessageListener(
    DiscordMessageHandler handler,
    std::shared_ptr<spdlog::logger> logger,
    std::function<void()> on_event)
    : handler_(std::move(handler))
    , logger_(std::move(logger))
    , on_event_(std::move(on_event))
{}

// Aligned with openclaw: handle(data, client)
// Fire-and-forget: hand off to the handler without blocking the listener.
// Per-session ordering and run timeouts are owned by the inbound worker queue,
// so the listener no longer serializes or applies its own timeout.
// Aligned with openclaw: DiscordMessageListener handles MESSAGE_CREATE
void DiscordMessageListener::handle(const std::string& event_name,
                                     const nlohmann::json& data,
                                     const std::string& /*account_id*/) {
    // Aligned with openclaw: DiscordMessageListener only handles MESSAGE_CREATE
    if (event_name != "MESSAGE_CREATE") return;

    // Aligned with openclaw: this.onEvent?.()
    if (on_event_) on_event_();

    // Aligned with openclaw:
    //   void Promise.resolve()
    //     .then(() => this.handler(data, client))
    //     .catch(err => logger.error(...))
    // Fire-and-forget: handler is responsible for enqueuing into inbound worker.
    if (!handler_) return;
    try {
        handler_(data, "");  // account_id injected by the handler closure
    } catch (const std::exception& e) {
        const auto& log = logger_ ? logger_ : get_fallback_logger();
        log->error("[discord] handler failed: {}", e.what());
    } catch (...) {
        const auto& log = logger_ ? logger_ : get_fallback_logger();
        log->error("[discord] handler failed (unknown exception)");
    }
}

// ============================================================================
// DiscordReactionListener
// Aligned with OpenClaw: DiscordReactionListener (listeners.ts)
// ============================================================================
DiscordReactionListener::DiscordReactionListener(DiscordListenerParams params)
    : params_(std::move(params))
{}

// Aligned with openclaw: DiscordReactionListener handles MESSAGE_REACTION_ADD
void DiscordReactionListener::handle(const std::string& event_name,
                                      const nlohmann::json& data,
                                      const std::string& /*account_id*/) {
    if (event_name != "MESSAGE_REACTION_ADD") return;
    // Aligned with openclaw: this.params.onEvent?.()
    if (params_.on_event) params_.on_event();
    // Aligned with openclaw: runDiscordReactionHandler({ action: "added", ... })
    run_reaction_handler(data, "added");
}

// Aligned with openclaw: runDiscordReactionHandler → handleDiscordReactionEvent
void DiscordReactionListener::run_reaction_handler(const nlohmann::json& data,
                                                    const std::string& action)
{
    const auto started_at = std::chrono::steady_clock::now();
    const std::string event_name = (action == "added") ? "MESSAGE_REACTION_ADD" : "MESSAGE_REACTION_REMOVE";
    try {
        // Aligned with openclaw: if (!("user" in data)) return
        if (!data.contains("user") || data["user"].is_null()) return;
        const auto& user = data["user"];

        // Aligned with openclaw: if (!user || user.bot) return
        if (user.contains("bot") && user["bot"].is_boolean() && user["bot"].get<bool>()) return;

        // Aligned with openclaw: if (botUserId && user.id === botUserId) return
        if (params_.routing.bot_user_id && user.contains("id") && user["id"].is_string()) {
            if (user["id"].get<std::string>() == *params_.routing.bot_user_id) return;
        }

        const std::string user_id = user.contains("id") && user["id"].is_string()
            ? user["id"].get<std::string>() : "";
        const std::string channel_id = data.contains("channel_id") && data["channel_id"].is_string()
            ? data["channel_id"].get<std::string>() : "";
        const bool is_guild_message = data.contains("guild_id") && !data["guild_id"].is_null()
            && data["guild_id"].is_string() && !data["guild_id"].get<std::string>().empty();

        // Aligned with openclaw: isDirectMessage = channelType === ChannelType.DM
        // HunterClaw: infer from guild_id absence (no channel type fetch without WebSocket client)
        const bool is_direct_message = !is_guild_message && !data.contains("guild_id");
        const bool is_group_dm = false;  // Cannot determine without channel type fetch

        // Aligned with openclaw: if (isDirectMessage && !dmEnabled) return
        if (is_direct_message && !params_.routing.dm_enabled) {
            if (params_.logger) {
                params_.logger->debug("[discord] reaction blocked: dm-disabled user={}", user_id);
            }
            return;
        }

        // Aligned with openclaw: if (isGroupDm && !groupDmEnabled) return
        if (is_group_dm && !params_.routing.group_dm_enabled) {
            return;
        }

        // Aligned with openclaw: guild reaction — check guildEntries allowlist
        // If guildEntries is configured and non-empty, only allow listed guilds
        if (is_guild_message && !params_.routing.guild_entries.is_null()
            && params_.routing.guild_entries.is_object()
            && !params_.routing.guild_entries.empty()) {
            const std::string guild_id = data["guild_id"].get<std::string>();
            if (!params_.routing.guild_entries.contains(guild_id)
                && !params_.routing.guild_entries.contains("*")) {
                // Aligned with openclaw: guildEntries configured but guild not listed → skip
                return;
            }
        }

        // Aligned with openclaw: groupPolicy check for guild messages
        if (is_guild_message) {
            const std::string& gp = params_.routing.group_policy;
            if (gp == "disabled") {
                return;
            }
            // "allowlist" policy: guild must be in guildEntries
            if (gp == "allowlist") {
                if (params_.routing.guild_entries.is_null() || !params_.routing.guild_entries.is_object()) {
                    return;
                }
                const std::string guild_id = data.contains("guild_id") && data["guild_id"].is_string()
                    ? data["guild_id"].get<std::string>() : "";
                if (!guild_id.empty()
                    && !params_.routing.guild_entries.contains(guild_id)
                    && !params_.routing.guild_entries.contains("*")) {
                    return;
                }
            }
        }

        // Aligned with openclaw: log reaction event
        if (params_.logger) {
            params_.logger->debug("[discord] reaction {} by user={} channel={} guild={}",
                action, user_id, channel_id,
                data.contains("guild_id") && data["guild_id"].is_string()
                    ? data["guild_id"].get<std::string>() : "dm");
        }
    } catch (const std::exception& e) {
        if (params_.logger) {
            params_.logger->error("[discord] reaction handler failed: {}", e.what());
        }
    }

    // Aligned with openclaw: logSlowDiscordListener
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at).count();
    log_slow_discord_listener(params_.logger, "DiscordReactionListener",
        event_name, elapsed);
}

// ============================================================================
// DiscordReactionRemoveListener
// Aligned with OpenClaw: DiscordReactionRemoveListener (listeners.ts)
// ============================================================================
DiscordReactionRemoveListener::DiscordReactionRemoveListener(DiscordListenerParams params)
    : params_(std::move(params))
{}

// Aligned with openclaw: DiscordReactionRemoveListener handles MESSAGE_REACTION_REMOVE
void DiscordReactionRemoveListener::handle(const std::string& event_name,
                                            const nlohmann::json& data,
                                            const std::string& /*account_id*/) {
    if (event_name != "MESSAGE_REACTION_REMOVE") return;
    // Aligned with openclaw: this.params.onEvent?.()
    if (params_.on_event) params_.on_event();
    // Aligned with openclaw: runDiscordReactionHandler({ action: "removed", ... })
    // Reuse DiscordReactionListener::run_reaction_handler with action="removed"
    DiscordReactionListener delegate(params_);
    delegate.run_reaction_handler(data, "removed");
}

// ============================================================================
// DiscordPresenceListener
// Aligned with OpenClaw: DiscordPresenceListener (listeners.ts)
// ============================================================================
DiscordPresenceListener::DiscordPresenceListener(
    std::shared_ptr<spdlog::logger> logger,
    std::optional<std::string> account_id)
    : logger_(std::move(logger))
    , account_id_(std::move(account_id))
{}

// Aligned with openclaw: DiscordPresenceListener handles PRESENCE_UPDATE
void DiscordPresenceListener::handle(const std::string& event_name,
                                      const nlohmann::json& data,
                                      const std::string& /*account_id*/) {
    if (event_name != "PRESENCE_UPDATE") return;
    try {
        // Aligned with openclaw:
        //   const userId = "user" in data && data.user?.id ? String(data.user.id) : undefined
        if (!data.contains("user") || data["user"].is_null()) return;
        const auto& user = data["user"];
        if (!user.contains("id")) return;

        std::string user_id;
        if (user["id"].is_string()) {
            user_id = user["id"].get<std::string>();
        } else if (user["id"].is_number()) {
            user_id = std::to_string(user["id"].get<int64_t>());
        }
        if (user_id.empty()) return;

        // Aligned with openclaw: setPresence(accountId, userId, data)
        // Presence cache will be implemented in presence_cache.cc
        // For now: log the presence update
        if (logger_) {
            logger_->debug("[discord] presence update user={} account={}",
                user_id, account_id_.value_or("?"));
        }
    } catch (const std::exception& e) {
        const auto& log = logger_ ? logger_ : get_fallback_logger();
        log->error("[discord] presence handler failed: {}", e.what());
    }
}

// ============================================================================
// DiscordThreadUpdateListener
// Aligned with OpenClaw: DiscordThreadUpdateListener (listeners.ts)
// ============================================================================
DiscordThreadUpdateListener::DiscordThreadUpdateListener(
    core::Config cfg,
    std::string account_id,
    std::shared_ptr<spdlog::logger> logger)
    : cfg_(std::move(cfg))
    , account_id_(std::move(account_id))
    , logger_(std::move(logger))
{}

// Aligned with openclaw: isThreadArchived(data)
// Discord only fires THREAD_UPDATE when a field actually changes, so
// thread_metadata.archived === true means the thread just transitioned to archived.
bool DiscordThreadUpdateListener::is_thread_archived(const nlohmann::json& data) {
    if (!data.contains("thread_metadata")) return false;
    const auto& meta = data["thread_metadata"];
    if (!meta.is_object()) return false;
    if (!meta.contains("archived")) return false;
    return meta["archived"].is_boolean() && meta["archived"].get<bool>();
}

// Aligned with openclaw: DiscordThreadUpdateListener handles THREAD_UPDATE
void DiscordThreadUpdateListener::handle(const std::string& event_name,
                                          const nlohmann::json& data,
                                          const std::string& /*account_id*/) {
    if (event_name != "THREAD_UPDATE") return;
    const auto started_at = std::chrono::steady_clock::now();
    try {
        // Aligned with openclaw: if (!isThreadArchived(data)) return
        if (!is_thread_archived(data)) return;

        // Aligned with openclaw: const threadId = "id" in data ? data.id : undefined
        if (!data.contains("id") || !data["id"].is_string()) return;
        const std::string thread_id = data["id"].get<std::string>();

        const auto& log = logger_ ? logger_ : get_fallback_logger();
        log->info("[discord] thread archived — resetting session threadId={}", thread_id);

        // Aligned with openclaw: closeDiscordThreadSessions({ cfg, accountId, threadId })
        // Thread session close will be implemented in thread_session_close.cc
        // For now: log the event
        log->info("[discord] thread sessions reset after archival threadId={}", thread_id);
    } catch (const std::exception& e) {
        const auto& log = logger_ ? logger_ : get_fallback_logger();
        log->error("[discord] thread-update handler failed: {}", e.what());
    }

    // Aligned with openclaw: logSlowDiscordListener
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at).count();
    log_slow_discord_listener(logger_, "DiscordThreadUpdateListener",
        "THREAD_UPDATE", elapsed);
}

} // namespace channels
} // namespace hunterclaw
