#pragma once

#include <string>
#include <optional>
#include <regex>
#include "hunterclaw/react/verbose_reasoning_config.h"

namespace hunterclaw {
namespace auto_reply {

// ✅ OpenClaw 对齐: 消息指令解析
// 参考: openclaw-analysis/src/auto-reply/reply/get-reply-directives.ts:97-165

/// 消息中的内联指令
struct MessageDirectives {
    /// Verbose 级别 (来自 /verbose <level> 指令)
    std::optional<react::VerboseLevel> verbose_level;

    /// Reasoning 级别 (来自 /reasoning <level> 指令)
    std::optional<react::ReasoningLevel> reasoning_level;

    /// 是否为纯指令消息 (没有其他内容)
    bool is_directive_only = false;

    /// 原始消息文本 (移除指令后)
    std::string text_without_directives;
};

/// 命令检测器
/// Aligned with openclaw: src/auto-reply/command-detection.ts
class CommandDetection {
public:
    // ─────────────────────────────────────────────────────────────
    // Directive parsing (aligned with get-reply-directives.ts)
    // ─────────────────────────────────────────────────────────────

    /// 从消息文本中解析指令
    static MessageDirectives parse_directives(const std::string& text);

    /// 检测是否为纯指令消息 (只包含指令,没有其他内容)
    static bool is_directive_only_message(const std::string& text);

    /// 从文本中移除所有指令
    static std::string strip_directives(const std::string& text);

    // ─────────────────────────────────────────────────────────────
    // Control command detection (aligned with command-detection.ts)
    // ─────────────────────────────────────────────────────────────

    /// Aligned with openclaw: hasControlCommand
    /// Returns true if text matches a registered chat command (e.g. /status, /stop, /help).
    static bool has_control_command(const std::string& text);

    /// Aligned with openclaw: isControlCommandMessage
    /// Returns true if text is a control command OR an abort trigger.
    static bool is_control_command_message(const std::string& text);

    /// Aligned with openclaw: hasInlineCommandTokens
    /// Coarse detection for inline directives/shortcuts (e.g. "hey /status").
    /// Errs on the side of false positives.
    static bool has_inline_command_tokens(const std::string& text);

    /// Aligned with openclaw: shouldComputeCommandAuthorized
    /// Returns true if command authorization should be computed for this message.
    static bool should_compute_command_authorized(const std::string& text);

    // ─────────────────────────────────────────────────────────────
    // Abort trigger detection (aligned with abort.ts)
    // ─────────────────────────────────────────────────────────────

    /// Aligned with openclaw: isAbortTrigger
    /// Returns true if text is a recognized abort/stop trigger word.
    static bool is_abort_trigger(const std::string& text);

    /// Aligned with openclaw: isAbortRequestText
    /// Returns true if text is "/stop" or an abort trigger.
    static bool is_abort_request_text(const std::string& text);

    /// Aligned with openclaw: normalizeCommandBody
    /// Strips @-mentions and structural prefixes before command matching.
    static std::string normalize_command_body(const std::string& text);

private:
    // Regex patterns for directive parsing (statically compiled)
    static const std::regex verbose_regex_;
    static const std::regex reasoning_regex_;
    static const std::regex thinking_regex_;

    // Regex for inline command token detection (aligned with openclaw)
    // Matches /cmd or !cmd at word boundary
    static const std::regex inline_command_token_re_;

    // Trailing punctuation to strip before abort trigger matching
    static const std::regex trailing_abort_punctuation_re_;
};

} // namespace auto_reply
} // namespace hunterclaw
