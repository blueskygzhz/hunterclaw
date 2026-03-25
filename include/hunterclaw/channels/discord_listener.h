// Aligned with openclaw: src/discord/monitor/listeners.ts
#pragma once

#include "hunterclaw/channels/discord_inbound_job.h"
#include "hunterclaw/channels/discord_inbound_worker.h"
#include "hunterclaw/channels/plugin.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace channels {

// ============================================================================
// DiscordMessageHandler
// Aligned with OpenClaw: DiscordMessageHandler (listeners.ts)
//
// Called by DiscordMessageListener when a MESSAGE_CREATE event arrives.
// Signature: void(const nlohmann::json& data, const std::string& account_id)
// ============================================================================
using DiscordMessageHandler =
    std::function<void(const nlohmann::json& data, const std::string& account_id)>;

// ============================================================================
// DiscordReactionRoutingParams
// Aligned with OpenClaw: DiscordReactionRoutingParams (listeners.ts)
// ============================================================================
struct DiscordReactionRoutingParams {
    std::string account_id;
    std::optional<std::string> bot_user_id;
    bool dm_enabled = false;
    bool group_dm_enabled = false;
    std::vector<std::string> group_dm_channels;
    // "open" | "pairing" | "allowlist" | "disabled"
    std::string dm_policy = "open";
    std::vector<std::string> allow_from;
    // "open" | "allowlist" | "disabled"
    std::string group_policy = "open";
    bool allow_name_matching = false;
    // guild_entries: guildId → resolved guild entry (JSON)
    nlohmann::json guild_entries;  // object: { guildId: {...} }
};

// ============================================================================
// DiscordListenerParams
// Aligned with OpenClaw: DiscordReactionListenerParams (listeners.ts)
// ============================================================================
struct DiscordListenerParams {
    core::Config cfg;
    std::shared_ptr<spdlog::logger> logger;
    std::function<void()> on_event;  // Aligned with openclaw: onEvent?()
    DiscordReactionRoutingParams routing;
};

// ============================================================================
// DiscordListenerBase
// Aligned with openclaw: BaseListener (listeners.ts)
//
// Common interface for all Discord event listeners.
// dispatch_discord_event() calls handle(event_name, data, account_id) on each.
// ============================================================================
class DiscordListenerBase {
public:
    virtual ~DiscordListenerBase() = default;
    // Aligned with openclaw: listener.handle(data, client)
    // event_name: e.g. "MESSAGE_CREATE", "MESSAGE_REACTION_ADD", "PRESENCE_UPDATE"
    virtual void handle(const std::string& event_name,
                        const nlohmann::json& data,
                        const std::string& account_id) = 0;
};

// ============================================================================
// DiscordMessageListener
// Aligned with OpenClaw: DiscordMessageListener (listeners.ts)
//
// Receives MESSAGE_CREATE events and dispatches them fire-and-forget to the
// handler (which enqueues into DiscordInboundWorker for per-session ordering).
// ============================================================================
class DiscordMessageListener : public DiscordListenerBase {
public:
    explicit DiscordMessageListener(
        DiscordMessageHandler handler,
        std::shared_ptr<spdlog::logger> logger = nullptr,
        std::function<void()> on_event = nullptr);

    // Aligned with openclaw: handle(data, client)
    // Fire-and-forget: hands off to handler without blocking.
    void handle(const std::string& event_name,
                const nlohmann::json& data,
                const std::string& account_id) override;

    // Aligned with openclaw: DISCORD_SLOW_LISTENER_THRESHOLD_MS = 30_000
    static constexpr int SLOW_LISTENER_THRESHOLD_MS = 30000;

private:
    DiscordMessageHandler handler_;
    std::shared_ptr<spdlog::logger> logger_;
    std::function<void()> on_event_;
};

// ============================================================================
// DiscordReactionListener / DiscordReactionRemoveListener
// Aligned with OpenClaw: DiscordReactionListener / DiscordReactionRemoveListener
// ============================================================================
class DiscordReactionListener : public DiscordListenerBase {
public:
    explicit DiscordReactionListener(DiscordListenerParams params);

    // Aligned with openclaw: handle(data, client) → runDiscordReactionHandler
    void handle(const std::string& event_name,
                const nlohmann::json& data,
                const std::string& account_id) override;

    // Aligned with openclaw: runDiscordReactionHandler → handleDiscordReactionEvent
    // Public so DiscordReactionRemoveListener can reuse it with action="removed"
    void run_reaction_handler(const nlohmann::json& data, const std::string& action);

private:
    DiscordListenerParams params_;
};

class DiscordReactionRemoveListener : public DiscordListenerBase {
public:
    explicit DiscordReactionRemoveListener(DiscordListenerParams params);

    // Aligned with openclaw: handle(data, client) → runDiscordReactionHandler (action="removed")
    void handle(const std::string& event_name,
                const nlohmann::json& data,
                const std::string& account_id) override;

private:
    DiscordListenerParams params_;
};

// ============================================================================
// DiscordPresenceListener
// Aligned with OpenClaw: DiscordPresenceListener (listeners.ts)
//
// Receives PRESENCE_UPDATE events and updates the presence cache.
// ============================================================================
class DiscordPresenceListener : public DiscordListenerBase {
public:
    explicit DiscordPresenceListener(
        std::shared_ptr<spdlog::logger> logger = nullptr,
        std::optional<std::string> account_id = std::nullopt);

    // Aligned with openclaw: handle(data) → setPresence(accountId, userId, data)
    void handle(const std::string& event_name,
                const nlohmann::json& data,
                const std::string& account_id) override;

private:
    std::shared_ptr<spdlog::logger> logger_;
    std::optional<std::string> account_id_;
};

// ============================================================================
// DiscordThreadUpdateListener
// Aligned with OpenClaw: DiscordThreadUpdateListener (listeners.ts)
//
// Receives THREAD_UPDATE events and closes sessions for archived threads.
// ============================================================================
class DiscordThreadUpdateListener : public DiscordListenerBase {
public:
    explicit DiscordThreadUpdateListener(
        core::Config cfg,
        std::string account_id,
        std::shared_ptr<spdlog::logger> logger = nullptr);

    // Aligned with openclaw: handle(data) → closeDiscordThreadSessions if archived
    void handle(const std::string& event_name,
                const nlohmann::json& data,
                const std::string& account_id) override;

private:
    core::Config cfg_;
    std::string account_id_;
    std::shared_ptr<spdlog::logger> logger_;

    // Aligned with openclaw: isThreadArchived(data)
    static bool is_thread_archived(const nlohmann::json& data);
};

// ============================================================================
// registerDiscordListener (utility)
// Aligned with OpenClaw: registerDiscordListener (listeners.ts)
//
// Registers a listener only if no listener of the same type is already present.
// Returns true if registered, false if already present.
// ============================================================================
template <typename TListener>
bool register_discord_listener(std::vector<std::shared_ptr<void>>& listeners,
                                std::shared_ptr<TListener> listener)
{
    for (const auto& existing : listeners) {
        if (std::dynamic_pointer_cast<TListener>(existing)) {
            return false;  // already registered
        }
    }
    listeners.push_back(listener);
    return true;
}

// ============================================================================
// logSlowDiscordListener (utility)
// Aligned with OpenClaw: logSlowDiscordListener (listeners.ts)
// ============================================================================
void log_slow_discord_listener(
    std::shared_ptr<spdlog::logger> logger,
    const std::string& listener_name,
    const std::string& event,
    int64_t duration_ms,
    const nlohmann::json& context = nullptr);

} // namespace channels
} // namespace hunterclaw
