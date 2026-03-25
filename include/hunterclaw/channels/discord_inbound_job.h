// Aligned with openclaw: src/discord/monitor/inbound-job.ts
#pragma once

#include "hunterclaw/channels/plugin.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <optional>
#include <memory>
#include <atomic>

namespace hunterclaw {
namespace channels {

// ============================================================================
// DiscordInboundJobPayload
// Aligned with OpenClaw: DiscordInboundJobPayload (inbound-job.ts)
//
// The serializable, non-runtime portion of a Discord inbound message context.
// Separated from runtime fields so the job can be queued safely.
// ============================================================================
struct DiscordInboundJobPayload {
    // Routing
    std::string account_id;
    std::string message_channel_id;
    std::optional<std::string> session_key;       // route.sessionKey
    std::optional<std::string> base_session_key;  // baseSessionKey

    // Message data (sanitized — no live channel reference)
    nlohmann::json message;   // sanitized Discord message object
    nlohmann::json data;      // raw event data (message field sanitized)

    // Thread channel info (normalized — no live references)
    struct ThreadChannelInfo {
        std::string id;
        std::optional<std::string> name;
        std::optional<std::string> parent_id;
        std::optional<std::string> owner_id;
        struct ParentInfo {
            std::string id;
            std::optional<std::string> name;
        };
        std::optional<ParentInfo> parent;
    };
    std::optional<ThreadChannelInfo> thread_channel;  // null if not a thread

    // Access context (from inbound-context.ts)
    std::optional<std::string> group_system_prompt;
    std::optional<std::vector<std::string>> untrusted_context;
    std::optional<std::vector<std::string>> owner_allow_from;

    // Preflight results
    std::string channel = "discord";
    std::optional<std::string> agent_id;
    std::optional<std::string> guild_id;
    std::optional<std::string> bot_user_id;
    bool is_direct_message = false;
    bool is_group_dm = false;
    bool is_guild_message = false;
    bool is_thread = false;
};

// ============================================================================
// DiscordInboundJobRuntime
// Aligned with OpenClaw: DiscordInboundJobRuntime (inbound-job.ts)
//
// Non-serializable runtime fields injected at materialization time.
// ============================================================================
struct DiscordInboundJobRuntime {
    // Aligned with openclaw: runtime (RuntimeEnv)
    std::shared_ptr<spdlog::logger> logger;
    // Aligned with openclaw: abortSignal
    std::shared_ptr<std::atomic<bool>> abort_signal;
    // Aligned with openclaw: cfg
    core::Config cfg;
    // Aligned with openclaw: accountId
    std::string account_id;
    // Aligned with openclaw: dispatchInbound — injected by manager
    std::function<void(const InboundMessage&)> dispatch_inbound;
};

// ============================================================================
// DiscordInboundJob
// Aligned with OpenClaw: DiscordInboundJob (inbound-job.ts)
// ============================================================================
struct DiscordInboundJob {
    // Aligned with openclaw: queueKey — per-session serialization key
    std::string queue_key;
    DiscordInboundJobPayload payload;
    DiscordInboundJobRuntime runtime;
};

// ============================================================================
// resolveDiscordInboundJobQueueKey
// Aligned with OpenClaw: resolveDiscordInboundJobQueueKey (inbound-job.ts)
//
// sessionKey → baseSessionKey → messageChannelId (fallback)
// ============================================================================
inline std::string resolve_discord_inbound_job_queue_key(
    const DiscordInboundJobPayload& payload)
{
    if (payload.session_key && !payload.session_key->empty()) {
        std::string key = *payload.session_key;
        // trim
        while (!key.empty() && std::isspace((unsigned char)key.front())) key.erase(key.begin());
        while (!key.empty() && std::isspace((unsigned char)key.back()))  key.pop_back();
        if (!key.empty()) return key;
    }
    if (payload.base_session_key && !payload.base_session_key->empty()) {
        std::string key = *payload.base_session_key;
        while (!key.empty() && std::isspace((unsigned char)key.front())) key.erase(key.begin());
        while (!key.empty() && std::isspace((unsigned char)key.back()))  key.pop_back();
        if (!key.empty()) return key;
    }
    return payload.message_channel_id;
}

} // namespace channels
} // namespace hunterclaw
