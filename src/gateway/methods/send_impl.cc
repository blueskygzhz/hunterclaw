// ============================================================================
// send_impl.cc - Send & Poll Methods
// 🎯 严格对齐 OpenClaw server-methods/send.ts (sendHandlers)
//
// Methods:
//   send  → 发送消息到指定目标 (幂等性去重、渠道解析、会话路由)
//   poll  → 发送投票到指定目标
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/outbound/deliver.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <map>
#include <mutex>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t send_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// Inflight dedup for send (对齐 OpenClaw inflightByContext WeakMap)
// ============================================================================
static std::mutex s_send_dedupe_mutex;
static std::map<std::string, json> s_send_dedupe_cache;    // dedupeKey → {ts, ok, payload, error}
static constexpr int64_t SEND_DEDUPE_TTL_MS = 5 * 60 * 1000;

static void prune_send_dedupe() {
    int64_t now = send_now_ms();
    for (auto it = s_send_dedupe_cache.begin(); it != s_send_dedupe_cache.end();) {
        if (now - it->second.value("ts", (int64_t)0) > SEND_DEDUPE_TTL_MS) {
            it = s_send_dedupe_cache.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// Helper: resolveRequestedChannel
// 🎯 OpenClaw 对齐: send.ts L48-88
// ============================================================================
struct ResolvedChannel {
    bool ok = false;
    std::string channel;
    std::string error;
};

static ResolvedChannel resolve_requested_channel(
    std::shared_ptr<GatewayServer> server,
    const json& params,
    bool reject_webchat_as_internal = false
) {
    std::string channel_input;
    if (params.contains("channel") && params["channel"].is_string()) {
        channel_input = params["channel"].get<std::string>();
        // Trim
        while (!channel_input.empty() && channel_input.front() == ' ') channel_input.erase(0, 1);
        while (!channel_input.empty() && channel_input.back() == ' ') channel_input.pop_back();
    }

    if (channel_input.empty()) {
        // 🎯 OpenClaw: resolveMessageChannelSelection → use default channel
        auto cfg = server->config();
        if (cfg && !cfg->default_channel.empty()) {
            return {true, cfg->default_channel, ""};
        }
        return {false, "", "no default channel configured"};
    }

    // Normalize channel ID
    std::string normalized = channel_input;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);

    if (reject_webchat_as_internal && normalized == "webchat") {
        return {false, "", "unsupported channel: webchat (internal-only). Use `chat.send` for WebChat UI messages or choose a deliverable channel."};
    }

    // Validate channel exists
    auto cfg = server->config();
    if (cfg) {
        auto channel_mgr = server->channel_manager();
        if (channel_mgr) {
            auto ch = channel_mgr->get_channel(normalized);
            if (!ch) {
                return {false, "", "unsupported channel: " + channel_input};
            }
        }
    }

    return {true, normalized, ""};
}

// ============================================================================
// send
// 🎯 OpenClaw 对齐: send.ts L90-333
// ============================================================================
core::Result<json> method_send(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    // ── Parameter extraction ──
    // 🎯 OpenClaw 对齐: send.ts L104-116
    std::string to = params.value("to", "");
    std::string message = params.value("message", "");
    std::string media_url = params.value("mediaUrl", "");
    std::string channel_param = params.value("channel", "");
    std::string account_id = params.value("accountId", "");
    std::string agent_id = params.value("agentId", "");
    std::string thread_id = params.value("threadId", "");
    std::string session_key = params.value("sessionKey", "");
    std::string idempotency_key = params.value("idempotencyKey", "");

    // Trim
    auto trim = [](std::string& s) {
        while (!s.empty() && s.front() == ' ') s.erase(0, 1);
        while (!s.empty() && s.back() == ' ') s.pop_back();
    };
    trim(to); trim(message); trim(media_url);
    trim(account_id); trim(agent_id); trim(thread_id); trim(session_key);

    if (idempotency_key.empty()) {
        return core::Result<json>::Err("invalid send params: idempotencyKey required");
    }

    // ── Idempotency dedup ──
    // 🎯 OpenClaw 对齐: send.ts L117-133
    std::string dedupe_key = "send:" + idempotency_key;
    {
        std::lock_guard<std::mutex> lock(s_send_dedupe_mutex);
        prune_send_dedupe();
        auto it = s_send_dedupe_cache.find(dedupe_key);
        if (it != s_send_dedupe_cache.end()) {
            json cached = it->second;
            cached["cached"] = true;
            return core::Result<json>::Ok(cached);
        }
    }

    // ── Validate content ──
    // 🎯 OpenClaw 对齐: send.ts L145-152
    std::vector<std::string> media_urls;
    if (params.contains("mediaUrls") && params["mediaUrls"].is_array()) {
        for (const auto& entry : params["mediaUrls"]) {
            if (entry.is_string()) {
                std::string u = entry.get<std::string>();
                trim(u);
                if (!u.empty()) media_urls.push_back(u);
            }
        }
    }

    if (message.empty() && media_url.empty() && media_urls.empty()) {
        return core::Result<json>::Err("invalid send params: text or media is required");
    }

    if (to.empty()) {
        return core::Result<json>::Err("invalid send params: to is required");
    }

    // ── Resolve channel ──
    // 🎯 OpenClaw 对齐: send.ts L153-162
    auto resolved_channel = resolve_requested_channel(server, params, true);
    if (!resolved_channel.ok) {
        return core::Result<json>::Err(resolved_channel.error);
    }
    std::string channel = resolved_channel.channel;

    // ── Deliver ──
    // 🎯 OpenClaw 对齐: send.ts L182-333
    try {
        auto channel_mgr = server->channel_manager();
        if (!channel_mgr) {
            return core::Result<json>::Err("channel manager not available");
        }

        auto ch = channel_mgr->get_channel(channel);
        if (!ch) {
            return core::Result<json>::Err("unsupported channel: " + channel);
        }

        // Build outbound payload
        json payload;
        if (!message.empty()) payload["text"] = message;
        if (!media_url.empty()) payload["mediaUrl"] = media_url;
        if (!media_urls.empty()) payload["mediaUrls"] = media_urls;

        // Deliver via channel
        auto delivery_result = ch->send_message(to, payload, {
            {"accountId", account_id},
            {"threadId", thread_id},
        });

        json result;
        result["runId"] = idempotency_key;
        result["channel"] = channel;
        if (delivery_result.contains("messageId")) result["messageId"] = delivery_result["messageId"];
        if (delivery_result.contains("chatId")) result["chatId"] = delivery_result["chatId"];
        if (delivery_result.contains("channelId")) result["channelId"] = delivery_result["channelId"];
        if (delivery_result.contains("toJid")) result["toJid"] = delivery_result["toJid"];
        if (delivery_result.contains("conversationId")) result["conversationId"] = delivery_result["conversationId"];

        // Cache for dedup
        {
            std::lock_guard<std::mutex> lock(s_send_dedupe_mutex);
            json cache_entry = result;
            cache_entry["ts"] = send_now_ms();
            cache_entry["ok"] = true;
            s_send_dedupe_cache[dedupe_key] = cache_entry;
        }

        return core::Result<json>::Ok(result);
    } catch (const std::exception& e) {
        // Cache error for dedup
        {
            std::lock_guard<std::mutex> lock(s_send_dedupe_mutex);
            json cache_entry;
            cache_entry["ts"] = send_now_ms();
            cache_entry["ok"] = false;
            cache_entry["error"] = e.what();
            s_send_dedupe_cache[dedupe_key] = cache_entry;
        }
        return core::Result<json>::Err(std::string(e.what()));
    }
}

// ============================================================================
// send.message (alias for send)
// ============================================================================
core::Result<json> method_send_message(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    return method_send(server, method_guard, client_ctx, params);
}

// ============================================================================
// poll
// 🎯 OpenClaw 对齐: send.ts L334-483
// ============================================================================
core::Result<json> method_poll(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    // Parameter extraction
    std::string to = params.value("to", "");
    std::string question = params.value("question", "");
    std::string idempotency_key = params.value("idempotencyKey", "");
    std::string channel_param = params.value("channel", "");
    std::string account_id = params.value("accountId", "");
    std::string thread_id = params.value("threadId", "");
    int max_selections = params.value("maxSelections", 1);
    int duration_seconds = params.value("durationSeconds", 0);
    bool silent = params.value("silent", false);
    bool is_anonymous = params.value("isAnonymous", false);

    auto trim = [](std::string& s) {
        while (!s.empty() && s.front() == ' ') s.erase(0, 1);
        while (!s.empty() && s.back() == ' ') s.pop_back();
    };
    trim(to); trim(question); trim(account_id); trim(thread_id);

    if (idempotency_key.empty()) {
        return core::Result<json>::Err("invalid poll params: idempotencyKey required");
    }
    if (to.empty() || question.empty()) {
        return core::Result<json>::Err("invalid poll params: to and question required");
    }

    // Options
    std::vector<std::string> options;
    if (params.contains("options") && params["options"].is_array()) {
        for (const auto& opt : params["options"]) {
            if (opt.is_string()) options.push_back(opt.get<std::string>());
        }
    }
    if (options.size() < 2) {
        return core::Result<json>::Err("invalid poll params: at least 2 options required");
    }

    // Dedup check
    std::string dedupe_key = "poll:" + idempotency_key;
    {
        std::lock_guard<std::mutex> lock(s_send_dedupe_mutex);
        auto it = s_send_dedupe_cache.find(dedupe_key);
        if (it != s_send_dedupe_cache.end()) {
            json cached = it->second;
            cached["cached"] = true;
            return core::Result<json>::Ok(cached);
        }
    }

    // Resolve channel
    auto resolved_channel = resolve_requested_channel(server, params, false);
    if (!resolved_channel.ok) {
        return core::Result<json>::Err(resolved_channel.error);
    }
    std::string channel = resolved_channel.channel;

    // 🎯 OpenClaw 对齐: durationSeconds/isAnonymous are Telegram-only (send.ts L379-397)
    if (duration_seconds > 0 && channel != "telegram") {
        return core::Result<json>::Err("durationSeconds is only supported for Telegram polls");
    }
    if (is_anonymous && channel != "telegram") {
        return core::Result<json>::Err("isAnonymous is only supported for Telegram polls");
    }

    // Deliver poll via channel
    try {
        auto channel_mgr = server->channel_manager();
        if (!channel_mgr) {
            return core::Result<json>::Err("channel manager not available");
        }
        auto ch = channel_mgr->get_channel(channel);
        if (!ch) {
            return core::Result<json>::Err("unsupported poll channel: " + channel);
        }

        json poll_payload;
        poll_payload["question"] = question;
        poll_payload["options"] = options;
        poll_payload["maxSelections"] = max_selections;
        if (duration_seconds > 0) poll_payload["durationSeconds"] = duration_seconds;
        if (silent) poll_payload["silent"] = true;
        if (is_anonymous) poll_payload["isAnonymous"] = true;

        auto delivery_result = ch->send_poll(to, poll_payload, {
            {"accountId", account_id},
            {"threadId", thread_id},
        });

        json result;
        result["runId"] = idempotency_key;
        result["channel"] = channel;
        if (delivery_result.contains("messageId")) result["messageId"] = delivery_result["messageId"];
        if (delivery_result.contains("pollId")) result["pollId"] = delivery_result["pollId"];

        // Cache
        {
            std::lock_guard<std::mutex> lock(s_send_dedupe_mutex);
            json cache_entry = result;
            cache_entry["ts"] = send_now_ms();
            cache_entry["ok"] = true;
            s_send_dedupe_cache[dedupe_key] = cache_entry;
        }
        return core::Result<json>::Ok(result);
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lock(s_send_dedupe_mutex);
            json cache_entry;
            cache_entry["ts"] = send_now_ms();
            cache_entry["ok"] = false;
            cache_entry["error"] = e.what();
            s_send_dedupe_cache[dedupe_key] = cache_entry;
        }
        return core::Result<json>::Err(std::string(e.what()));
    }
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
