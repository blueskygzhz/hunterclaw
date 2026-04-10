#pragma once

#include <functional>
#include <optional>
#include <string>
#include <cstdint>

// 严格对齐 OpenClaw src/auto-reply/reply/agent-runner-session-reset.ts (128行)
// Session 重置：overflow/corruption 后自动轮转 session ID

namespace hunterclaw {
namespace react {

// ============================================================================
// ResetSessionOptions (对齐 agent-runner-session-reset.ts:14-18)
// ============================================================================
struct ResetSessionOptions {
    std::string failure_label;
    bool cleanup_transcripts = false;
};

// ============================================================================
// SessionResetResult (对齐 agent-runner-session-reset.ts:37-48 返回值)
// ============================================================================
struct SessionResetResult {
    bool reset = false;
    std::string new_session_id;
    std::string new_session_file;
};

// ============================================================================
// SessionResetParams (对齐 agent-runner-session-reset.ts:37 参数)
// ============================================================================
struct SessionResetParams {
    ResetSessionOptions options;
    std::string session_key;
    std::string queue_key;
    std::string message_thread_id;

    // Session state
    std::optional<std::string> prev_session_id;
    std::optional<std::string> store_path;

    // Callbacks
    std::function<std::string()> generate_uuid;  // 对齐 generateSecureUuid
    std::function<void(const std::string& session_key, const std::string& new_session_id)>
        update_session_store;
    std::function<void(const std::string& old_session_id)> cleanup_transcript;
};

// ============================================================================
// resetReplyRunSession (对齐 agent-runner-session-reset.ts:37-128)
// ============================================================================
SessionResetResult reset_reply_run_session(const SessionResetParams& params);

}  // namespace react
}  // namespace hunterclaw
