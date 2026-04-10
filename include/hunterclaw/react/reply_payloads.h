#pragma once
// ============================================================================
// reply_payloads.h — 严格对齐 OpenClaw agent-runner-payloads.ts (235行)
//
// Reply payload 后处理: heartbeat 过滤、messaging tool 去重、
// block streaming 去重、reply threading、media 规范化
// ============================================================================

#include <string>
#include <vector>
#include <set>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace react {

// ============================================================================
// ReplyPayload (对齐 OpenClaw types.ts:ReplyPayload 核心字段)
// ============================================================================
struct ReplyPayload {
    std::string text;
    bool is_error = false;
    bool is_reasoning = false;
    bool is_compaction_notice = false;
    std::optional<std::string> media_url;
    std::vector<std::string> media_urls;
    bool audio_as_voice = false;
    std::optional<std::string> reply_to_id;
    bool reply_to_current = false;
    bool interactive = false;
};

// ============================================================================
// ReplyToMode (对齐 OpenClaw config/types.ts:ReplyToMode)
// ============================================================================
enum class ReplyToMode {
    Off,     // No reply threading
    First,   // Thread only first payload
    All,     // Thread all payloads
};

// ============================================================================
// 对齐 agent-runner-payloads.ts:90-234: buildReplyPayloads
// ============================================================================

struct BuildReplyPayloadsParams {
    std::vector<ReplyPayload> payloads;
    bool is_heartbeat = false;
    bool did_log_heartbeat_strip = false;
    bool silent_expected = false;
    bool block_streaming_enabled = false;
    bool block_pipeline_did_stream = false;
    bool block_pipeline_is_aborted = false;

    // Messaging tool dedup
    std::vector<std::string> messaging_tool_sent_texts;
    std::vector<std::string> messaging_tool_sent_media_urls;

    // Reply threading
    ReplyToMode reply_to_mode = ReplyToMode::Off;
    std::optional<std::string> current_message_id;

    // Block streaming dedup
    std::set<std::string> directly_sent_block_keys;
    std::function<bool(const ReplyPayload&)> has_sent_payload;

    // Media normalization
    std::function<ReplyPayload(const ReplyPayload&)> normalize_media_paths;
};

struct BuildReplyPayloadsResult {
    std::vector<ReplyPayload> reply_payloads;
    bool did_log_heartbeat_strip = false;
};

BuildReplyPayloadsResult build_reply_payloads(const BuildReplyPayloadsParams& params);

// ============================================================================
// 辅助函数 (对齐 reply-payloads-base.ts)
// ============================================================================

// 对齐: isRenderablePayload
bool is_renderable_payload(const ReplyPayload& payload);

// 对齐: applyReplyThreading
std::vector<ReplyPayload> apply_reply_threading(
    const std::vector<ReplyPayload>& payloads,
    ReplyToMode mode,
    const std::optional<std::string>& current_message_id
);

// 对齐: createBlockReplyContentKey
std::string create_block_reply_content_key(const ReplyPayload& payload);

// 对齐: filterMessagingToolDuplicates
std::vector<ReplyPayload> filter_messaging_tool_duplicates(
    const std::vector<ReplyPayload>& payloads,
    const std::vector<std::string>& sent_texts
);

// 对齐: filterMessagingToolMediaDuplicates
std::vector<ReplyPayload> filter_messaging_tool_media_duplicates(
    const std::vector<ReplyPayload>& payloads,
    const std::vector<std::string>& sent_media_urls
);

} // namespace react
} // namespace hunterclaw
