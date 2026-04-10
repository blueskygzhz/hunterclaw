#pragma once
// ============================================================================
// execution_guard.h — 严格对齐 OpenClaw agent-runner-execution.ts (1547行)
//
// 精细错误分类、恢复策略、streaming 文本规范化、GPT chat 简洁性控制
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>

namespace hunterclaw {
namespace react {

// ============================================================================
// 对齐 agent-runner-execution.ts:82-84: 常量
// ============================================================================
constexpr int MAX_LIVE_SWITCH_RETRIES = 2;
constexpr int GPT_CHAT_BREVITY_ACK_MAX_CHARS = 420;
constexpr int GPT_CHAT_BREVITY_ACK_MAX_SENTENCES = 3;
constexpr int GPT_CHAT_BREVITY_SOFT_MAX_CHARS = 900;
constexpr int GPT_CHAT_BREVITY_SOFT_MAX_SENTENCES = 6;
constexpr int TRANSIENT_HTTP_RETRY_DELAY_MS = 2500;

// ============================================================================
// 对齐 agent-runner-execution.ts:88-95: RuntimeFallbackAttempt
// ============================================================================
struct RuntimeFallbackAttempt {
    std::string provider;
    std::string model;
    std::string error;
    std::optional<std::string> reason;
    std::optional<int> status;
    std::optional<std::string> code;
};

// ============================================================================
// 对齐 agent-runner-execution.ts:97-110: AgentRunLoopResult
// ============================================================================
enum class RunOutcomeKind { Success, Final };

struct RunOutcomeFinal {
    std::string text;
    bool is_error = false;
};

// ============================================================================
// 错误分类 (对齐 pi-embedded-helpers.ts 20+ 种错误模式)
// ============================================================================
enum class ErrorClass {
    Unknown,
    Billing,
    ContextOverflow,
    CompactionFailure,
    RoleOrdering,
    SessionCorruption,
    TransientHttp,
    RateLimit,
    Overloaded,
    LiveModelSwitch,
    GatewayDraining,
    CommandLaneCleared,
    MissingApiKey,
    OAuthRefreshFailure,
    ToolResultTurnMismatch,
    BunFetchSocket,
};

// 对齐 pi-embedded-helpers.ts: isBillingErrorMessage
bool is_billing_error_message(const std::string& message);

// 对齐 pi-embedded-helpers.ts: isLikelyContextOverflowError
bool is_likely_context_overflow_error(const std::string& message);

// 对齐 pi-embedded-helpers.ts: isCompactionFailureError
bool is_compaction_failure_error(const std::string& message);

// 对齐 pi-embedded-helpers.ts: isRateLimitErrorMessage
bool is_rate_limit_error_message(const std::string& message);

// 对齐 pi-embedded-helpers.ts: isOverloadedErrorMessage
bool is_overloaded_error_message(const std::string& message);

// 对齐 pi-embedded-helpers.ts: isTransientHttpError
bool is_transient_http_error(const std::string& message);

// 对齐 agent-runner-execution.ts:313-321: isToolResultTurnMismatchError
bool is_tool_result_turn_mismatch_error(const std::string& message);

// 对齐 agent-runner-execution.ts:300-311: isRoleOrderingError
bool is_role_ordering_error(const std::string& message);

// 对齐 agent-runner-execution.ts:300: isSessionCorruption
bool is_session_corruption_error(const std::string& message);

// 综合错误分类
ErrorClass classify_error(const std::string& message);

// ============================================================================
// 错误消息构建 (对齐 agent-runner-execution.ts:281-370)
// ============================================================================

// 对齐: buildRateLimitCooldownMessage
std::string build_rate_limit_cooldown_message(
    std::optional<int64_t> soonest_cooldown_expiry_ms = std::nullopt
);

// 对齐: buildExternalRunFailureText
std::string build_external_run_failure_text(const std::string& message);

// 对齐: buildMissingApiKeyFailureText
std::optional<std::string> build_missing_api_key_failure_text(const std::string& message);

// 对齐: buildRestartLifecycleReplyText
std::string build_restart_lifecycle_reply_text();

// 对齐: collapseRepeatedFailureDetail
std::string collapse_repeated_failure_detail(const std::string& message);

// ============================================================================
// GPT Chat 简洁性控制 (对齐 agent-runner-execution.ts:372-501)
// ============================================================================

// 对齐: shouldApplyOpenAIGptChatGuard
bool should_apply_openai_gpt_chat_guard(
    const std::string& provider,
    const std::string& model
);

// 对齐: countChatReplySentences
int count_chat_reply_sentences(const std::string& text);

// 对齐: scoreChattyFinalReplyText
int score_chatty_final_reply_text(const std::string& text);

// 对齐: shortenChattyFinalReplyText
std::string shorten_chatty_final_reply_text(
    const std::string& text,
    int max_chars,
    int max_sentences
);

// 对齐: applyOpenAIGptChatReplyGuard — modifies payloads in-place
struct ReplyText {
    std::string text;
    bool is_error = false;
    bool is_reasoning = false;
    bool has_media = false;
    bool has_interactive = false;
    bool has_code_block = false;
};
void apply_openai_gpt_chat_reply_guard(
    const std::string& provider,
    const std::string& model,
    const std::string& command_body,
    bool is_heartbeat,
    std::vector<ReplyText>& payloads
);

// ============================================================================
// Streaming 文本规范化 (对齐 agent-runner-execution.ts:702-747)
// ============================================================================

struct NormalizedText {
    std::optional<std::string> text;
    bool skip = false;
};

// 对齐: normalizeStreamingText
NormalizedText normalize_streaming_text(
    const std::string& text,
    bool is_heartbeat,
    bool silent_expected,
    bool is_error = false
);

// 对齐 pi-embedded-helpers.ts: sanitizeUserFacingText
std::string sanitize_user_facing_text(
    const std::string& text,
    bool error_context = false
);

// 对齐 tokens.ts: isSilentReplyText
bool is_silent_reply_text(const std::string& text);

// 对齐 tokens.ts: isSilentReplyPrefixText
bool is_silent_reply_prefix_text(const std::string& text);

// 对齐 tokens.ts: startsWithSilentToken + stripLeadingSilentToken
std::string strip_leading_silent_token(const std::string& text);

// 对齐 heartbeat.ts: stripHeartbeatToken
struct HeartbeatStripResult {
    std::string text;
    bool did_strip = false;
    bool should_skip = false;
};
HeartbeatStripResult strip_heartbeat_token(const std::string& text);

// ============================================================================
// FallbackSelectionState (对齐 agent-runner-execution.ts:112-279)
// ============================================================================
struct FallbackSelectionState {
    std::optional<std::string> provider_override;
    std::optional<std::string> model_override;
    std::optional<std::string> model_override_source;
    std::optional<std::string> auth_profile_override;
    std::optional<std::string> auth_profile_override_source;
    std::optional<int> auth_profile_override_compaction_count;
};

FallbackSelectionState build_fallback_selection_state(
    const std::string& provider,
    const std::string& model,
    const std::optional<std::string>& auth_profile_id = std::nullopt,
    const std::optional<std::string>& auth_profile_id_source = std::nullopt
);

// 对齐: isLikelyExecutionAckPrompt (incomplete-turn.ts)
bool is_likely_execution_ack_prompt(const std::string& prompt);

} // namespace react
} // namespace hunterclaw
