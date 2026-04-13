// subagent_announce_delivery.cc
// Aligned with openclaw:
//   src/agents/subagent-announce-delivery.ts  (deliverSubagentAnnouncement / runAnnounceDeliveryWithRetry)
//   src/agents/subagent-announce-dispatch.ts  (runSubagentAnnounceDispatch)
//
// Provides:
//   - is_internal_announce_requester_session
//   - is_transient_announce_delivery_error
//   - is_permanent_announce_delivery_error
//   - resolve_subagent_announce_timeout_ms
//   - run_announce_delivery_with_retry
//   - deliver_subagent_announcement

#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <regex>
#include <chrono>
#include <thread>

using json = nlohmann::json;

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("subagent_announce_delivery");
    if (!logger) logger = spdlog::stdout_logger_mt("subagent_announce_delivery");
    return logger;
}

// 🎯 对齐 OpenClaw: DEFAULT_SUBAGENT_ANNOUNCE_TIMEOUT_MS = 120_000
static constexpr int64_t DEFAULT_SUBAGENT_ANNOUNCE_TIMEOUT_MS = 120000;
static constexpr int64_t MAX_TIMER_SAFE_TIMEOUT_MS = 2147000000LL;

// 🎯 对齐 OpenClaw: TRANSIENT_ANNOUNCE_DELIVERY_ERROR_PATTERNS
static const std::vector<std::regex> TRANSIENT_ERROR_PATTERNS = {
    std::regex("errorcode=unavailable", std::regex::icase),
    std::regex("status\\s*[:=]\\s*\"?unavailable", std::regex::icase),
    std::regex("\\bUNAVAILABLE\\b"),
    std::regex("no active .* listener", std::regex::icase),
    std::regex("gateway not connected", std::regex::icase),
    std::regex("gateway closed \\(1006", std::regex::icase),
    std::regex("gateway timeout", std::regex::icase),
    std::regex("\\b(econnreset|econnrefused|etimedout|enotfound|ehostunreach|network error)\\b",
               std::regex::icase),
};

// 🎯 对齐 OpenClaw: PERMANENT_ANNOUNCE_DELIVERY_ERROR_PATTERNS
static const std::vector<std::regex> PERMANENT_ERROR_PATTERNS = {
    std::regex("unsupported channel", std::regex::icase),
    std::regex("unknown channel", std::regex::icase),
    std::regex("chat not found", std::regex::icase),
    std::regex("user not found", std::regex::icase),
    std::regex("bot.*not.*member", std::regex::icase),
    std::regex("bot was blocked by the user", std::regex::icase),
    std::regex("forbidden: bot was kicked", std::regex::icase),
    std::regex("recipient is not a valid", std::regex::icase),
    std::regex("outbound not configured for channel", std::regex::icase),
};

// 🎯 对齐 OpenClaw: resolveDirectAnnounceTransientRetryDelaysMs
static const std::vector<int64_t> TRANSIENT_RETRY_DELAYS_MS = {5000, 10000, 20000};

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSubagentAnnounceTimeoutMs(cfg)
// ============================================================================

int64_t resolve_subagent_announce_timeout_ms(const core::Config* cfg) {
    // TODO: Read from cfg->agents.defaults.subagents.announceTimeoutMs
    return DEFAULT_SUBAGENT_ANNOUNCE_TIMEOUT_MS;
}

// ============================================================================
// 🎯 对齐 OpenClaw: isTransientAnnounceDeliveryError(error)
// ============================================================================

bool is_transient_announce_delivery_error(const std::string& error) {
    if (error.empty()) return false;

    // Check permanent first
    for (const auto& re : PERMANENT_ERROR_PATTERNS) {
        if (std::regex_search(error, re)) return false;
    }

    // Check transient
    for (const auto& re : TRANSIENT_ERROR_PATTERNS) {
        if (std::regex_search(error, re)) return true;
    }

    return false;
}

// ============================================================================
// 🎯 对齐 OpenClaw: isPermanentAnnounceDeliveryError(error)
// ============================================================================

bool is_permanent_announce_delivery_error(const std::string& error) {
    if (error.empty()) return false;
    for (const auto& re : PERMANENT_ERROR_PATTERNS) {
        if (std::regex_search(error, re)) return true;
    }
    return false;
}

// ============================================================================
// 🎯 对齐 OpenClaw: runAnnounceDeliveryWithRetry
// ============================================================================

struct AnnounceDeliveryResult {
    bool delivered = false;
    std::string error;
    int attempts = 0;
};

AnnounceDeliveryResult run_announce_delivery_with_retry(
    gateway::GatewayClient& gw,
    const json& agent_params,
    int64_t timeout_ms
) {
    AnnounceDeliveryResult result;

    auto start = std::chrono::steady_clock::now();
    int max_retries = static_cast<int>(TRANSIENT_RETRY_DELAYS_MS.size());

    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        result.attempts = attempt + 1;

        // Check timeout
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            result.error = "Announce delivery timed out after " +
                std::to_string(elapsed) + "ms";
            break;
        }

        try {
            auto response = gw.request<json>("agent", agent_params);
            if (response.is_ok()) {
                result.delivered = true;
                return result;
            }

            result.error = response.error;

            // Check if error is transient (retryable)
            if (!is_transient_announce_delivery_error(result.error)) {
                log()->warn("run_announce_delivery_with_retry: permanent error on attempt {}: {}",
                    attempt + 1, result.error);
                break;
            }

            // Retry with delay
            if (attempt < max_retries) {
                int64_t delay_ms = TRANSIENT_RETRY_DELAYS_MS[attempt];
                log()->info("run_announce_delivery_with_retry: transient error, retrying in {}ms (attempt {})",
                    delay_ms, attempt + 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }

        } catch (const std::exception& e) {
            result.error = e.what();

            if (!is_transient_announce_delivery_error(result.error)) {
                break;
            }

            if (attempt < max_retries) {
                int64_t delay_ms = TRANSIENT_RETRY_DELAYS_MS[attempt];
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
    }

    return result;
}

// ============================================================================
// 🎯 对齐 OpenClaw: deliverSubagentAnnouncement
// ============================================================================

AnnounceDeliveryResult deliver_subagent_announcement(
    gateway::GatewayClient& gw,
    const std::string& requester_session_key,
    const std::string& message,
    const std::string& idempotency_key,
    bool deliver,
    const json& internal_events,
    const json& input_provenance,
    int64_t timeout_ms
) {
    // 🎯 对齐 OpenClaw: callGateway("agent", { sessionKey, message, deliver, idempotencyKey, ... })
    json agent_params = {
        {"sessionKey",      requester_session_key},
        {"message",         message},
        {"deliver",         deliver},
        {"idempotencyKey",  idempotency_key},
    };

    if (!internal_events.is_null()) {
        agent_params["internalEvents"] = internal_events;
    }
    if (!input_provenance.is_null()) {
        agent_params["inputProvenance"] = input_provenance;
    }

    log()->info("deliver_subagent_announcement: target={} deliver={} idem={}",
        requester_session_key, deliver, idempotency_key);

    return run_announce_delivery_with_retry(gw, agent_params, timeout_ms);
}

} // namespace agents
} // namespace hunterclaw
