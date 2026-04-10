#pragma once
// ============================================================================
// normalize_reply.h — 严格对齐 OpenClaw normalize-reply.ts (129行)
//
// 回复内容终端规范化管道: empty check → silent token strip → heartbeat strip
// → sanitizeUserFacingText → channel transform → response prefix 拼接
// ============================================================================

#include "hunterclaw/react/reply_payloads.h"
#include "hunterclaw/react/response_prefix_template.h"
#include <string>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace react {

// 对齐 normalize-reply.ts:20
enum class NormalizeReplySkipReason { Empty, Silent, Heartbeat };

// 对齐 normalize-reply.ts:22-32
struct NormalizeReplyOptions {
    std::optional<std::string> response_prefix;
    bool apply_channel_transforms = true;
    std::optional<ResponsePrefixContext> response_prefix_context;
    std::function<void()> on_heartbeat_strip;
    bool strip_heartbeat = true;
    std::string silent_token = "NO_REPLY";
    std::function<ReplyPayload(const ReplyPayload&)> transform_reply_payload;
    std::function<void(NormalizeReplySkipReason)> on_skip;
};

// 对齐 normalize-reply.ts:34-128: normalizeReplyPayload
// Returns nullopt if payload should be skipped
std::optional<ReplyPayload> normalize_reply_payload(
    const ReplyPayload& payload,
    const NormalizeReplyOptions& opts = {}
);

} // namespace react
} // namespace hunterclaw
