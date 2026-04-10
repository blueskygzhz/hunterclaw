#include "hunterclaw/react/execution_guard.h"
#include <algorithm>
#include <chrono>
#include <regex>
#include <sstream>

namespace hunterclaw {
namespace react {

namespace {
std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    auto end = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}
} // anon

// ============================================================================
// 错误分类 (对齐 pi-embedded-helpers.ts)
// ============================================================================

// 对齐: isBillingErrorMessage
bool is_billing_error_message(const std::string& message) {
    auto lower = to_lower(message);
    return (lower.find("billing") != std::string::npos &&
            (lower.find("hard limit") != std::string::npos ||
             lower.find("exceeded") != std::string::npos ||
             lower.find("not active") != std::string::npos)) ||
           lower.find("your credit balance is too low") != std::string::npos ||
           lower.find("insufficient_quota") != std::string::npos ||
           lower.find("payment required") != std::string::npos;
}

// 对齐: isLikelyContextOverflowError
bool is_likely_context_overflow_error(const std::string& message) {
    auto lower = to_lower(message);
    return (lower.find("context") != std::string::npos &&
            lower.find("overflow") != std::string::npos) ||
           lower.find("too long") != std::string::npos ||
           (lower.find("maximum") != std::string::npos &&
            lower.find("token") != std::string::npos) ||
           (lower.find("exceeds") != std::string::npos &&
            lower.find("limit") != std::string::npos) ||
           lower.find("prompt is too long") != std::string::npos ||
           lower.find("max_tokens") != std::string::npos;
}

// 对齐: isCompactionFailureError
bool is_compaction_failure_error(const std::string& message) {
    auto lower = to_lower(message);
    return (lower.find("compaction") != std::string::npos &&
            lower.find("fail") != std::string::npos) ||
           (lower.find("context") != std::string::npos &&
            lower.find("overflow") != std::string::npos &&
            lower.find("compaction") != std::string::npos);
}

// 对齐: isRateLimitErrorMessage
bool is_rate_limit_error_message(const std::string& message) {
    auto lower = to_lower(message);
    return lower.find("rate_limit") != std::string::npos ||
           lower.find("rate limit") != std::string::npos ||
           lower.find("too many requests") != std::string::npos ||
           lower.find("429") != std::string::npos;
}

// 对齐: isOverloadedErrorMessage
bool is_overloaded_error_message(const std::string& message) {
    auto lower = to_lower(message);
    return lower.find("overloaded") != std::string::npos ||
           lower.find("503") != std::string::npos ||
           lower.find("temporarily unavailable") != std::string::npos;
}

// 对齐: isTransientHttpError
bool is_transient_http_error(const std::string& message) {
    auto lower = to_lower(message);
    return lower.find("502") != std::string::npos ||
           lower.find("521") != std::string::npos ||
           lower.find("524") != std::string::npos ||
           lower.find("econnreset") != std::string::npos ||
           lower.find("socket hang up") != std::string::npos ||
           lower.find("fetch failed") != std::string::npos;
}

// 对齐: isToolResultTurnMismatchError
bool is_tool_result_turn_mismatch_error(const std::string& message) {
    auto lower = to_lower(message);
    return lower.find("toolresult") != std::string::npos &&
           lower.find("tooluse") != std::string::npos &&
           lower.find("exceeds the number") != std::string::npos &&
           lower.find("previous turn") != std::string::npos;
}

// 对齐: isRoleOrderingError
bool is_role_ordering_error(const std::string& message) {
    auto lower = to_lower(message);
    return lower.find("incorrect role information") != std::string::npos ||
           lower.find("roles must alternate") != std::string::npos;
}

// 对齐: isSessionCorruption
bool is_session_corruption_error(const std::string& message) {
    auto lower = to_lower(message);
    return lower.find("function call turn comes immediately after") != std::string::npos;
}

ErrorClass classify_error(const std::string& message) {
    if (is_billing_error_message(message)) return ErrorClass::Billing;
    if (is_compaction_failure_error(message)) return ErrorClass::CompactionFailure;
    if (is_session_corruption_error(message)) return ErrorClass::SessionCorruption;
    if (is_role_ordering_error(message)) return ErrorClass::RoleOrdering;
    if (is_likely_context_overflow_error(message)) return ErrorClass::ContextOverflow;
    if (is_rate_limit_error_message(message)) return ErrorClass::RateLimit;
    if (is_overloaded_error_message(message)) return ErrorClass::Overloaded;
    if (is_transient_http_error(message)) return ErrorClass::TransientHttp;
    if (is_tool_result_turn_mismatch_error(message)) return ErrorClass::ToolResultTurnMismatch;
    return ErrorClass::Unknown;
}

// ============================================================================
// 错误消息构建 (对齐 agent-runner-execution.ts:281-370)
// ============================================================================

std::string build_rate_limit_cooldown_message(
    std::optional<int64_t> soonest_cooldown_expiry_ms
) {
    if (!soonest_cooldown_expiry_ms.has_value()) {
        return "⚠️ All models are temporarily rate-limited. Please try again in a few minutes.";
    }
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto expiry = *soonest_cooldown_expiry_ms;
    if (expiry > now) {
        int secs_left = std::max(1, static_cast<int>((expiry - now + 999) / 1000));
        if (secs_left <= 60) {
            return "⚠️ Rate-limited — ready in ~" + std::to_string(secs_left) +
                   "s. Please wait a moment.";
        }
        int mins_left = (secs_left + 59) / 60;
        return "⚠️ Rate-limited — ready in ~" + std::to_string(mins_left) +
               " min. Please try again shortly.";
    }
    return "⚠️ All models are temporarily rate-limited. Please try again in a few minutes.";
}

std::string collapse_repeated_failure_detail(const std::string& message) {
    // Split by " | " and check if all parts are identical
    std::vector<std::string> parts;
    std::istringstream iss(message);
    std::string part;
    std::string delimiter = " | ";
    std::string remaining = message;
    size_t pos;
    while ((pos = remaining.find(delimiter)) != std::string::npos) {
        part = trim(remaining.substr(0, pos));
        if (!part.empty()) parts.push_back(part);
        remaining = remaining.substr(pos + delimiter.size());
    }
    part = trim(remaining);
    if (!part.empty()) parts.push_back(part);

    if (parts.size() >= 2) {
        bool all_same = true;
        for (size_t i = 1; i < parts.size(); ++i) {
            if (parts[i] != parts[0]) { all_same = false; break; }
        }
        if (all_same) return parts[0];
    }
    return trim(message);
}

std::optional<std::string> build_missing_api_key_failure_text(const std::string& message) {
    auto normalized = collapse_repeated_failure_detail(message);
    // Match: No API key found for provider "xxx"
    // Match: No API key found for provider "xxx"
    std::regex re("No API key found for provider \"([^\"]+)\"");
    std::smatch match;
    if (!std::regex_search(normalized, match, re)) return std::nullopt;
    auto provider = to_lower(trim(match[1].str()));
    if (provider.empty()) return std::nullopt;

    if (provider == "openai" && normalized.find("OpenAI Codex OAuth") != std::string::npos) {
        return "⚠️ Missing API key for OpenAI on the gateway. Use `openai-codex/gpt-5.4` for OAuth, or set `OPENAI_API_KEY`, then try again.";
    }
    static const std::vector<std::string> safe_providers = {
        "anthropic", "google", "openai", "openai-codex"
    };
    for (const auto& sp : safe_providers) {
        if (provider == sp) {
            return "⚠️ Missing API key for provider \"" + provider +
                   "\". Configure the gateway auth for that provider, then try again.";
        }
    }
    return "⚠️ Missing API key for the selected provider on the gateway. Configure provider auth, then try again.";
}

std::string build_external_run_failure_text(const std::string& message) {
    auto normalized = collapse_repeated_failure_detail(message);
    if (is_tool_result_turn_mismatch_error(normalized)) {
        return "⚠️ Session history got out of sync. Please try again, or use /new to start a fresh session.";
    }
    auto missing_key = build_missing_api_key_failure_text(normalized);
    if (missing_key.has_value()) return *missing_key;
    return "⚠️ Something went wrong while processing your request. Please try again, or use /new to start a fresh session.";
}

std::string build_restart_lifecycle_reply_text() {
    return "⚠️ Gateway is restarting. Please wait a few seconds and try again.";
}

// ============================================================================
// GPT Chat 简洁性控制 (对齐 agent-runner-execution.ts:372-501)
// ============================================================================

bool should_apply_openai_gpt_chat_guard(
    const std::string& provider,
    const std::string& model
) {
    if (provider != "openai" && provider != "openai-codex") return false;
    std::regex re(R"(^gpt-5(?:[.\-]|$))", std::regex_constants::icase);
    return std::regex_search(model, re);
}

int count_chat_reply_sentences(const std::string& text) {
    auto trimmed = trim(text);
    if (trimmed.empty()) return 0;
    // Split by sentence-ending punctuation followed by whitespace
    std::regex re(R"((?<=[.!?])\s+)");
    std::sregex_token_iterator it(trimmed.begin(), trimmed.end(), re, -1);
    std::sregex_token_iterator end;
    int count = 0;
    for (; it != end; ++it) {
        auto s = trim(it->str());
        if (!s.empty()) count++;
    }
    return count;
}

int score_chatty_final_reply_text(const std::string& text) {
    auto trimmed = trim(text);
    if (trimmed.empty()) return 0;
    int score = 0;
    auto sentence_count = count_chat_reply_sentences(trimmed);
    if (trimmed.size() > 900) score++;
    if (trimmed.size() > 1500) score++;
    if (sentence_count > 6) score++;
    if (sentence_count > 10) score++;
    // Count double-newline separated paragraphs
    {
        int para_count = 0;
        size_t pos = 0;
        while (pos < trimmed.size()) {
            auto next = trimmed.find("\n\n", pos);
            if (next == std::string::npos) { para_count++; break; }
            para_count++;
            pos = next + 2;
        }
        if (para_count >= 3) score++;
    }
    // Summary pattern detection
    {
        std::regex summary_re(
            R"(\b(?:in summary|to summarize|here(?:'s| is) what|what changed|what I verified)\b)",
            std::regex_constants::icase);
        if (std::regex_search(trimmed, summary_re)) score++;
    }
    return score;
}

std::string shorten_chatty_final_reply_text(
    const std::string& text, int max_chars, int max_sentences
) {
    auto trimmed = trim(text);
    if (trimmed.empty()) return trimmed;

    // Split into sentences
    std::regex re(R"((?<=[.!?])\s+)");
    std::sregex_token_iterator it(trimmed.begin(), trimmed.end(), re, -1);
    std::sregex_token_iterator end;
    std::vector<std::string> sentences;
    for (; it != end; ++it) {
        auto s = trim(it->str());
        if (!s.empty()) sentences.push_back(s);
    }

    // Take at most max_sentences
    std::string shortened;
    int count = 0;
    for (const auto& s : sentences) {
        if (count >= max_sentences) break;
        if (!shortened.empty()) shortened += " ";
        shortened += s;
        count++;
    }
    if (shortened.empty()) {
        shortened = trimmed.substr(0, max_chars);
        auto end_pos = shortened.find_last_not_of(" \t\n\r");
        if (end_pos != std::string::npos) shortened = shortened.substr(0, end_pos + 1);
    }
    if (static_cast<int>(shortened.size()) > max_chars) {
        shortened = shortened.substr(0, max_chars);
        auto end_pos = shortened.find_last_not_of(" \t\n\r");
        if (end_pos != std::string::npos) shortened = shortened.substr(0, end_pos + 1);
    }
    if (shortened.size() >= trimmed.size()) return trimmed;
    // Strip trailing punctuation
    while (!shortened.empty()) {
        char c = shortened.back();
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '-') {
            shortened.pop_back();
        } else break;
    }
    auto end_pos = shortened.find_last_not_of(" \t\n\r");
    if (end_pos != std::string::npos) shortened = shortened.substr(0, end_pos + 1);
    return shortened + "...";
}

void apply_openai_gpt_chat_reply_guard(
    const std::string& provider,
    const std::string& model,
    const std::string& command_body,
    bool is_heartbeat,
    std::vector<ReplyText>& payloads
) {
    if (is_heartbeat || !should_apply_openai_gpt_chat_guard(provider, model) || payloads.empty()) {
        return;
    }
    auto trimmed_command = trim(command_body);
    bool is_ack_turn = is_likely_execution_ack_prompt(trimmed_command);
    bool allow_soft_cap = !is_ack_turn &&
        !trimmed_command.empty() &&
        trimmed_command.size() <= 120;
    if (allow_soft_cap) {
        std::regex depth_re(
            R"(\b(?:detail|detailed|depth|deep dive|explain|compare|walk me through|why|how)\b)",
            std::regex_constants::icase);
        if (std::regex_search(trimmed_command, depth_re)) allow_soft_cap = false;
    }

    for (auto& payload : payloads) {
        if (payload.text.empty() || payload.is_error || payload.is_reasoning ||
            payload.has_media || payload.has_interactive || payload.has_code_block) {
            continue;
        }
        if (is_ack_turn) {
            payload.text = shorten_chatty_final_reply_text(
                payload.text, GPT_CHAT_BREVITY_ACK_MAX_CHARS, GPT_CHAT_BREVITY_ACK_MAX_SENTENCES);
            continue;
        }
        if (allow_soft_cap && score_chatty_final_reply_text(payload.text) >= 4) {
            payload.text = shorten_chatty_final_reply_text(
                payload.text, GPT_CHAT_BREVITY_SOFT_MAX_CHARS, GPT_CHAT_BREVITY_SOFT_MAX_SENTENCES);
        }
    }
}

// ============================================================================
// Streaming 文本规范化 (对齐 agent-runner-execution.ts:702-747)
// ============================================================================

const std::string SILENT_REPLY_TOKEN = "NO_REPLY";
const std::string HEARTBEAT_TOKEN = "HEARTBEAT_OK";

bool is_silent_reply_text(const std::string& text) {
    auto trimmed = trim(text);
    return trimmed == SILENT_REPLY_TOKEN;
}

bool is_silent_reply_prefix_text(const std::string& text) {
    auto trimmed = trim(text);
    if (trimmed.empty()) return false;
    // Check if text starts with silent token and rest is all whitespace
    if (trimmed.find(SILENT_REPLY_TOKEN) == 0) {
        auto rest = trim(trimmed.substr(SILENT_REPLY_TOKEN.size()));
        return rest.empty();
    }
    return false;
}

std::string strip_leading_silent_token(const std::string& text) {
    auto pos = text.find(SILENT_REPLY_TOKEN);
    if (pos == std::string::npos) return text;
    return text.substr(pos + SILENT_REPLY_TOKEN.size());
}

HeartbeatStripResult strip_heartbeat_token(const std::string& text) {
    HeartbeatStripResult result;
    result.text = text;
    auto pos = text.find(HEARTBEAT_TOKEN);
    if (pos == std::string::npos) return result;

    result.did_strip = true;
    std::string stripped;
    size_t search_pos = 0;
    while (true) {
        auto found = text.find(HEARTBEAT_TOKEN, search_pos);
        if (found == std::string::npos) {
            stripped += text.substr(search_pos);
            break;
        }
        stripped += text.substr(search_pos, found - search_pos);
        search_pos = found + HEARTBEAT_TOKEN.size();
    }
    result.text = stripped;
    auto trimmed = trim(stripped);
    result.should_skip = trimmed.empty();
    return result;
}

std::string sanitize_user_facing_text(const std::string& text, bool /*error_context*/) {
    // Basic sanitization: strip control chars, keep printable
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        if (c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c != 127)) {
            result.push_back(c);
        }
    }
    return result;
}

NormalizedText normalize_streaming_text(
    const std::string& text,
    bool is_heartbeat,
    bool silent_expected,
    bool is_error
) {
    NormalizedText result;
    if (silent_expected) {
        result.skip = true;
        return result;
    }
    std::string processed = text;
    // Strip heartbeat tokens
    if (!is_heartbeat && processed.find(HEARTBEAT_TOKEN) != std::string::npos) {
        auto stripped = strip_heartbeat_token(processed);
        if (stripped.should_skip) {
            result.skip = true;
            return result;
        }
        processed = stripped.text;
    }
    if (is_silent_reply_text(processed)) {
        result.skip = true;
        return result;
    }
    if (is_silent_reply_prefix_text(processed)) {
        result.skip = true;
        return result;
    }
    // Strip leading silent token
    if (processed.find(SILENT_REPLY_TOKEN) == 0) {
        processed = strip_leading_silent_token(processed);
    }
    if (processed.empty()) {
        result.skip = true;
        return result;
    }
    auto sanitized = sanitize_user_facing_text(processed, is_error);
    if (trim(sanitized).empty()) {
        result.skip = true;
        return result;
    }
    result.text = sanitized;
    result.skip = false;
    return result;
}

// ============================================================================
// FallbackSelectionState (对齐 agent-runner-execution.ts:188-202)
// ============================================================================

FallbackSelectionState build_fallback_selection_state(
    const std::string& provider,
    const std::string& model,
    const std::optional<std::string>& auth_profile_id,
    const std::optional<std::string>& auth_profile_id_source
) {
    FallbackSelectionState state;
    state.provider_override = provider;
    state.model_override = model;
    state.model_override_source = "auto";
    state.auth_profile_override = auth_profile_id;
    state.auth_profile_override_source = auth_profile_id.has_value()
        ? auth_profile_id_source : std::nullopt;
    return state;
}

// 对齐: isLikelyExecutionAckPrompt
bool is_likely_execution_ack_prompt(const std::string& prompt) {
    auto trimmed = trim(prompt);
    if (trimmed.empty()) return false;
    auto lower = to_lower(trimmed);
    // Short affirmative prompts that acknowledge tool execution
    static const std::vector<std::string> ack_patterns = {
        "ok", "okay", "sure", "yes", "yep", "yeah", "go ahead",
        "do it", "proceed", "continue", "approved", "confirm",
        "looks good", "lgtm", "go for it", "sounds good",
    };
    for (const auto& pat : ack_patterns) {
        if (lower == pat) return true;
        // Also match with trailing punctuation
        if (lower.size() == pat.size() + 1 &&
            lower.substr(0, pat.size()) == pat &&
            (lower.back() == '.' || lower.back() == '!' || lower.back() == '?')) {
            return true;
        }
    }
    return trimmed.size() <= 30 && trimmed.find(' ') == std::string::npos;
}

} // namespace react
} // namespace hunterclaw
