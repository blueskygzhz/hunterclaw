#include "hunterclaw/react/abort_cutoff.h"
#include <chrono>

namespace hunterclaw {
namespace react {

// ============================================================================
// resolveAbortCutoffFromContext (对齐 abort-cutoff.ts:22-42)
// ============================================================================

std::optional<AbortCutoff> resolve_abort_cutoff_from_context(
    const std::string& command_session_key,
    int64_t now_ms
) {
    if (command_session_key.empty()) {
        return std::nullopt;
    }

    if (now_ms <= 0) {
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    AbortCutoff cutoff;
    cutoff.timestamp_ms = now_ms;
    cutoff.session_key = command_session_key;
    return cutoff;
}

// ============================================================================
// shouldPersistAbortCutoff (对齐 abort-cutoff.ts:44-58)
// ============================================================================

bool should_persist_abort_cutoff(
    const std::string& command_session_key,
    const std::string& target_session_key
) {
    // 对齐: only persist when the command sender is the same as the target
    if (command_session_key.empty() || target_session_key.empty()) {
        return false;
    }
    return command_session_key == target_session_key;
}

// ============================================================================
// applyAbortCutoffToSessionEntry (对齐 abort-cutoff.ts:60-80)
// ============================================================================

SessionEntryAbortFields apply_abort_cutoff_to_session_entry(
    const AbortCutoff& cutoff
) {
    SessionEntryAbortFields fields;
    fields.abort_cutoff_ms = cutoff.timestamp_ms;
    fields.aborted_last_run = true;
    return fields;
}

// ============================================================================
// shouldSkipMessageByAbortCutoff (对齐 abort-cutoff.ts:82-100)
// ============================================================================

bool should_skip_message_by_abort_cutoff(
    int64_t message_timestamp_ms,
    std::optional<int64_t> abort_cutoff_ms
) {
    if (!abort_cutoff_ms.has_value()) {
        return false;
    }
    // 对齐: message was enqueued before the abort → skip it
    return message_timestamp_ms > 0 &&
           *abort_cutoff_ms > 0 &&
           message_timestamp_ms <= *abort_cutoff_ms;
}

}  // namespace react
}  // namespace hunterclaw
