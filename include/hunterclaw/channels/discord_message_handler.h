// Aligned with openclaw: src/discord/monitor/message-handler.ts
#pragma once

#include "hunterclaw/channels/discord_inbound_job.h"
#include "hunterclaw/channels/discord_inbound_worker.h"
#include "hunterclaw/channels/discord_listener.h"
#include "hunterclaw/channels/plugin.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <optional>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace channels {

// ============================================================================
// DiscordMessageHandlerParams
// Aligned with OpenClaw: DiscordMessageHandlerParams (message-handler.ts)
// ============================================================================
struct DiscordMessageHandlerParams {
    core::Config cfg;
    std::shared_ptr<spdlog::logger> logger;
    std::string account_id;
    std::optional<std::string> bot_user_id;

    // Aligned with openclaw: setStatus? (DiscordMonitorStatusSink)
    DiscordMonitorStatusSink set_status;
    // Aligned with openclaw: abortSignal?
    std::shared_ptr<std::atomic<bool>> abort_signal;
    // Aligned with openclaw: workerRunTimeoutMs?
    std::optional<int> worker_run_timeout_ms;

    // Routing params (used for preflight — passed through to inbound worker)
    bool dm_enabled = true;
    bool group_dm_enabled = false;
    std::string dm_policy = "pairing";
    std::string group_policy = "open";
    std::vector<std::string> allow_from;
    std::vector<std::string> group_dm_channels;
    bool allow_name_matching = false;
    nlohmann::json guild_entries;  // object: { guildId: {...} }

    // Aligned with openclaw: dispatch_inbound — injected by monitor
    std::function<void(const InboundMessage&)> dispatch_inbound;
};

// ============================================================================
// DiscordMessageHandlerWithLifecycle
// Aligned with OpenClaw: DiscordMessageHandlerWithLifecycle (message-handler.ts)
//
// A DiscordMessageHandler that also owns a DiscordInboundWorker and exposes
// deactivate() to stop the worker when the account is stopped.
// ============================================================================
struct DiscordMessageHandlerWithLifecycle {
    // Aligned with openclaw: handler(data, accountId)
    DiscordMessageHandler handler;
    // Aligned with openclaw: handler.deactivate = inboundWorker.deactivate
    std::function<void()> deactivate;
};

// ============================================================================
// create_discord_message_handler
// Aligned with OpenClaw: createDiscordMessageHandler (message-handler.ts)
//
// Creates a DiscordMessageHandler backed by a DiscordInboundWorker.
// The handler:
//   1. Filters bot-own messages early (before enqueue)
//   2. Builds a DiscordInboundJob from the raw event data
//   3. Enqueues the job into the inbound worker for per-session processing
// ============================================================================
DiscordMessageHandlerWithLifecycle create_discord_message_handler(
    DiscordMessageHandlerParams params);

} // namespace channels
} // namespace hunterclaw
