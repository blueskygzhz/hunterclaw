#pragma once
// ============================================================================
// reply_delivery.h — 严格对齐 OpenClaw reply-delivery.ts (141行)
//                    + reply-directives.ts (49行) + reply-tags.ts (22行)
//                    + reply-reference.ts (64行)
// ============================================================================

#include "hunterclaw/react/reply_payloads.h"
#include "hunterclaw/react/streaming_directives.h"
#include "hunterclaw/react/execution_guard.h"
#include <string>
#include <set>
#include <optional>
#include <functional>
#include <memory>

namespace hunterclaw {
namespace react {

// ============================================================================
// 对齐 reply-delivery.ts:11: ReplyDirectiveParseMode
// ============================================================================
enum class ReplyDirectiveParseMode { Always, Auto, Never };

// ============================================================================
// 对齐 reply-delivery.ts:13-59: normalizeReplyPayloadDirectives
// ============================================================================
struct NormalizedDirectiveResult {
    ReplyPayload payload;
    bool is_silent = false;
};

NormalizedDirectiveResult normalize_reply_payload_directives(
    const ReplyPayload& payload,
    const std::optional<std::string>& current_message_id = std::nullopt,
    const std::string& silent_token = "NO_REPLY",
    bool trim_leading_whitespace = false,
    ReplyDirectiveParseMode parse_mode = ReplyDirectiveParseMode::Always
);

// ============================================================================
// 对齐 reply-payloads-base.ts:71-76: applyReplyTagsToPayload
// ============================================================================
ReplyPayload apply_reply_tags_to_payload(
    const ReplyPayload& payload,
    const std::optional<std::string>& current_message_id = std::nullopt
);

// ============================================================================
// 对齐 reply-delivery.ts:61-140: createBlockReplyDeliveryHandler
// ============================================================================
struct BlockReplyDeliveryParams {
    std::function<void(const ReplyPayload&)> on_block_reply;
    std::optional<std::string> current_message_id;
    std::function<NormalizedText(const ReplyPayload&)> normalize_streaming_text;
    std::function<ReplyPayload(const ReplyPayload&)> apply_reply_to_mode;
    std::function<ReplyPayload(const ReplyPayload&)> normalize_media_paths;
    // Typing signaler (simplified — just signal_text_delta)
    std::function<void(const std::string& text)> signal_text_delta;
    bool block_streaming_enabled = false;
    // Pipeline interface (simplified)
    std::function<void(const ReplyPayload&)> pipeline_enqueue;
    std::shared_ptr<std::set<std::string>> directly_sent_block_keys;
};

std::function<void(const ReplyPayload&)> create_block_reply_delivery_handler(
    const BlockReplyDeliveryParams& params
);

// ============================================================================
// 对齐 reply-reference.ts (64行): ReplyReferencePlanner
// ============================================================================
struct ReplyReferencePlanner {
    std::function<std::optional<std::string>()> use;
    std::function<void()> mark_sent;
    std::function<bool()> has_replied;
};

ReplyReferencePlanner create_reply_reference_planner(
    ReplyToMode reply_to_mode,
    const std::optional<std::string>& existing_id = std::nullopt,
    const std::optional<std::string>& start_id = std::nullopt,
    bool allow_reference = true,
    bool initial_has_replied = false
);

// ============================================================================
// 对齐 reply-payloads-dedupe.ts:119-161: shouldSuppressMessagingToolReplies
// ============================================================================
struct MessagingToolSend {
    std::string to;
    std::optional<std::string> provider;
    std::optional<std::string> account_id;
    std::optional<std::string> thread_id;
};

bool should_suppress_messaging_tool_replies(
    const std::optional<std::string>& message_provider,
    const std::vector<MessagingToolSend>& sent_targets,
    const std::optional<std::string>& originating_to,
    const std::optional<std::string>& account_id = std::nullopt
);

// ============================================================================
// 对齐 session-run-accounting.ts (42行): 委托函数
// ============================================================================
struct SessionUsageData {
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cache_read_tokens = 0;
    int64_t cache_write_tokens = 0;
    int context_tokens_used = 0;
    std::string model_used;
    std::string provider_used;
};

struct IncrementCompactionParams {
    std::string session_key;
    std::string store_path;
    std::optional<int> amount;
    std::optional<int64_t> last_call_input_tokens;
    std::optional<int64_t> last_call_output_tokens;
    std::optional<int> context_tokens_used;
    std::optional<std::string> new_session_id;
};

// 对齐: deriveSessionTotalTokens
std::optional<int64_t> derive_session_total_tokens(
    int64_t input_tokens,
    int64_t output_tokens,
    std::optional<int> context_tokens = std::nullopt
);

} // namespace react
} // namespace hunterclaw
