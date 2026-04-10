#pragma once
// ============================================================================
// reply_threading.h — 严格对齐 OpenClaw reply-threading.ts (164行)
//                     + origin-routing.ts (27行)
// ============================================================================

#include "hunterclaw/react/reply_payloads.h"
#include <string>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace react {

// ============================================================================
// 对齐 config/types.ts: ReplyToMode (扩展 Batched)
// ============================================================================
enum class ReplyToModeEx {
    Off,
    First,
    All,
    Batched,  // 对齐 OpenClaw: "batched" — implicit current message reply for batched runs
};

// ============================================================================
// 对齐 types.ts: ReplyThreadingPolicy
// ============================================================================
struct ReplyThreadingPolicy {
    // "default" | "allow" | "deny"
    std::string implicit_current_message = "default";
};

// ============================================================================
// 对齐 reply-threading.ts:26-52: resolveConfiguredReplyToMode
// ============================================================================
ReplyToModeEx resolve_configured_reply_to_mode(
    const std::string& channel = "",
    const std::string& chat_type = ""
);

// ============================================================================
// 对齐 reply-threading.ts:71-79: resolveReplyToMode
// ============================================================================
ReplyToModeEx resolve_reply_to_mode(
    const std::string& channel = "",
    const std::string& account_id = "",
    const std::string& chat_type = ""
);

// ============================================================================
// 对齐 reply-threading.ts:81-123: createReplyToModeFilter (有状态)
// ============================================================================
std::function<ReplyPayload(const ReplyPayload&)> create_reply_to_mode_filter(
    ReplyToModeEx mode,
    bool allow_explicit_reply_tags_when_off = true
);

// ============================================================================
// 对齐 reply-threading.ts:125-137: resolveImplicitCurrentMessageReplyAllowance
// ============================================================================
bool resolve_implicit_current_message_reply_allowance(
    ReplyToModeEx mode,
    const std::optional<ReplyThreadingPolicy>& policy = std::nullopt
);

// ============================================================================
// 对齐 reply-threading.ts:139-149: resolveBatchedReplyThreadingPolicy
// ============================================================================
std::optional<ReplyThreadingPolicy> resolve_batched_reply_threading_policy(
    ReplyToModeEx mode,
    bool is_batched
);

// ============================================================================
// 对齐 reply-threading.ts:151-163: createReplyToModeFilterForChannel
// ============================================================================
std::function<ReplyPayload(const ReplyPayload&)> create_reply_to_mode_filter_for_channel(
    ReplyToModeEx mode,
    const std::string& channel = ""
);

// ============================================================================
// 对齐 origin-routing.ts (27行): 3 个 originating 路由解析函数
// ============================================================================
std::optional<std::string> resolve_origin_message_provider(
    const std::optional<std::string>& originating_channel,
    const std::optional<std::string>& provider = std::nullopt
);

std::optional<std::string> resolve_origin_message_to(
    const std::optional<std::string>& originating_to,
    const std::optional<std::string>& to = std::nullopt
);

std::optional<std::string> resolve_origin_account_id(
    const std::optional<std::string>& originating_account_id,
    const std::optional<std::string>& account_id = std::nullopt
);

} // namespace react
} // namespace hunterclaw
