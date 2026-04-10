#include "hunterclaw/react/typing_mode.h"
#include <algorithm>
#include <memory>

namespace hunterclaw {
namespace react {

// ============================================================================
// resolveTypingMode (对齐 typing-mode.ts:18-42)
// ============================================================================

// 对齐: DEFAULT_GROUP_TYPING_MODE = "message"
static const TypingMode DEFAULT_GROUP_TYPING_MODE = TypingMode::Message;

TypingMode resolve_typing_mode(const TypingModeContext& ctx) {
    // 对齐: if (isHeartbeat || typingPolicy === "heartbeat" | "system_event" | "internal_webchat" || suppressTyping) → never
    if (ctx.is_heartbeat ||
        ctx.typing_policy == "heartbeat" ||
        ctx.typing_policy == "system_event" ||
        ctx.typing_policy == "internal_webchat" ||
        ctx.suppress_typing) {
        return TypingMode::Never;
    }

    // 对齐: if (configured) return configured
    if (ctx.configured.has_value()) {
        return *ctx.configured;
    }

    // 对齐: if (!isGroupChat || wasMentioned) return "instant"
    if (!ctx.is_group_chat || ctx.was_mentioned) {
        return TypingMode::Instant;
    }

    return DEFAULT_GROUP_TYPING_MODE;
}

// ============================================================================
// isSilentReplyText helper (对齐 tokens.ts:isSilentReplyText)
// ============================================================================
static bool is_silent_reply_text(const std::string& text) {
    // 对齐: text === SILENT_REPLY_TOKEN or starts with "[SILENT]"
    std::string trimmed = text;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return false;
    auto e = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(s, e - s + 1);
    return trimmed == "[SILENT]" || trimmed.find("[SILENT]") == 0;
}

// ============================================================================
// createTypingSignaler (对齐 typing-mode.ts:57-156)
// ============================================================================

TypingSignaler create_typing_signaler(
    TypingController& typing,
    TypingMode mode,
    bool is_heartbeat
) {
    bool should_start_immediately = (mode == TypingMode::Instant);
    bool should_start_on_message_start = (mode == TypingMode::Message);
    bool should_start_on_text = (mode == TypingMode::Message || mode == TypingMode::Instant);
    bool should_start_on_reasoning = (mode == TypingMode::Thinking);
    bool disabled = is_heartbeat || (mode == TypingMode::Never);

    // Shared state: track if we've seen renderable text
    auto has_renderable = std::make_shared<bool>(false);

    // 对齐: isRenderableText
    auto is_renderable_text = [](const std::string& text) -> bool {
        std::string trimmed = text;
        auto s = trimmed.find_first_not_of(" \t\n\r");
        if (s == std::string::npos) return false;
        auto e = trimmed.find_last_not_of(" \t\n\r");
        trimmed = trimmed.substr(s, e - s + 1);
        if (trimmed.empty()) return false;
        return !is_silent_reply_text(trimmed);
    };

    TypingSignaler signaler;
    signaler.mode = mode;
    signaler.should_start_immediately = should_start_immediately;
    signaler.should_start_on_message_start = should_start_on_message_start;
    signaler.should_start_on_text = should_start_on_text;
    signaler.should_start_on_reasoning = should_start_on_reasoning;

    // 对齐: signalRunStart
    signaler.signal_run_start = [disabled, should_start_immediately, &typing]() {
        if (disabled || !should_start_immediately) return;
        if (typing.start_typing_loop) typing.start_typing_loop();
    };

    // 对齐: signalMessageStart
    signaler.signal_message_start = [disabled, should_start_on_message_start, has_renderable, &typing]() {
        if (disabled || !should_start_on_message_start) return;
        if (!*has_renderable) return;
        if (typing.start_typing_loop) typing.start_typing_loop();
    };

    // 对齐: signalTextDelta
    signaler.signal_text_delta = [disabled, should_start_on_text, should_start_on_reasoning, has_renderable, is_renderable_text, &typing](const std::string& text) {
        if (disabled) return;
        if (is_renderable_text(text)) {
            *has_renderable = true;
        } else {
            return;
        }
        if (should_start_on_text) {
            if (typing.start_typing_on_text) typing.start_typing_on_text(text);
            return;
        }
        if (should_start_on_reasoning) {
            if (typing.is_active && !typing.is_active()) {
                if (typing.start_typing_loop) typing.start_typing_loop();
            }
            if (typing.refresh_typing_ttl) typing.refresh_typing_ttl();
        }
    };

    // 对齐: signalReasoningDelta
    signaler.signal_reasoning_delta = [disabled, should_start_on_reasoning, has_renderable, &typing]() {
        if (disabled || !should_start_on_reasoning) return;
        if (!*has_renderable) return;
        if (typing.start_typing_loop) typing.start_typing_loop();
        if (typing.refresh_typing_ttl) typing.refresh_typing_ttl();
    };

    // 对齐: signalToolStart
    signaler.signal_tool_start = [disabled, &typing]() {
        if (disabled) return;
        if (typing.is_active && !typing.is_active()) {
            if (typing.start_typing_loop) typing.start_typing_loop();
            if (typing.refresh_typing_ttl) typing.refresh_typing_ttl();
            return;
        }
        if (typing.refresh_typing_ttl) typing.refresh_typing_ttl();
    };

    return signaler;
}

}  // namespace react
}  // namespace hunterclaw
