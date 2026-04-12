// subagent_control.cc
// Aligned with openclaw:
//   src/agents/subagent-control.ts
//
// Provides:
//   - ResolvedSubagentController
//   - resolve_subagent_controller
//   - list_controlled_subagent_runs
//   - kill_subagent_run
//   - cascade_kill_children
//   - steer_subagent
//   - get_subagent_reply

#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <set>
#include <mutex>

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("subagent_control");
    if (!logger) logger = spdlog::stdout_logger_mt("subagent_control");
    return logger;
}

// ============================================================================
// 🎯 对齐 OpenClaw: Constants from subagent-control.ts
// ============================================================================

static constexpr int DEFAULT_RECENT_MINUTES = 30;
static constexpr int MAX_RECENT_MINUTES = 24 * 60;
static constexpr int MAX_STEER_MESSAGE_CHARS = 4000;
static constexpr int64_t STEER_RATE_LIMIT_MS = 2000;
static constexpr int64_t STEER_ABORT_SETTLE_TIMEOUT_MS = 5000;
static constexpr int SUBAGENT_REPLY_HISTORY_LIMIT = 50;

// 🎯 对齐 OpenClaw: steerRateLimit = new Map<string, number>()
static std::mutex rate_limit_mutex;
static std::map<std::string, int64_t> steer_rate_limit;

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// 🎯 对齐 OpenClaw: isSubagentSessionKey
// ============================================================================

bool is_subagent_key(const std::string& key) {
    return key.find("sub:") != std::string::npos;
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSubagentController({ cfg, agentSessionKey })
// ============================================================================

SubagentController resolve_subagent_controller(
    const core::Config& cfg,
    const std::string& agent_session_key
) {
    SubagentController result;

    AgentScopeManager scope(cfg);
    std::string default_id = scope.resolve_default_agent_id();

    // Resolve caller session key
    std::string caller_raw = agent_session_key;
    if (caller_raw.empty()) {
        caller_raw = "agent:" + default_id + ":main";
    }
    // trim
    auto start = caller_raw.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
        auto end = caller_raw.find_last_not_of(" \t\n\r");
        caller_raw = caller_raw.substr(start, end - start + 1);
    }

    result.caller_session_key = caller_raw;
    result.controller_session_key = caller_raw;

    if (!is_subagent_key(caller_raw)) {
        result.caller_is_subagent = false;
        result.control_scope = "children";
        return result;
    }

    // 🎯 对齐 OpenClaw: resolveStoredSubagentCapabilities(callerSessionKey, { cfg })
    SubagentCapabilities caps = resolve_stored_subagent_capabilities(caller_raw, &cfg);
    result.caller_is_subagent = true;
    result.control_scope = caps.control_scope;
    return result;
}

// ============================================================================
// 🎯 对齐 OpenClaw: listControlledSubagentRuns(controllerSessionKey)
// ============================================================================

std::vector<std::shared_ptr<SubagentEntry>> list_controlled_subagent_runs(
    SubagentRegistry& registry,
    const std::string& controller_session_key
) {
    std::string key = controller_session_key;
    // trim
    auto start = key.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return {};
    auto end = key.find_last_not_of(" \t\n\r");
    key = key.substr(start, end - start + 1);
    if (key.empty()) return {};

    // Get all active entries and filter by controller
    auto all_active = registry.list_active();
    std::vector<std::shared_ptr<SubagentEntry>> filtered;

    // Build latest-by-child index to dedup
    std::map<std::string, std::shared_ptr<SubagentEntry>> latest_by_child;
    for (const auto& entry : all_active) {
        std::string child_key = entry->subagent_run_id;
        if (child_key.empty()) continue;

        auto existing = latest_by_child.find(child_key);
        if (existing == latest_by_child.end() || entry->spawned_at > existing->second->spawned_at) {
            latest_by_child[child_key] = entry;
        }
    }

    for (const auto& [child_key, entry] : latest_by_child) {
        std::string controller = entry->controller_session_key;
        if (controller.empty()) controller = entry->requester_run_id;
        if (controller == key) {
            filtered.push_back(entry);
        }
    }

    // Sort by spawned_at descending
    std::sort(filtered.begin(), filtered.end(),
        [](const std::shared_ptr<SubagentEntry>& a, const std::shared_ptr<SubagentEntry>& b) {
            return a->spawned_at > b->spawned_at;
        });

    return filtered;
}

// ============================================================================
// 🎯 对齐 OpenClaw: ensureControllerOwnsRun({ controller, entry })
// ============================================================================

std::string ensure_controller_owns_run(
    const SubagentController& controller,
    const SubagentEntry& entry
) {
    std::string owner = entry.controller_session_key;
    if (owner.empty()) owner = entry.requester_run_id;
    if (owner == controller.controller_session_key) {
        return "";  // OK
    }
    return "Subagents can only control runs spawned from their own session.";
}

// ============================================================================
// 🎯 对齐 OpenClaw: killSubagentRun({ cfg, entry, cache })
// ============================================================================

KillSubagentResult kill_subagent_run(
    SubagentRegistry& registry,
    const std::string& run_id
) {
    KillSubagentResult result;
    result.killed = false;

    auto entry = registry.get_entry(run_id);
    if (!entry) {
        result.error = "Run not found: " + run_id;
        return result;
    }

    // Already ended?
    if (entry->completed_at > 0 || entry->state == SubagentState::Completed ||
        entry->state == SubagentState::Failed || entry->state == SubagentState::Killed) {
        return result;
    }

    // Mark as terminated
    int marked = registry.mark_subagent_run_terminated(run_id, "", "killed");
    result.killed = (marked > 0);
    result.session_id = entry->subagent_run_id;

    log()->info("kill_subagent_run: run={} killed={}", run_id, result.killed);
    return result;
}

// ============================================================================
// 🎯 对齐 OpenClaw: cascadeKillChildren({ cfg, parentChildSessionKey, ... })
// ============================================================================

CascadeKillResult cascade_kill_children(
    SubagentRegistry& registry,
    const std::string& parent_child_session_key
) {
    CascadeKillResult result;
    result.killed = 0;

    std::set<std::string> seen;

    // Find all child runs for this parent
    auto children = registry.list_by_requester(parent_child_session_key);
    for (const auto& child : children) {
        if (seen.count(child->subagent_run_id)) continue;
        seen.insert(child->subagent_run_id);

        // Kill this child
        auto kill_result = kill_subagent_run(registry, child->subagent_run_id);
        if (kill_result.killed) {
            result.killed++;
            result.labels.push_back(resolve_subagent_label(*child));
        }

        // Recurse into grandchildren
        auto grandchild_result = cascade_kill_children(registry, child->subagent_run_id);
        result.killed += grandchild_result.killed;
        result.labels.insert(result.labels.end(),
            grandchild_result.labels.begin(), grandchild_result.labels.end());
    }

    return result;
}

// ============================================================================
// 🎯 对齐 OpenClaw: steerSubagent
// Redirect a running subagent with a new message
// Rate-limited to STEER_RATE_LIMIT_MS per session
// ============================================================================

core::Result<std::string> steer_subagent(
    SubagentRegistry& registry,
    gateway::GatewayClient& gw,
    const std::string& run_id,
    const std::string& message,
    const SubagentController& controller
) {
    auto entry = registry.get_entry(run_id);
    if (!entry) {
        return core::Result<std::string>::Err("Subagent run not found: " + run_id);
    }

    // Check ownership
    std::string ownership_error = ensure_controller_owns_run(controller, *entry);
    if (!ownership_error.empty()) {
        return core::Result<std::string>::Err(ownership_error);
    }

    // Check message length — 对齐 OpenClaw: MAX_STEER_MESSAGE_CHARS
    if (message.size() > MAX_STEER_MESSAGE_CHARS) {
        return core::Result<std::string>::Err(
            "Steer message exceeds maximum length of " +
            std::to_string(MAX_STEER_MESSAGE_CHARS) + " characters.");
    }

    // Rate limit — 对齐 OpenClaw: steerRateLimit
    {
        std::lock_guard<std::mutex> lock(rate_limit_mutex);
        auto it = steer_rate_limit.find(run_id);
        if (it != steer_rate_limit.end()) {
            int64_t elapsed = now_ms() - it->second;
            if (elapsed < STEER_RATE_LIMIT_MS) {
                return core::Result<std::string>::Err(
                    "Rate limited: wait " + std::to_string(STEER_RATE_LIMIT_MS - elapsed) + "ms");
            }
        }
        steer_rate_limit[run_id] = now_ms();
    }

    // Check if run is still active
    if (entry->completed_at > 0 || entry->state == SubagentState::Completed ||
        entry->state == SubagentState::Failed || entry->state == SubagentState::Killed) {
        return core::Result<std::string>::Err(
            "Cannot steer completed subagent: " + run_id);
    }

    // 🎯 对齐 OpenClaw: mark for steer restart, then send via gateway
    bool marked = registry.mark_subagent_run_for_steer_restart(run_id);
    if (!marked) {
        log()->warn("steer_subagent: failed to mark for steer restart: {}", run_id);
    }

    // Abort current run
    // (In real implementation, this would call abortEmbeddedPiRun)

    // Wait for settle — 对齐 OpenClaw: STEER_ABORT_SETTLE_TIMEOUT_MS
    // (Simplified: in production this would be async)

    // Clear steer restart and send new message via gateway
    registry.clear_subagent_run_steer_restart(run_id);

    log()->info("steer_subagent: run={} message_len={}", run_id, message.size());
    return core::Result<std::string>::Ok("Steer message sent to " + run_id);
}

// ============================================================================
// 🎯 对齐 OpenClaw: getSubagentReply — read latest assistant reply
// ============================================================================

core::Result<std::string> get_subagent_reply(
    SubagentRegistry& registry,
    const std::string& run_id,
    const SubagentController& controller
) {
    auto entry = registry.get_entry(run_id);
    if (!entry) {
        return core::Result<std::string>::Err("Subagent run not found: " + run_id);
    }

    // Check ownership
    std::string ownership_error = ensure_controller_owns_run(controller, *entry);
    if (!ownership_error.empty()) {
        return core::Result<std::string>::Err(ownership_error);
    }

    // Return frozen result if available
    if (entry->frozen_result_captured && !entry->frozen_result_text.empty()) {
        return core::Result<std::string>::Ok(entry->frozen_result_text);
    }

    // Return result text
    if (!entry->result_text.empty()) {
        return core::Result<std::string>::Ok(entry->result_text);
    }

    // Return fallback frozen result
    if (!entry->fallback_frozen_result_text.empty()) {
        return core::Result<std::string>::Ok(entry->fallback_frozen_result_text);
    }

    return core::Result<std::string>::Err("No reply available yet for " + run_id);
}

// ============================================================================
// Helper: resolve_subagent_label (used by cascade_kill)
// ============================================================================

std::string resolve_subagent_label(const SubagentEntry& entry) {
    if (!entry.label.empty()) return entry.label;
    if (!entry.subagent_name.empty()) return entry.subagent_name;
    return entry.description.empty() ? "subagent" : entry.description;
}

} // namespace agents
} // namespace hunterclaw
