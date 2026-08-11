// ============================================================================
// Agent Step 实现 — 严格对齐 OpenClaw agent-step.ts
// ============================================================================

#include "hunterclaw/tools/agent_step.h"
#include "hunterclaw/tools/sessions_tool.h"
#include "hunterclaw/tools/support_tokens.h"

#include <chrono>
#include <thread>
#include <random>
#include <spdlog/spdlog.h>

namespace {
    std::shared_ptr<spdlog::logger> as_log() {
        static auto logger = spdlog::get("sessions.agent_step");
        if (!logger) logger = spdlog::stdout_logger_mt("sessions.agent_step");
        return logger;
    }
}

namespace hunterclaw {
namespace tools {

namespace {
    const std::string AGENT_LANE_NESTED = "nested";
    const std::string INTERNAL_MESSAGE_CHANNEL = "internal";
    const std::string TOOL_NAME = "sessions_send";

    // Fetch the last assistant reply from session history (aligned with
    // readLatestAssistantReplySnapshot / stopAtTranscriptArtifact)
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
            // Walk backward to find the last assistant message with text content
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
        } catch (const std::exception& e) {
            as_log()->warn("agent_step: failed to fetch history for {}: {}", session_key, e.what());
        }
        return "";
    }

    // Generate UUID v4 for idempotency key
    std::string generate_uuid_v4() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<uint64_t> dis;
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

    // Wait for agent run to complete (aligned with waitForAgentRun)
    bool wait_for_agent_run(
        std::shared_ptr<gateway::GatewayClient> gw,
        const std::string& run_id,
        int timeout_ms)
    {
        if (!gw) return false;
        try {
            auto wait_result = gw->request<nlohmann::json>("agent.wait", nlohmann::json{
                {"runId", run_id},
                {"timeoutMs", timeout_ms}
            });
            if (!wait_result.is_ok()) return false;
            const auto& w = wait_result.get();
            std::string status = w.value("status", "");
            return (status == "ok" || status == "completed");
        } catch (const std::exception& e) {
            as_log()->warn("agent_step: agent.wait failed for {}: {}", run_id, e.what());
            return false;
        }
    }

}  // anonymous namespace

core::Result<std::string> run_agent_step(
    std::shared_ptr<gateway::GatewayClient> gw,
    const std::string& session_key,
    const std::string& message,
    const std::string& extra_context,
    int timeout_ms)
{
    if (!gw) {
        return core::Result<std::string>::Err("Gateway not available");
    }
    if (session_key.empty()) {
        return core::Result<std::string>::Err("Session key is empty");
    }

    std::string idempotency_key = generate_uuid_v4();

    // Build the full message: extra_context + message (aligned with OpenClaw
    // where extraSystemPrompt is injected as system context)
    std::string full_message;
    if (!extra_context.empty()) {
        full_message = extra_context + "\n\n---\n\n" + message;
    } else {
        full_message = message;
    }

    nlohmann::json send_params = {
        {"message",         full_message},
        {"sessionKey",      session_key},
        {"idempotencyKey",  idempotency_key},
        {"deliver",         false},
        {"channel",         INTERNAL_MESSAGE_CHANNEL},
        {"lane",            AGENT_LANE_NESTED},
        {"inputProvenance", nlohmann::json{
            {"kind",             "a2a_step"},
            {"sourceSessionKey", ""},
            {"sourceChannel",    ""},
            {"sourceTool",       TOOL_NAME}
        }}
    };

    // Start agent run
    std::string run_id;
    try {
        auto response = gw->request<nlohmann::json>("agent", send_params);
        if (!response.is_ok()) {
            return core::Result<std::string>::Err(
                "agent_step: agent command failed: " + response.error);
        }
        const auto& resp = response.get();
        if (resp.contains("runId") && resp["runId"].is_string()) {
            run_id = resp["runId"].get<std::string>();
        }
    } catch (const std::exception& e) {
        return core::Result<std::string>::Err(
            std::string("agent_step: agent command exception: ") + e.what());
    }

    if (run_id.empty()) {
        return core::Result<std::string>::Err("agent_step: no runId returned");
    }

    // Wait for agent to complete
    bool completed = wait_for_agent_run(gw, run_id, timeout_ms);
    if (!completed) {
        return core::Result<std::string>::Err(
            "agent_step: timeout waiting for agent " + session_key);
    }

    // Fetch the reply
    std::string reply = fetch_last_assistant_reply(gw, session_key);
    if (reply.empty()) {
        return core::Result<std::string>::Err("agent_step: no reply from " + session_key);
    }

    return core::Result<std::string>::Ok(reply);
}

}  // namespace tools
}  // namespace hunterclaw
