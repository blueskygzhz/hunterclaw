#pragma once

// 严格对齐 OpenClaw: auto-reply/fallback-state.ts (205行)
// Model fallback state tracking — notice building, transition detection, clearing.

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// 对齐 OpenClaw fallback-state.ts:5-7 + agent-runner-execution.ts RuntimeFallbackAttempt
// ============================================================================

struct RuntimeFallbackAttempt {
    std::string provider;
    std::string model;
    std::optional<std::string> reason;  // "rate_limit" | "overloaded" | "timeout" | etc.
    std::optional<std::string> code;
    std::optional<int> status;          // HTTP status
    std::optional<std::string> error;   // Raw error text
};

// 对齐 fallback-state.ts:12-15
struct FallbackNoticeState {
    std::optional<std::string> fallback_notice_selected_model;
    std::optional<std::string> fallback_notice_active_model;
    std::optional<std::string> fallback_notice_reason;
};

// ============================================================================
// 对齐 OpenClaw fallback-state.ts:48-65
// ============================================================================

std::string format_fallback_attempt_reason(const RuntimeFallbackAttempt& attempt);

// ============================================================================
// 对齐 OpenClaw fallback-state.ts:71-78
// ============================================================================

std::string build_fallback_reason_summary(const std::vector<RuntimeFallbackAttempt>& attempts);
std::vector<std::string> build_fallback_attempt_summaries(const std::vector<RuntimeFallbackAttempt>& attempts);

// ============================================================================
// 对齐 OpenClaw fallback-state.ts:86-100
// ============================================================================

std::optional<std::string> build_fallback_notice(
    const std::string& selected_provider, const std::string& selected_model,
    const std::string& active_provider, const std::string& active_model,
    const std::vector<RuntimeFallbackAttempt>& attempts
);

std::string build_fallback_cleared_notice(
    const std::string& selected_provider, const std::string& selected_model,
    const std::optional<std::string>& previous_active_model = std::nullopt
);

// ============================================================================
// 对齐 OpenClaw fallback-state.ts:115-131
// ============================================================================

struct ActiveFallbackState {
    bool active;
    std::optional<std::string> reason;
};

ActiveFallbackState resolve_active_fallback_state(
    const std::string& selected_model_ref,
    const std::string& active_model_ref,
    const FallbackNoticeState* state = nullptr
);

// ============================================================================
// 对齐 OpenClaw fallback-state.ts:133-205
// ============================================================================

struct ResolvedFallbackTransition {
    std::string selected_model_ref;
    std::string active_model_ref;
    bool fallback_active;
    bool fallback_transitioned;
    bool fallback_cleared;
    std::string reason_summary;
    std::vector<std::string> attempt_summaries;
    FallbackNoticeState previous_state;
    FallbackNoticeState next_state;
    bool state_changed;
};

ResolvedFallbackTransition resolve_fallback_transition(
    const std::string& selected_provider, const std::string& selected_model,
    const std::string& active_provider, const std::string& active_model,
    const std::vector<RuntimeFallbackAttempt>& attempts,
    const FallbackNoticeState* state = nullptr
);

}  // namespace auto_reply
}  // namespace hunterclaw
