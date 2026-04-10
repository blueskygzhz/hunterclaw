#include "hunterclaw/react/runner_helpers.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace hunterclaw {
namespace react {

// ============================================================================
// UNSCHEDULED_REMINDER_NOTE (对齐 agent-runner-reminder-guard.ts:5-6)
// ============================================================================

const std::string UNSCHEDULED_REMINDER_NOTE =
    "Note: I did not schedule a reminder in this turn, so this will not trigger automatically.";

// ============================================================================
// Reminder Commitment Patterns (对齐 agent-runner-reminder-guard.ts:8-11)
// ============================================================================

static const std::vector<std::regex> REMINDER_COMMITMENT_PATTERNS = {
    std::regex(R"(\b(?:i\s*['']?ll|i will)\s+(?:make sure to\s+)?(?:remember|remind|ping|follow up|follow-up|check back|circle back)\b)", std::regex_constants::icase),
    std::regex(R"(\b(?:i\s*['']?ll|i will)\s+(?:set|create|schedule)\s+(?:a\s+)?reminder\b)", std::regex_constants::icase),
};

// ============================================================================
// hasUnbackedReminderCommitment (对齐 agent-runner-reminder-guard.ts:13-22)
// ============================================================================

bool has_unbacked_reminder_commitment(const std::string& text) {
    if (text.empty()) return false;

    // 对齐: if (normalized.includes(UNSCHEDULED_REMINDER_NOTE)) return false
    if (text.find(UNSCHEDULED_REMINDER_NOTE) != std::string::npos) {
        return false;
    }

    for (const auto& pattern : REMINDER_COMMITMENT_PATTERNS) {
        if (std::regex_search(text, pattern)) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// appendUnscheduledReminderNote (对齐 agent-runner-reminder-guard.ts:49-65)
// ============================================================================

std::string append_unscheduled_reminder_note(const std::string& text) {
    if (!has_unbacked_reminder_commitment(text)) {
        return text;
    }
    // 对齐: const trimmed = payload.text.trimEnd()
    std::string trimmed = text;
    auto e = trimmed.find_last_not_of(" \t\n\r");
    if (e != std::string::npos) {
        trimmed = trimmed.substr(0, e + 1);
    }
    return trimmed + "\n\n" + UNSCHEDULED_REMINDER_NOTE;
}

// ============================================================================
// Token count formatting helper (对齐 usage-format.ts:formatTokenCount)
// ============================================================================

static std::string format_token_count(int64_t tokens) {
    if (tokens < 1000) return std::to_string(tokens);
    if (tokens < 1000000) {
        double k = static_cast<double>(tokens) / 1000.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << k << "k";
        return oss.str();
    }
    double m = static_cast<double>(tokens) / 1000000.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << m << "M";
    return oss.str();
}

static std::string format_usd(double amount) {
    std::ostringstream oss;
    oss << "$" << std::fixed << std::setprecision(4) << amount;
    return oss.str();
}

// ============================================================================
// formatResponseUsageLine (对齐 agent-runner-usage-line.ts:4-45)
// ============================================================================

std::optional<std::string> format_response_usage_line(const UsageLineParams& params) {
    if (!params.input_tokens.has_value() && !params.output_tokens.has_value()) {
        return std::nullopt;
    }

    std::string input_label = params.input_tokens.has_value()
        ? format_token_count(*params.input_tokens) : "?";
    std::string output_label = params.output_tokens.has_value()
        ? format_token_count(*params.output_tokens) : "?";

    std::string suffix;
    if (params.show_cost && params.cost_config.has_value() &&
        params.input_tokens.has_value() && params.output_tokens.has_value()) {
        double cost = 0.0;
        cost += static_cast<double>(*params.input_tokens) * params.cost_config->input / 1e6;
        cost += static_cast<double>(*params.output_tokens) * params.cost_config->output / 1e6;
        if (params.cache_read_tokens.has_value()) {
            cost += static_cast<double>(*params.cache_read_tokens) * params.cost_config->cache_read / 1e6;
        }
        if (params.cache_write_tokens.has_value()) {
            cost += static_cast<double>(*params.cache_write_tokens) * params.cost_config->cache_write / 1e6;
        }
        suffix = " \xC2\xB7 est " + format_usd(cost);  // " · est $X.XXXX"
    }

    return "Usage: " + input_label + " in / " + output_label + " out" + suffix;
}

// ============================================================================
// appendUsageLine (对齐 agent-runner-usage-line.ts:47-68)
// ============================================================================

std::string append_usage_line(const std::string& text, const std::string& line) {
    if (text.empty()) return line;
    std::string separator = text.back() == '\n' ? "" : "\n";
    return text + separator + line;
}

// ============================================================================
// 对齐 agent-runner-helpers.ts:12-16: isAudioPayload
// ============================================================================

static bool has_audio_extension(const std::string& url) {
    // Common audio extensions
    static const std::vector<std::string> audio_exts = {
        ".mp3", ".wav", ".ogg", ".m4a", ".aac", ".flac", ".opus", ".wma", ".webm"
    };
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& ext : audio_exts) {
        if (lower.size() >= ext.size() &&
            lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

bool is_audio_payload(const std::vector<std::string>& media_urls) {
    if (media_urls.empty()) return false;
    return std::any_of(media_urls.begin(), media_urls.end(), has_audio_extension);
}

// ============================================================================
// 对齐 agent-runner-helpers.ts:18-55: verbose gate (shouldEmitToolResult/Output)
// ============================================================================

VerboseGate create_should_emit_tool_result(const std::string& resolved_verbose_level) {
    // 对齐: shouldEmit = (level) => level !== "off"
    auto level = resolved_verbose_level.empty() ? "off" : resolved_verbose_level;
    return [level]() -> bool {
        return level != "off";
    };
}

VerboseGate create_should_emit_tool_output(const std::string& resolved_verbose_level) {
    // 对齐: shouldEmit = (level) => level === "full"
    auto level = resolved_verbose_level.empty() ? "off" : resolved_verbose_level;
    return [level]() -> bool {
        return level == "full";
    };
}

// ============================================================================
// 对齐 body.ts:13-53: applySessionHints
// ============================================================================

std::string apply_session_hints(
    const std::string& base_body,
    bool aborted_last_run
) {
    if (!aborted_last_run) return base_body;
    return "Note: The previous agent run was aborted by the user. "
           "Resume carefully or ask for clarification.\n\n" + base_body;
}

}  // namespace react
}  // namespace hunterclaw
