#include "hunterclaw/react/session_reset.h"
#include <spdlog/spdlog.h>
#include <filesystem>

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
// resetReplyRunSession (对齐 agent-runner-session-reset.ts:37-128)
// ============================================================================

SessionResetResult reset_reply_run_session(const SessionResetParams& params) {
    SessionResetResult result;

    if (params.session_key.empty() || !params.store_path.has_value()) {
        return result;
    }

    if (!params.generate_uuid) {
        log()->error("session reset: generate_uuid callback not provided");
        return result;
    }

    // 对齐: const nextSessionId = deps.generateSecureUuid()
    std::string next_session_id = params.generate_uuid();

    // 对齐: const nextEntry = { ...prevEntry, sessionId: nextSessionId, ... }
    // C++ 中不直接操作 SessionEntry 对象，通过回调更新
    if (params.update_session_store) {
        try {
            params.update_session_store(params.session_key, next_session_id);
        } catch (const std::exception& e) {
            log()->error("Failed to persist session reset after {}: {}",
                        params.options.failure_label, e.what());
        }
    }

    // 对齐: cleanup old transcripts if requested
    if (params.options.cleanup_transcripts &&
        params.prev_session_id.has_value() &&
        params.cleanup_transcript) {
        try {
            params.cleanup_transcript(*params.prev_session_id);
        } catch (const std::exception& e) {
            // Best-effort cleanup
            log()->debug("Transcript cleanup failed for '{}': {}",
                        *params.prev_session_id, e.what());
        }
    }

    result.reset = true;
    result.new_session_id = next_session_id;
    // 对齐: nextSessionFile = resolveSessionTranscriptPath(nextSessionId, agentId, messageThreadId)
    result.new_session_file = next_session_id + ".jsonl";

    log()->info("Session reset after {}: {} → {}",
                params.options.failure_label,
                params.prev_session_id.value_or("<none>"),
                next_session_id);

    return result;
}

}  // namespace react
}  // namespace hunterclaw
