#pragma once

#include <functional>
#include <string>
#include <optional>

// 严格对齐 OpenClaw src/auto-reply/reply/typing-mode.ts (157行)
// Typing mode 策略解析 + TypingSignaler 信号控制器

namespace hunterclaw {
namespace react {

// ============================================================================
// TypingMode (对齐 config/types.ts TypingMode)
// ============================================================================
enum class TypingMode {
    Never,     // "never" — no typing indicator
    Instant,   // "instant" — start typing immediately on run start
    Message,   // "message" — start typing on first message_start event
    Thinking   // "thinking" — start typing only during reasoning/thinking
};

// ============================================================================
// TypingModeContext (对齐 typing-mode.ts:7-14)
// ============================================================================
struct TypingModeContext {
    std::optional<TypingMode> configured;
    bool is_group_chat = false;
    bool was_mentioned = false;
    bool is_heartbeat = false;
    std::string typing_policy;  // "heartbeat" | "system_event" | "internal_webchat" | ""
    bool suppress_typing = false;
};

// ============================================================================
// resolveTypingMode (对齐 typing-mode.ts:18-42)
// ============================================================================
TypingMode resolve_typing_mode(const TypingModeContext& ctx);

// ============================================================================
// TypingController interface (minimal, matches channels/typing.h callbacks)
// ============================================================================
struct TypingController {
    std::function<void()> start_typing_loop;
    std::function<void(const std::string& text)> start_typing_on_text;
    std::function<bool()> is_active;
    std::function<void()> refresh_typing_ttl;
    std::function<void()> mark_run_complete;
    std::function<void()> mark_dispatch_idle;
};

// ============================================================================
// TypingSignaler (对齐 typing-mode.ts:44-55)
// ============================================================================
struct TypingSignaler {
    TypingMode mode;
    bool should_start_immediately;
    bool should_start_on_message_start;
    bool should_start_on_text;
    bool should_start_on_reasoning;

    std::function<void()> signal_run_start;
    std::function<void()> signal_message_start;
    std::function<void(const std::string& text)> signal_text_delta;
    std::function<void()> signal_reasoning_delta;
    std::function<void()> signal_tool_start;
};

// ============================================================================
// createTypingSignaler (对齐 typing-mode.ts:57-156)
// ============================================================================
TypingSignaler create_typing_signaler(
    TypingController& typing,
    TypingMode mode,
    bool is_heartbeat
);

}  // namespace react
}  // namespace hunterclaw
