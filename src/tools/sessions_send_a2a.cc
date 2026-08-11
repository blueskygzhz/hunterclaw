// ============================================================================
// sessions_send A2A Flow 实现 — 严格对齐 OpenClaw sessions-send-tool.a2a.ts
// ============================================================================

#include "hunterclaw/tools/sessions_send_a2a.h"
#include "hunterclaw/tools/agent_step.h"
#include "hunterclaw/tools/a2a_context.h"
#include "hunterclaw/tools/support_tokens.h"

#include <chrono>
#include <random>
#include <sstream>
#include <thread>
#include <spdlog/spdlog.h>

namespace {
    std::shared_ptr<spdlog::logger> a2a_log() {
        static auto logger = spdlog::get("sessions.a2a");
        if (!logger) logger = spdlog::stdout_logger_mt("sessions.a2a");
        return logger;
    }
}

namespace hunterclaw {
namespace tools {

namespace {

    const std::string SESSIONS_SEND_TOOL = "sessions_send";
    const std::string AGENT_LANE_NESTED = "nested";
    const std::string INTERNAL_MESSAGE_CHANNEL = "internal";

    // ── UUID generation ─────────────────────────────────────────────────

    std::string generate_uuid_v4() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<int> hex_dis(0, 15);
        const char* hex = "0123456789abcdef";
        std::string uuid(36, '-');
        for (size_t i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) continue;
            if (i == 14) { uuid[i] = '4'; continue; }
            if (i == 19) { uuid[i] = hex[(hex_dis(gen) & 0x3) | 0x8]; continue; }
            uuid[i] = hex[hex_dis(gen)];
        }
        return uuid;
    }

    // ── Wait helpers ────────────────────────────────────────────────────

    bool wait_for_agent_completion(
        std::shared_ptr<gateway::GatewayClient> gw,
        const std::string& run_id,
        int timeout_ms)
    {
        if (!gw || run_id.empty()) return false;
        try {
            auto result = gw->request<nlohmann::json>("agent.wait", nlohmann::json{
                {"runId", run_id},
                {"timeoutMs", timeout_ms}
            });
            if (!result.is_ok()) return false;
            const auto& w = result.get();
            std::string status = w.value("status", "");
            return (status == "ok" || status == "completed");
        } catch (const std::exception& e) {
            a2a_log()->warn("a2a_flow: agent.wait failed for {}: {}", run_id, e.what());
            return false;
        }
    }

    std::string fetch_last_assistant_reply(
        std::shared_ptr<gateway::GatewayClient> gw,
        const std::string& session_key)
    {
        if (!gw) return "";
        try {
            auto result = gw->request<nlohmann::json>("chat.history", nlohmann::json{
                {"sessionKey", session_key},
                {"limit", 50}
            });
            if (!result.is_ok()) return "";

            auto raw_msgs = result.get().value("messages", nlohmann::json::array());
            for (int i = (int)raw_msgs.size() - 1; i >= 0; --i) {
                const auto& msg = raw_msgs[i];
                if (!msg.is_object()) continue;
                if (msg.value("role", "") != "assistant") continue;

                std::string text;
                if (msg.contains("content") && msg["content"].is_string()) {
                    text = msg["content"].get<std::string>();
                } else if (msg.contains("content") && msg["content"].is_array()) {
                    for (const auto& block : msg["content"]) {
                        if (block.is_object() && block.value("type", "") == "text") {
                            text += block.value("text", "");
                        }
                    }
                }
                if (!text.empty()) return text;
            }
        } catch (...) {}
        return "";
    }

    // ── Announce target resolver ────────────────────────────────────────
    // Aligned with OpenClaw resolveAnnounceTarget

    struct AnnounceTarget {
        std::string channel;
        std::string to;
        std::string account_id;
        std::string thread_id;
    };

    AnnounceTarget resolve_announce_target(
        std::shared_ptr<gateway::GatewayClient> gw,
        const std::string& session_key,
        const std::string& display_key)
    {
        AnnounceTarget target;
        try {
            auto list_result = gw->request<nlohmann::json>("sessions.list", nlohmann::json{
                {"includeGlobal", true},
                {"includeUnknown", true},
                {"limit", 200}
            });
            if (!list_result.is_ok()) return target;

            auto sessions = list_result.get().value("sessions", nlohmann::json::array());

            // Find session by key (prefer session_key, fallback to display_key)
            nlohmann::json match;
            for (const auto& entry : sessions) {
                if (entry.value("key", "") == session_key) { match = entry; break; }
            }
            if (match.empty()) {
                for (const auto& entry : sessions) {
                    if (entry.value("key", "") == display_key) { match = entry; break; }
                }
            }

            // Extract delivery context
            if (!match.empty() && match.contains("deliveryContext") && match["deliveryContext"].is_object()) {
                const auto& dc = match["deliveryContext"];
                if (!dc.value("channel", "").empty() && !dc.value("to", "").empty()) {
                    target.channel    = dc.value("channel", "");
                    target.to         = dc.value("to", "");
                    target.account_id = dc.value("accountId", "");
                    target.thread_id  = dc.value("threadId", "");
                    return target;
                }
            }

            // Fallback: use lastChannel / lastTo / lastAccountId
            if (!match.empty()) {
                target.channel    = match.value("lastChannel", "");
                target.to         = match.value("lastTo", "");
                target.account_id = match.value("lastAccountId", "");
            }
        } catch (const std::exception& e) {
            a2a_log()->warn("a2a_flow: resolve_announce_target failed: {}", e.what());
        }
        return target;
    }

    // ── Announce delivery ───────────────────────────────────────────────
    // Aligned with OpenClaw deliverAnnounceReply

    void deliver_announce_reply(
        std::shared_ptr<gateway::GatewayClient> gw,
        const AnnounceTarget& target,
        const std::string& message)
    {
        if (!gw || message.empty()) return;
        if (target.channel.empty() || target.to.empty()) {
            a2a_log()->warn("a2a_flow: cannot deliver announce — no channel/to");
            return;
        }

        std::string msg = message;
        // Strip control tokens before delivering
        msg.erase(msg.find(support_tokens::ANNOUNCE_SKIP_TOKEN), support_tokens::ANNOUNCE_SKIP_TOKEN.length());
        msg.erase(msg.find(support_tokens::REPLY_SKIP_TOKEN), support_tokens::REPLY_SKIP_TOKEN.length());
        msg.erase(msg.find(support_tokens::SILENT_REPLY_TOKEN), support_tokens::SILENT_REPLY_TOKEN.length());

        nlohmann::json send_params = {
            {"to",          target.to},
            {"message",     msg},
            {"channel",     target.channel},
            {"idempotencyKey", generate_uuid_v4()}
        };
        if (!target.account_id.empty()) send_params["accountId"] = target.account_id;
        if (!target.thread_id.empty())  send_params["threadId"]  = target.thread_id;

        try {
            auto result = gw->request<nlohmann::json>("send", send_params);
            if (!result.is_ok()) {
                a2a_log()->warn("a2a_flow: announce send failed: {}", result.error);
            }
        } catch (const std::exception& e) {
            a2a_log()->warn("a2a_flow: announce send exception: {}", e.what());
        }
    }

    // ── Notify requester of wait failure ────────────────────────────────
    // Aligned with OpenClaw sessions-send-tool.a2a.ts lines 119-138

    void notify_requester_of_wait_failure(
        std::shared_ptr<gateway::GatewayClient> gw,
        const SessionsSendA2AParams& params,
        const std::string& error_text)
    {
        std::string err_suffix = error_text.empty() ? "" : (": " + error_text);
        std::string notify_msg =
            "sessions_send delivery to " + params.display_key + " failed" + err_suffix + ". "
            "The target may not have received the message; retry or report the failure "
            "instead of assuming delivery succeeded.";
        std::string extra_context =
            "A previous sessions_send delivery failed after it was accepted. "
            "Decide whether to retry, use another route, or report the failure. "
            "Do not assume the target received the message.";

        run_agent_step(gw, params.requester_session_key,
            notify_msg, extra_context,
            params.announce_timeout_ms);
    }

}  // anonymous namespace

// ============================================================================
// Main A2A Flow
// ============================================================================

void run_sessions_send_a2a_flow(
    std::shared_ptr<gateway::GatewayClient> gw,
    const SessionsSendA2AParams& params)
{
    std::string run_context_id = params.wait_run_id.empty()
        ? "unknown" : params.wait_run_id;

    try {
        // ── Step 1: Get round-1 reply ───────────────────────────────────

        std::string primary_reply = params.round_one_reply;
        std::string latest_reply  = params.round_one_reply;

        if (primary_reply.empty() && !params.wait_run_id.empty()) {
            bool completed = wait_for_agent_completion(
                gw, params.wait_run_id,
                std::min(params.announce_timeout_ms, 60000));
            if (completed) {
                latest_reply = fetch_last_assistant_reply(gw, params.target_session_key);
                primary_reply = latest_reply;
            } else {
                if (params.notify_requester_on_wait_failure &&
                    !params.requester_session_key.empty()) {
                    notify_requester_of_wait_failure(gw, params, "agent wait failed");
                }
                return;
            }
        }

        if (latest_reply.empty()) {
            return;
        }

        if (support_tokens::is_non_deliverable_sessions_reply(latest_reply)) {
            return;
        }

        // ── Step 2: Resolve announce target ─────────────────────────────

        auto announce_target = resolve_announce_target(
            gw, params.target_session_key, params.display_key);
        std::string target_channel = announce_target.channel.empty()
            ? "unknown" : announce_target.channel;

        // ── Step 3: Same-session source reply ───────────────────────────
        // Aligned with OpenClaw: sameSessionSourceReply path
        bool same_session = !params.requester_session_key.empty() &&
            params.requester_session_key == params.target_session_key;

        if (same_session) {
            bool can_deliver = announce_target.channel.empty() ? false :
                (params.requester_channel.empty() ||
                 params.requester_channel == announce_target.channel);

            if (can_deliver && !params.round_one_reply.empty()) {
                // Already fetched reply — deliver directly
                deliver_announce_reply(gw, announce_target, latest_reply);
                return;
            }
            if (can_deliver) {
                // waitRunId set but roundOneReply not pre-fetched → skip (openclaw lines 163-166)
                return;
            }
            if (!can_deliver && announce_target.channel.empty()) {
                return;
            }
        }

        // ── Step 4: Ping-pong loop ──────────────────────────────────────
        // Aligned with OpenClaw: maxPingPongTurns > 0 && requesterSessionKey

        if (params.max_ping_pong_turns > 0 &&
            !params.requester_session_key.empty() &&
            !same_session)
        {
            std::string current_key = params.requester_session_key;
            std::string next_key    = params.target_session_key;
            std::string incoming    = latest_reply;
            std::string target_display = params.display_key;
            std::string requester_display; // could be resolved via sessions.list

            for (int turn = 1; turn <= params.max_ping_pong_turns; ++turn) {
                std::string reply_context = build_agent_to_agent_reply_context(
                    turn, params.max_ping_pong_turns, incoming);

                auto step_result = run_agent_step(
                    gw, current_key, incoming, reply_context,
                    params.announce_timeout_ms);

                if (!step_result.is_ok()) {
                    a2a_log()->warn("a2a_flow: ping-pong step failed turn {}: {}",
                        turn, step_result.error);
                    break;
                }

                std::string reply_text = step_result.get();
                if (reply_text.empty() ||
                    support_tokens::is_reply_skip(reply_text) ||
                    support_tokens::is_non_deliverable_sessions_reply(reply_text)) {
                    break;
                }

                latest_reply = reply_text;
                incoming = reply_text;

                // Swap
                std::swap(current_key, next_key);
            }
        }

        // ── Step 5: Announce ────────────────────────────────────────────

        std::string announce_prompt = build_agent_to_agent_announce_context(
            params.message);

        auto announce_result = run_agent_step(
            gw, params.target_session_key,
            "Agent-to-agent announce step.",
            announce_prompt,
            params.announce_timeout_ms);

        if (announce_result.is_ok()) {
            std::string reply = announce_result.get();
            if (!reply.empty() &&
                !support_tokens::is_announce_skip(reply) &&
                !support_tokens::is_non_deliverable_sessions_reply(reply) &&
                announce_target.channel.empty() == false) {
                deliver_announce_reply(gw, announce_target, reply);
            }
        }

    } catch (const std::exception& e) {
        a2a_log()->warn("a2a_flow: runSessionsSendA2AFlow failed: {}",
            std::string(e.what()));
    }
}

}  // namespace tools
}  // namespace hunterclaw
