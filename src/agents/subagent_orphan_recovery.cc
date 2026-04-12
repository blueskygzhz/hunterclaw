// subagent_orphan_recovery.cc
// Aligned with openclaw:
//   src/agents/subagent-orphan-recovery.ts
//
// Post-restart orphan recovery for subagent sessions.
// After a gateway reload aborts in-flight subagent LLM calls,
// this module scans for orphaned sessions (those with abortedLastRun: true
// that are still tracked as active in the subagent registry) and sends a
// synthetic resume message to restart their work.
//
// Provides:
//   - build_resume_message
//   - resume_orphaned_session
//   - recover_orphaned_subagent_sessions

#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <thread>
#include <chrono>

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("subagent_orphan_recovery");
    if (!logger) logger = spdlog::stdout_logger_mt("subagent_orphan_recovery");
    return logger;
}

// ============================================================================
// 🎯 对齐 OpenClaw: Constants
// ============================================================================

// Delay before attempting recovery to let the gateway finish bootstrapping
static constexpr int64_t DEFAULT_RECOVERY_DELAY_MS = 5000;

// Maximum task length in resume message
static constexpr size_t MAX_TASK_LEN = 2000;

// ============================================================================
// 🎯 对齐 OpenClaw: isRestartAbortedTimeoutRun(runRecord, entry)
// ============================================================================

bool is_restart_aborted_timeout_run(
    const SubagentEntry& run_record,
    bool session_aborted_last_run
) {
    return session_aborted_last_run &&
           run_record.state == SubagentState::TimedOut &&
           run_record.completed_at > 0;
}

// ============================================================================
// 🎯 对齐 OpenClaw: buildResumeMessage(task, lastHumanMessage)
// ============================================================================

std::string build_resume_message(
    const std::string& task,
    const std::string& last_human_message
) {
    std::string truncated_task = task;
    if (truncated_task.size() > MAX_TASK_LEN) {
        truncated_task = truncated_task.substr(0, MAX_TASK_LEN) + "...";
    }

    std::string message =
        "[System] Your previous turn was interrupted by a gateway reload. "
        "Your original task was:\n\n" + truncated_task + "\n\n";

    if (!last_human_message.empty()) {
        message += "The last message from the user before the interruption was:\n\n" +
                   last_human_message + "\n\n";
    }

    message += "Please continue where you left off.";
    return message;
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: resumeOrphanedSession(params)
// Send a resume message to an orphaned subagent session via the gateway agent method.
// ============================================================================

bool resume_orphaned_session(
    gateway::GatewayClient& gw,
    SubagentRegistry& registry,
    const std::string& session_key,
    const std::string& task,
    const std::string& last_human_message,
    const std::string& original_run_id
) {
    try {
        std::string resume_message = build_resume_message(task, last_human_message);

        log()->info("resume_orphaned_session: session={} original_run={} task_len={}",
            session_key, original_run_id, task.size());

        // 🎯 对齐 OpenClaw: replaceSubagentRunAfterSteer
        // Mark the old run and create a replacement entry
        auto entry = registry.get_entry(original_run_id);
        if (!entry) {
            log()->warn("resume_orphaned_session: run not found: {}", original_run_id);
            return false;
        }

        // Mark for steer restart
        registry.mark_subagent_run_for_steer_restart(original_run_id);

        // Send resume via gateway — 对齐 OpenClaw: callGateway("agent", { ... })
        nlohmann::json params;
        params["sessionKey"] = session_key;
        params["message"] = resume_message;
        params["lane"] = "subagent";
        params["internal"] = true;

        auto result = gw.request("agent", params);
        if (!result.is_ok()) {
            log()->error("resume_orphaned_session: gateway call failed for {}: {}",
                session_key, result.error);
            return false;
        }

        // Clear steer restart
        registry.clear_subagent_run_steer_restart(original_run_id);

        log()->info("resume_orphaned_session: successfully resumed {}", session_key);
        return true;

    } catch (const std::exception& e) {
        log()->error("resume_orphaned_session: exception for {}: {}",
            session_key, e.what());
        return false;
    }
}

// ============================================================================
// 🎯 对齐 OpenClaw: recoverOrphanedSubagentSessions()
// Main entry point — scans registry for orphaned sessions and resumes them.
// Called after gateway restart/reload.
// ============================================================================

void recover_orphaned_subagent_sessions(
    gateway::GatewayClient& gw,
    SubagentRegistry& registry,
    int64_t recovery_delay_ms
) {
    int64_t delay = (recovery_delay_ms > 0) ? recovery_delay_ms : DEFAULT_RECOVERY_DELAY_MS;

    log()->info("recover_orphaned_subagent_sessions: starting in {}ms", delay);

    // 🎯 对齐 OpenClaw: await sleep(DEFAULT_RECOVERY_DELAY_MS)
    // Wait for gateway to finish bootstrapping
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));

    // Get all active runs
    auto active_runs = registry.list_active();
    if (active_runs.empty()) {
        log()->info("recover_orphaned_subagent_sessions: no active runs found");
        return;
    }

    int recovered = 0;
    int skipped = 0;
    int failed = 0;

    for (const auto& run : active_runs) {
        // Skip runs that are not timed out
        if (run->state != SubagentState::TimedOut) {
            skipped++;
            continue;
        }

        // Skip runs without a task
        if (run->task.empty() && run->description.empty()) {
            skipped++;
            continue;
        }

        std::string task = run->task.empty() ? run->description : run->task;

        bool success = resume_orphaned_session(
            gw, registry,
            run->subagent_run_id,
            task,
            "",  // last_human_message (would need session history access)
            run->subagent_run_id
        );

        if (success) {
            recovered++;
        } else {
            failed++;
        }
    }

    log()->info("recover_orphaned_subagent_sessions: complete — recovered={} skipped={} failed={}",
        recovered, skipped, failed);
}

} // namespace agents
} // namespace hunterclaw
