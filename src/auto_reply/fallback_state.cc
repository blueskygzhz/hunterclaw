#include "hunterclaw/auto_reply/fallback_state.h"
#include <algorithm>
#include <regex>
#include <set>

namespace hunterclaw {
namespace auto_reply {

namespace {
constexpr int FALLBACK_REASON_PART_MAX = 80;

std::string truncate_fallback_reason_part(const std::string& value, int max = FALLBACK_REASON_PART_MAX) {
    // 对齐 fallback-state.ts:17-25
    std::string text = std::regex_replace(value, std::regex("\\s+"), " ");
    // Trim
    auto start = text.find_first_not_of(" \t");
    auto end = text.find_last_not_of(" \t");
    if (start == std::string::npos) return "";
    text = text.substr(start, end - start + 1);
    
    if (static_cast<int>(text.size()) <= max) return text;
    return text.substr(0, std::max(0, max - 1)) + "\xe2\x80\xa6"; // "…"
}

// 对齐 fallback-state.ts:27-46
static const std::set<std::string> TRANSIENT_FALLBACK_REASONS = {"rate_limit", "overloaded", "timeout"};
static const std::regex TRANSIENT_ERROR_DETAIL_RE(
    "\\b(?:429|5\\d\\d|too many requests|usage limit|quota|try again in|retry[- ]after|seconds?|minutes?|hours?|temporarily unavailable|overloaded|service unavailable|throttl)\\b",
    std::regex_constants::icase
);

std::string format_provider_model_ref(const std::string& provider, const std::string& model) {
    if (provider.empty()) return model;
    return provider + "/" + model;
}
}  // namespace

// 对齐 fallback-state.ts:48-65
std::string format_fallback_attempt_reason(const RuntimeFallbackAttempt& attempt) {
    // Try transient error preview first
    if (attempt.error.has_value() && attempt.reason.has_value()) {
        auto raw = attempt.error.value();
        if (TRANSIENT_FALLBACK_REASONS.count(attempt.reason.value()) &&
            std::regex_search(raw, TRANSIENT_ERROR_DETAIL_RE)) {
            std::string formatted = std::regex_replace(raw, std::regex("^\\xe2\\x9a\\xa0\\xef\\xb8\\x8f\\s*"), "");
            formatted = std::regex_replace(formatted, std::regex("\\s+"), " ");
            auto start = formatted.find_first_not_of(" ");
            auto end = formatted.find_last_not_of(" ");
            if (start != std::string::npos) {
                formatted = formatted.substr(start, end - start + 1);
            }
            if (!formatted.empty() && !std::regex_search(formatted, std::regex("unknown error", std::regex_constants::icase))) {
                return formatted;
            }
        }
    }
    
    if (attempt.reason.has_value() && !attempt.reason.value().empty()) {
        auto reason = attempt.reason.value();
        std::replace(reason.begin(), reason.end(), '_', ' ');
        return reason;
    }
    if (attempt.code.has_value() && !attempt.code.value().empty()) {
        return attempt.code.value();
    }
    if (attempt.status.has_value()) {
        return "HTTP " + std::to_string(attempt.status.value());
    }
    return truncate_fallback_reason_part(attempt.error.value_or("error"));
}

// 对齐 fallback-state.ts:71-78
std::string build_fallback_reason_summary(const std::vector<RuntimeFallbackAttempt>& attempts) {
    std::string first_reason = attempts.empty()
        ? "selected model unavailable"
        : format_fallback_attempt_reason(attempts[0]);
    std::string more = attempts.size() > 1
        ? " (+" + std::to_string(attempts.size() - 1) + " more attempts)"
        : "";
    return truncate_fallback_reason_part(first_reason) + more;
}

// 对齐 fallback-state.ts:80-84
std::vector<std::string> build_fallback_attempt_summaries(const std::vector<RuntimeFallbackAttempt>& attempts) {
    std::vector<std::string> result;
    for (const auto& attempt : attempts) {
        auto summary = format_provider_model_ref(attempt.provider, attempt.model) + " " + format_fallback_attempt_reason(attempt);
        result.push_back(truncate_fallback_reason_part(summary));
    }
    return result;
}

// 对齐 fallback-state.ts:86-100
std::optional<std::string> build_fallback_notice(
    const std::string& selected_provider, const std::string& selected_model,
    const std::string& active_provider, const std::string& active_model,
    const std::vector<RuntimeFallbackAttempt>& attempts
) {
    auto selected = format_provider_model_ref(selected_provider, selected_model);
    auto active = format_provider_model_ref(active_provider, active_model);
    if (selected == active) return std::nullopt;
    auto reason_summary = build_fallback_reason_summary(attempts);
    return "\xe2\x86\xaa\xef\xb8\x8f Model Fallback: " + active + " (selected " + selected + "; " + reason_summary + ")";
}

// 对齐 fallback-state.ts:102-113
std::string build_fallback_cleared_notice(
    const std::string& selected_provider, const std::string& selected_model,
    const std::optional<std::string>& previous_active_model
) {
    auto selected = format_provider_model_ref(selected_provider, selected_model);
    if (previous_active_model.has_value() && previous_active_model.value() != selected) {
        return "\xe2\x86\xaa\xef\xb8\x8f Model Fallback cleared: " + selected + " (was " + previous_active_model.value() + ")";
    }
    return "\xe2\x86\xaa\xef\xb8\x8f Model Fallback cleared: " + selected;
}

// 对齐 fallback-state.ts:115-131
ActiveFallbackState resolve_active_fallback_state(
    const std::string& selected_model_ref,
    const std::string& active_model_ref,
    const FallbackNoticeState* state
) {
    if (!state) return {false, std::nullopt};
    
    bool fallback_active =
        selected_model_ref != active_model_ref &&
        state->fallback_notice_selected_model.value_or("") == selected_model_ref &&
        state->fallback_notice_active_model.value_or("") == active_model_ref;
    
    return {
        fallback_active,
        fallback_active ? state->fallback_notice_reason : std::nullopt
    };
}

// 对齐 fallback-state.ts:154-205
ResolvedFallbackTransition resolve_fallback_transition(
    const std::string& selected_provider, const std::string& selected_model,
    const std::string& active_provider, const std::string& active_model,
    const std::vector<RuntimeFallbackAttempt>& attempts,
    const FallbackNoticeState* state
) {
    auto selected_ref = format_provider_model_ref(selected_provider, selected_model);
    auto active_ref = format_provider_model_ref(active_provider, active_model);
    
    FallbackNoticeState prev;
    if (state) prev = *state;
    
    bool fallback_active = (selected_ref != active_ref);
    bool fallback_transitioned = fallback_active && (
        prev.fallback_notice_selected_model.value_or("") != selected_ref ||
        prev.fallback_notice_active_model.value_or("") != active_ref
    );
    bool fallback_cleared = !fallback_active && (
        prev.fallback_notice_selected_model.has_value() || prev.fallback_notice_active_model.has_value()
    );
    
    auto reason_summary = build_fallback_reason_summary(attempts);
    auto attempt_summaries = build_fallback_attempt_summaries(attempts);
    
    FallbackNoticeState next;
    if (fallback_active) {
        next.fallback_notice_selected_model = selected_ref;
        next.fallback_notice_active_model = active_ref;
        next.fallback_notice_reason = reason_summary;
    }
    
    bool state_changed =
        prev.fallback_notice_selected_model != next.fallback_notice_selected_model ||
        prev.fallback_notice_active_model != next.fallback_notice_active_model ||
        prev.fallback_notice_reason != next.fallback_notice_reason;
    
    return ResolvedFallbackTransition{
        selected_ref, active_ref,
        fallback_active, fallback_transitioned, fallback_cleared,
        reason_summary, attempt_summaries,
        prev, next, state_changed
    };
}

}  // namespace auto_reply
}  // namespace hunterclaw
