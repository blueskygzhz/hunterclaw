// Aligned with openclaw:
//   src/auto-reply/command-detection.ts
//   src/auto-reply/reply/abort.ts
//   src/auto-reply/reply/get-reply-directives.ts

#include "hunterclaw/auto_reply/command_detection.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Static regex patterns
// ============================================================================

const std::regex CommandDetection::verbose_regex_(
    R"(/verbose\s+(off|on|full))",
    std::regex_constants::icase
);

const std::regex CommandDetection::reasoning_regex_(
    R"(/reasoning\s+(off|on|stream))",
    std::regex_constants::icase
);

const std::regex CommandDetection::thinking_regex_(
    R"(/think(?:ing)?\s+(off|on|verbose))",
    std::regex_constants::icase
);

// Aligned with openclaw: /(?:^|\s)[/!][a-z]/i
const std::regex CommandDetection::inline_command_token_re_(
    R"((?:^|\s)[/!][a-zA-Z])",
    std::regex_constants::ECMAScript
);

// Aligned with openclaw: TRAILING_ABORT_PUNCTUATION_RE
const std::regex CommandDetection::trailing_abort_punctuation_re_(
    R"([.!?…,，。;；:：'"'")\]}]+$)",
    std::regex_constants::ECMAScript
);

// ============================================================================
// Abort triggers (aligned with openclaw: ABORT_TRIGGERS set in abort.ts)
// ============================================================================

static const std::set<std::string>& abort_triggers() {
    static const std::set<std::string> TRIGGERS = {
        "stop", "esc", "abort", "wait", "exit", "interrupt",
        "detente", "deten", "detén", "arrete", "arrête",
        "停止", "やめて", "止めて", "रुको", "توقف",
        "стоп", "остановись", "останови", "остановить", "прекрати",
        "halt", "anhalten", "aufhören", "hoer auf", "stopp",
        "pare",
        "stop openclaw", "openclaw stop",
        "stop action", "stop current action",
        "stop run", "stop current run",
        "stop agent", "stop the agent",
        "stop don't do anything", "stop dont do anything",
        "stop do not do anything", "stop doing anything",
        "do not do that",
        "please stop", "stop please",
    };
    return TRIGGERS;
}

// ============================================================================
// Registered control commands (aligned with openclaw: listChatCommands)
// These are the slash-commands that trigger command handling, not LLM reply.
// ============================================================================

static const std::set<std::string>& control_commands() {
    static const std::set<std::string> COMMANDS = {
        "/stop", "/abort", "/status", "/help", "/reset", "/clear",
        "/session", "/model", "/verbose", "/reasoning", "/thinking",
        "/memory", "/compact", "/export", "/config", "/send",
        "/info", "/context", "/approve", "/deny",
    };
    return COMMANDS;
}

// ============================================================================
// normalize_command_body
// Aligned with openclaw: normalizeCommandBody (commands-registry.ts)
// Strips @-mentions and structural prefixes before command matching.
// ============================================================================

std::string CommandDetection::normalize_command_body(const std::string& text) {
    std::string result = text;

    // Strip leading @-mentions (e.g. "@bot /stop" → "/stop")
    static const std::regex mention_prefix_re(R"(^@\S+\s*)");
    result = std::regex_replace(result, mention_prefix_re, "");

    // Trim
    size_t start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(" \t\n\r");
    return result.substr(start, end - start + 1);
}

// ============================================================================
// normalizeAbortTriggerText
// Aligned with openclaw: normalizeAbortTriggerText (abort.ts)
// ============================================================================

static std::string normalize_abort_trigger_text(const std::string& text) {
    std::string result = text;

    // Trim
    size_t start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(" \t\n\r");
    result = result.substr(start, end - start + 1);

    // Lowercase
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Normalize quotes: ' ` → '
    for (auto& c : result) {
        if (c == '\'' || c == '`') c = '\'';
    }

    // Collapse whitespace
    static const std::regex ws_re(R"(\s+)");
    result = std::regex_replace(result, ws_re, " ");

    // Strip trailing punctuation (aligned with openclaw: TRAILING_ABORT_PUNCTUATION_RE)
    static const std::regex trailing_punct_re(
        R"([.!?…,，。;；:：'"'")\]}]+$)",
        std::regex_constants::ECMAScript
    );
    result = std::regex_replace(result, trailing_punct_re, "");

    // Trim again after stripping
    start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    end = result.find_last_not_of(" \t\n\r");
    return result.substr(start, end - start + 1);
}

// ============================================================================
// is_abort_trigger
// Aligned with openclaw: isAbortTrigger (abort.ts)
// ============================================================================

bool CommandDetection::is_abort_trigger(const std::string& text) {
    if (text.empty()) return false;
    const std::string normalized = normalize_abort_trigger_text(text);
    return abort_triggers().count(normalized) > 0;
}

// ============================================================================
// is_abort_request_text
// Aligned with openclaw: isAbortRequestText (abort.ts)
// ============================================================================

bool CommandDetection::is_abort_request_text(const std::string& text) {
    if (text.empty()) return false;
    const std::string normalized = normalize_command_body(text);
    if (normalized.empty()) return false;

    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "/stop") return true;

    // Also check normalize_abort_trigger_text("/stop") == "/stop"
    const std::string norm_lower = normalize_abort_trigger_text(lower);
    if (norm_lower == "/stop") return true;

    return is_abort_trigger(lower);
}

// ============================================================================
// has_control_command
// Aligned with openclaw: hasControlCommand (command-detection.ts)
// ============================================================================

bool CommandDetection::has_control_command(const std::string& text) {
    if (text.empty()) return false;

    const std::string trimmed_text = [&]() {
        size_t s = text.find_first_not_of(" \t\n\r");
        if (s == std::string::npos) return std::string{};
        size_t e = text.find_last_not_of(" \t\n\r");
        return text.substr(s, e - s + 1);
    }();
    if (trimmed_text.empty()) return false;

    const std::string normalized = normalize_command_body(trimmed_text);
    if (normalized.empty()) return false;

    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& cmd : control_commands()) {
        // Exact match
        if (lower == cmd) return true;

        // Prefix match with space (commands that accept args, e.g. "/model claude-3")
        if (lower.size() > cmd.size() &&
            lower.substr(0, cmd.size()) == cmd &&
            std::isspace(static_cast<unsigned char>(lower[cmd.size()]))) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// is_control_command_message
// Aligned with openclaw: isControlCommandMessage (command-detection.ts)
// ============================================================================

bool CommandDetection::is_control_command_message(const std::string& text) {
    if (text.empty()) return false;

    const std::string trimmed = [&]() {
        size_t s = text.find_first_not_of(" \t\n\r");
        if (s == std::string::npos) return std::string{};
        size_t e = text.find_last_not_of(" \t\n\r");
        return text.substr(s, e - s + 1);
    }();
    if (trimmed.empty()) return false;

    if (has_control_command(trimmed)) return true;

    // Also check abort triggers (aligned with openclaw)
    const std::string normalized = normalize_command_body(trimmed);
    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return is_abort_trigger(lower);
}

// ============================================================================
// has_inline_command_tokens
// Aligned with openclaw: hasInlineCommandTokens (command-detection.ts)
// Coarse detection — errs on the side of false positives.
// ============================================================================

bool CommandDetection::has_inline_command_tokens(const std::string& text) {
    if (text.empty()) return false;
    const std::string trimmed = [&]() {
        size_t s = text.find_first_not_of(" \t\n\r");
        if (s == std::string::npos) return std::string{};
        size_t e = text.find_last_not_of(" \t\n\r");
        return text.substr(s, e - s + 1);
    }();
    if (trimmed.empty()) return false;
    return std::regex_search(trimmed, inline_command_token_re_);
}

// ============================================================================
// should_compute_command_authorized
// Aligned with openclaw: shouldComputeCommandAuthorized (command-detection.ts)
// ============================================================================

bool CommandDetection::should_compute_command_authorized(const std::string& text) {
    return is_control_command_message(text) || has_inline_command_tokens(text);
}

// ============================================================================
// Directive parsing (aligned with get-reply-directives.ts)
// ============================================================================

MessageDirectives CommandDetection::parse_directives(const std::string& text) {
    MessageDirectives directives;
    std::smatch match;

    // 1. Parse /verbose directive
    if (std::regex_search(text, match, verbose_regex_)) {
        std::string level = match[1].str();
        std::transform(level.begin(), level.end(), level.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (level == "off")       directives.verbose_level = react::VerboseLevel::Off;
        else if (level == "on")   directives.verbose_level = react::VerboseLevel::On;
        else if (level == "full") directives.verbose_level = react::VerboseLevel::Full;
    }

    // 2. Parse /reasoning directive
    if (std::regex_search(text, match, reasoning_regex_)) {
        std::string level = match[1].str();
        std::transform(level.begin(), level.end(), level.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (level == "off")         directives.reasoning_level = react::ReasoningLevel::Off;
        else if (level == "on")     directives.reasoning_level = react::ReasoningLevel::On;
        else if (level == "stream") directives.reasoning_level = react::ReasoningLevel::Stream;
    }

    // 3. Compute text_without_directives and is_directive_only
    directives.text_without_directives = strip_directives(text);
    directives.is_directive_only = is_directive_only_message(text);

    return directives;
}

bool CommandDetection::is_directive_only_message(const std::string& text) {
    std::string stripped = strip_directives(text);
    stripped.erase(
        std::remove_if(stripped.begin(), stripped.end(),
                       [](unsigned char c) { return std::isspace(c); }),
        stripped.end()
    );
    return stripped.empty();
}

std::string CommandDetection::strip_directives(const std::string& text) {
    std::string result = text;
    result = std::regex_replace(result, verbose_regex_, "");
    result = std::regex_replace(result, reasoning_regex_, "");
    result = std::regex_replace(result, thinking_regex_, "");

    // Collapse whitespace
    static const std::regex ws_re(R"(\s+)");
    result = std::regex_replace(result, ws_re, " ");

    // Trim
    size_t start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(" \t\n\r");
    return result.substr(start, end - start + 1);
}

} // namespace auto_reply
} // namespace hunterclaw
