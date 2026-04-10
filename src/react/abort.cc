#include "hunterclaw/react/abort.h"
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace react {

namespace {
auto log() {
    static auto logger = spdlog::get("react");
    if (!logger) logger = spdlog::stdout_logger_mt("react");
    return logger;
}
}

// ============================================================================
// formatAbortReplyText (对齐 abort.ts:93-98)
// ============================================================================

std::string format_abort_reply_text(int stopped_subagents) {
    if (stopped_subagents <= 0) {
        return "\xE2\x9A\x99\xEF\xB8\x8F Agent was aborted.";  // ⚙️
    }
    std::string label = (stopped_subagents == 1) ? "sub-agent" : "sub-agents";
    return "\xE2\x9A\x99\xEF\xB8\x8F Agent was aborted. Stopped " +
           std::to_string(stopped_subagents) + " " + label + ".";
}

// ============================================================================
// stopSubagentsForRequester (对齐 abort.ts:220-260)
// ============================================================================

StopSubagentsResult stop_subagents_for_requester(
    const std::string& requester_session_key,
    const std::function<std::vector<std::string>(const std::string&)>& list_subagent_runs,
    const std::function<void(const std::string&)>& abort_embedded_run,
    const std::function<void(const std::string&)>& terminate_subagent
) {
    StopSubagentsResult result;

    if (requester_session_key.empty() || !list_subagent_runs) {
        return result;
    }

    std::vector<std::string> child_keys;
    try {
        child_keys = list_subagent_runs(requester_session_key);
    } catch (const std::exception& e) {
        log()->warn("Failed to list subagent runs for abort: {}", e.what());
        return result;
    }

    for (const auto& child_key : child_keys) {
        try {
            // 对齐: abortEmbeddedPiRun(childSessionKey)
            if (abort_embedded_run) {
                abort_embedded_run(child_key);
            }
            // 对齐: markSubagentRunTerminated(childSessionKey)
            if (terminate_subagent) {
                terminate_subagent(child_key);
            }
            result.stopped++;
        } catch (const std::exception& e) {
            log()->warn("Failed to stop subagent '{}': {}", child_key, e.what());
        }
    }

    return result;
}

// ============================================================================
// handleAbortCommand (对齐 abort.ts:100-345)
// ============================================================================

AbortResult handle_abort_command(const AbortSessionParams& params) {
    AbortResult result;

    // 对齐: 验证参数
    if (params.session_key.empty()) {
        result.reply_text = "Cannot abort: no active session.";
        return result;
    }

    std::string target_key = params.target_session_key.empty()
        ? params.session_key
        : params.target_session_key;

    // 对齐: setAbortMemory(abortKey, true)
    std::string abort_key = target_key;
    set_abort_memory(abort_key, true);

    // 对齐: abort the embedded run
    if (params.abort_embedded_run) {
        try {
            params.abort_embedded_run(target_key);
        } catch (const std::exception& e) {
            log()->warn("Failed to abort embedded run for '{}': {}", target_key, e.what());
        }
    }

    // 对齐: stopSubagentsForRequester
    auto subagent_result = stop_subagents_for_requester(
        target_key,
        params.list_subagent_runs,
        params.abort_embedded_run,
        params.terminate_subagent
    );
    result.stopped_subagents = subagent_result.stopped;

    // 对齐: clearSessionQueues
    if (params.clear_session_queues) {
        try {
            params.clear_session_queues(target_key);
        } catch (const std::exception& e) {
            log()->warn("Failed to clear session queues for '{}': {}", target_key, e.what());
        }
    }

    // 对齐: resolve abort cutoff
    if (should_persist_abort_cutoff(params.session_key, target_key)) {
        auto cutoff = resolve_abort_cutoff_from_context(
            params.session_key, params.timestamp_ms);
        if (cutoff.has_value()) {
            result.cutoff = cutoff;
        }
    }

    result.aborted = true;
    result.reply_text = format_abort_reply_text(result.stopped_subagents);

    log()->info("Abort completed for session '{}': stopped {} subagents",
                target_key, result.stopped_subagents);

    return result;
}

}  // namespace react
}  // namespace hunterclaw
