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
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <regex>
#include <chrono>
#include <thread>
#include <functional>
#include <stdexcept>

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
    std::regex("\\berrorcode=unavailable\\b", std::regex::icase),
    std::regex("\\bstatus\\s*[:=]\\s*\"?unavailable\\b", std::regex::icase),
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

// ============================================================================
// 🎯 对齐 OpenClaw: summarizeDeliveryError(error)
// ============================================================================

std::string summarize_delivery_error(const std::string& error) {
    if (error.empty()) return "unknown error";
    return error;
}

// ============================================================================
// 🎯 对齐 OpenClaw: isCronSessionKey (sessions/session-key-utils.ts)
// agent:<id>:cron:... pattern
// ============================================================================

bool is_cron_session_key(const std::string& session_key) {
    if (session_key.empty()) return false;
    auto parsed = AgentScopeManager::parse_agent_session_key(session_key);
    if (!parsed.has_value()) return false;
    std::string rest = parsed->session_name;
    // lowercase
    std::transform(rest.begin(), rest.end(), rest.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return rest.find("cron:") == 0;
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSubagentAnnounceTimeoutMs(cfg)
// ============================================================================

int64_t resolve_subagent_announce_timeout_ms(const core::Config* cfg) {
    // TODO: Read from cfg->agents.defaults.subagents.announceTimeoutMs
    // Apply same clamp as OC: Math.min(Math.max(1, Math.floor(configured)), MAX_TIMER_SAFE_TIMEOUT_MS)
    return DEFAULT_SUBAGENT_ANNOUNCE_TIMEOUT_MS;
}

// ============================================================================
// 🎯 对齐 OpenClaw: isInternalAnnounceRequesterSession(sessionKey)
// OC: getSubagentDepthFromSessionStore(sessionKey) >= 1 || isCronSessionKey(sessionKey)
// ============================================================================

bool is_internal_announce_requester_session(const std::string& session_key) {
    if (session_key.empty()) return false;
    int depth = get_subagent_depth_from_session_store(session_key, nullptr);
    if (depth >= 1) return true;
    return is_cron_session_key(session_key);
}

// ============================================================================
// 🎯 对齐 OpenClaw: isTransientAnnounceDeliveryError(error)
// ============================================================================

bool is_transient_announce_delivery_error(const std::string& error) {
    if (error.empty()) return false;

    // Check permanent first — permanent takes priority
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
// 🎯 对齐 OpenClaw: runAnnounceDeliveryWithRetry<T>({ operation, signal, run })
//
// OC signature:
//   async function runAnnounceDeliveryWithRetry<T>(params: {
//     operation: string;
//     signal?: AbortSignal;
//     run: () => Promise<T>;
//   }): Promise<T>
//
// HC equivalent: takes operation name + abort flag + callable, throws on permanent/exhausted error
// ============================================================================

void run_announce_delivery_with_retry(
    const std::string& operation,
    const std::atomic<bool>* abort_signal,
    std::function<void()> run
) {
    int retry_index = 0;
    const int max_retries = static_cast<int>(TRANSIENT_RETRY_DELAYS_MS.size());

    for (;;) {
        // 🎯 对齐 OC: if (params.signal?.aborted) throw
        if (abort_signal && abort_signal->load()) {
            throw std::runtime_error("announce delivery aborted");
        }

        try {
            run();
            return; // success
        } catch (const std::exception& e) {
            std::string err_msg = e.what();
            int64_t delay_ms = (retry_index < max_retries)
                ? TRANSIENT_RETRY_DELAYS_MS[retry_index] : -1;

            // 🎯 对齐 OC: if (delayMs == null || !isTransientAnnounceDeliveryError(err) || signal?.aborted) throw
            if (delay_ms < 0
                || !is_transient_announce_delivery_error(err_msg)
                || (abort_signal && abort_signal->load())) {
                throw; // re-throw — permanent error or exhausted retries
            }

            int next_attempt = retry_index + 2;
            int max_attempts = max_retries + 1;
            log()->warn("[warn] Subagent announce {} transient failure, retrying {}/{} in {}s: {}",
                operation, next_attempt, max_attempts,
                static_cast<int>(delay_ms / 1000),
                summarize_delivery_error(err_msg));

            retry_index += 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
}

// ============================================================================
// 🎯 对齐 OpenClaw: SubagentAnnounceDeliveryResult / SubagentDeliveryPath
// ============================================================================

struct SubagentAnnounceDeliveryResult {
    bool delivered = false;
    std::string path;   // "queued" | "steered" | "direct" | "none"
    std::string error;
};

// ============================================================================
// 🎯 对齐 OpenClaw: runSubagentAnnounceDispatch (subagent-announce-dispatch.ts)
//
// Dispatch logic:
//   if (!expectsCompletionMessage):
//     1. queue-primary → if delivered, return
//     2. if dropped, return
//     3. direct-primary → return
//   else:
//     1. direct-primary → if delivered, return
//     2. queue-fallback → if delivered, return
//     3. return direct result
// ============================================================================

SubagentAnnounceDeliveryResult run_subagent_announce_dispatch(
    bool expects_completion_message,
    const std::atomic<bool>* abort_signal,
    std::function<std::string()> queue_fn,   // returns: "steered"|"queued"|"none"|"dropped"
    std::function<SubagentAnnounceDeliveryResult()> direct_fn
) {
    // 🎯 对齐 OC: if (params.signal?.aborted) return { delivered: false, path: "none" }
    if (abort_signal && abort_signal->load()) {
        return {false, "none", ""};
    }

    auto map_queue_outcome = [](const std::string& outcome) -> SubagentAnnounceDeliveryResult {
        if (outcome == "steered") return {true, "steered", ""};
        if (outcome == "queued")  return {true, "queued", ""};
        return {false, "none", ""};
    };

    if (!expects_completion_message) {
        // queue-primary
        std::string queue_outcome = queue_fn();
        auto primary_queue = map_queue_outcome(queue_outcome);
        if (primary_queue.delivered) return primary_queue;
        if (queue_outcome == "dropped") return primary_queue;

        // direct-primary
        return direct_fn();
    }

    // expects_completion_message path
    // direct-primary
    auto primary_direct = direct_fn();
    if (primary_direct.delivered) return primary_direct;

    if (abort_signal && abort_signal->load()) return primary_direct;

    // queue-fallback
    std::string fallback_outcome = queue_fn();
    auto fallback_queue = map_queue_outcome(fallback_outcome);
    if (fallback_queue.delivered) return fallback_queue;

    return primary_direct;
}

// ============================================================================
// 🎯 对齐 OpenClaw: deliverSubagentAnnouncement
//
// OC params:
//   requesterSessionKey, announceId, triggerMessage, steerMessage,
//   internalEvents, summaryLine, requesterSessionOrigin, requesterOrigin,
//   completionDirectOrigin, directOrigin, sourceSessionKey, sourceChannel,
//   sourceTool, targetRequesterSessionKey, requesterIsSubagent,
//   expectsCompletionMessage, bestEffortDeliver, directIdempotencyKey, signal
//
// HC simplified: uses GatewayClient for direct delivery, no queue/steer support yet
// ============================================================================

SubagentAnnounceDeliveryResult deliver_subagent_announcement(
    gateway::GatewayClient& gw,
    const std::string& requester_session_key,
    const std::string& target_requester_session_key,
    const std::string& trigger_message,
    const std::string& idempotency_key,
    bool requester_is_subagent,
    bool expects_completion_message,
    const json& internal_events,
    const json& input_provenance,
    int64_t timeout_ms,
    const std::atomic<bool>* abort_signal
) {
    // 🎯 对齐 OC: queue path — HC does not have embedded Pi queue, always returns "none"
    auto queue_fn = [&]() -> std::string {
        // No queue support in HC yet → always "none"
        return "none";
    };

    // 🎯 对齐 OC: direct path — sendSubagentAnnounceDirectly
    auto direct_fn = [&]() -> SubagentAnnounceDeliveryResult {
        if (abort_signal && abort_signal->load()) {
            return {false, "none", ""};
        }

        // 🎯 对齐 OC: callGateway("agent", { sessionKey, message, deliver, idempotencyKey, ... })
        json agent_params = {
            {"sessionKey",      target_requester_session_key},
            {"message",         trigger_message},
            {"deliver",         !requester_is_subagent},
            {"idempotencyKey",  idempotency_key},
        };

        if (!internal_events.is_null()) {
            agent_params["internalEvents"] = internal_events;
        }
        if (!input_provenance.is_null()) {
            agent_params["inputProvenance"] = input_provenance;
        }

        log()->info("deliver_subagent_announcement: target={} deliver={} idem={} expectsCompletion={}",
            target_requester_session_key, !requester_is_subagent,
            idempotency_key, expects_completion_message);

        try {
            run_announce_delivery_with_retry(
                expects_completion_message
                    ? "completion direct announce agent call"
                    : "direct announce agent call",
                abort_signal,
                [&]() {
                    auto response = gw.request<json>("agent", agent_params);
                    if (!response.is_ok()) {
                        throw std::runtime_error(response.error);
                    }
                }
            );
            return {true, "direct", ""};
        } catch (const std::exception& e) {
            return {false, "direct", summarize_delivery_error(e.what())};
        }
    };

    return run_subagent_announce_dispatch(
        expects_completion_message,
        abort_signal,
        queue_fn,
        direct_fn
    );
}

} // namespace agents
} // namespace hunterclaw
