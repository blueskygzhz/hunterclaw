#ifndef HUNTERCLAW_AUTO_REPLY_TYPES_H
#define HUNTERCLAW_AUTO_REPLY_TYPES_H

#include <string>
#include <optional>
#include <vector>
#include <functional>
#include <memory>
#include "hunterclaw/auto_reply/thinking.h"

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Auto-reply types
// Aligned with OpenClaw: src/auto-reply/types.ts
// ============================================================================

// Block reply context (aligned with OpenClaw: BlockReplyContext)
struct BlockReplyContext {
    std::optional<bool> abort_signal;  // simplified: just a flag
    std::optional<int> timeout_ms;
};

// Model selected context (aligned with OpenClaw: ModelSelectedContext)
struct ModelSelectedContext {
    std::string provider;
    std::string model;
    std::optional<std::string> think_level;
};

// Typing policy (aligned with OpenClaw: TypingPolicy)
enum class TypingPolicy {
    Auto,             // "auto"
    UserMessage,      // "user_message"
    SystemEvent,      // "system_event"
    InternalWebchat,  // "internal_webchat"
    Heartbeat         // "heartbeat"
};

// Reply payload (aligned with OpenClaw: ReplyPayload)
struct ReplyPayload {
    std::optional<std::string> text;
    std::optional<std::string> media_url;
    std::vector<std::string> media_urls;
    std::optional<std::string> reply_to_id;
    bool reply_to_tag = false;
    bool reply_to_current = false;  // [[reply_to_current]] not yet mapped
    bool audio_as_voice = false;    // send audio as voice bubble
    bool is_error = false;
    bool is_reasoning = false;      // marks reasoning/thinking block
    // Channel-specific payload data
    // (simplified: use string map instead of Record<string, unknown>)
    std::vector<std::pair<std::string, std::string>> channel_data;
};

// Bootstrap context mode (aligned with OpenClaw: bootstrapContextMode)
enum class BootstrapContextMode {
    Full,        // "full"
    Lightweight  // "lightweight"
};

// Get reply options (aligned with OpenClaw: GetReplyOptions)
struct GetReplyOptions {
    std::optional<std::string> run_id;
    bool abort_signal = false;  // simplified
    std::optional<std::string> heartbeat_model_override;
    BootstrapContextMode bootstrap_context_mode = BootstrapContextMode::Full;
    bool suppress_tool_error_warnings = false;
    bool is_heartbeat = false;
    std::optional<TypingPolicy> typing_policy;
    bool suppress_typing = false;
    std::optional<int> block_reply_timeout_ms;
    std::optional<std::vector<std::string>> skill_filter;
    std::optional<int> timeout_override_seconds;

    // Callbacks (simplified as flags/values since C++ doesn't have JS-style callbacks easily)
    std::function<void(const std::string& run_id)> on_agent_run_start;
    std::function<void()> on_reply_start;
    std::function<void()> on_typing_cleanup;
    std::function<void(const ReplyPayload&)> on_partial_reply;
    std::function<void(const ReplyPayload&)> on_reasoning_stream;
    std::function<void()> on_reasoning_end;
    std::function<void()> on_assistant_message_start;
    std::function<void(const ReplyPayload&, const BlockReplyContext&)> on_block_reply;
    std::function<void(const ReplyPayload&)> on_tool_result;
    std::function<void(const ModelSelectedContext&)> on_model_selected;
    std::function<void()> on_compaction_start;
    std::function<void()> on_compaction_end;

    bool disable_block_streaming = false;
};

} // namespace auto_reply
} // namespace hunterclaw

#endif // HUNTERCLAW_AUTO_REPLY_TYPES_H
