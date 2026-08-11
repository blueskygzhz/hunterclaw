#pragma once
// ============================================================================
// Support Tokens — 严格对齐 OpenClaw sessions-send-tokens.ts
// ============================================================================
// 用于 A2A flow (ping-pong + announce) 的控制 token:
//   - REPLY_SKIP:    目标 agent 回复中包含此 token → 跳过 ping-pong
//   - ANNOUNCE_SKIP: 目标 agent 回复中包含此 token → 跳过 announce/投递
//   - SILENT_REPLY:  回复中包含此 token → 不将回复转发给原始 channel
//   - HEARTBEAT:     用于保持会话连接
// ============================================================================

#include <string>

namespace hunterclaw {
namespace tools {
namespace support_tokens {

// ── Token 常量 ──────────────────────────────────────────────────────────────

/// 对齐 OpenClaw: ANNOUNCE_SKIP_TOKEN = "__ANNOUNCE_SKIP__"
extern const std::string ANNOUNCE_SKIP_TOKEN;

/// 对齐 OpenClaw: REPLY_SKIP_TOKEN = "__REPLY_SKIP__"
extern const std::string REPLY_SKIP_TOKEN;

/// 对齐 OpenClaw: SILENT_REPLY_TOKEN = "__SILENT_REPLY__"
extern const std::string SILENT_REPLY_TOKEN;

/// 对齐 OpenClaw: HEARTBEAT_TOKEN = "__HEARTBEAT__"
extern const std::string HEARTBEAT_TOKEN;

// ── 判定函数 ────────────────────────────────────────────────────────────────

/// 对齐 OpenClaw: isAnnounceSkip(reply: string) → boolean
/// 检测回复中是否包含 ANNOUNCE_SKIP_TOKEN → 跳过向源 channel 投递
bool is_announce_skip(const std::string& reply);

/// 对齐 OpenClaw: isReplySkip(reply: string) → boolean
/// 检测回复中是否包含 REPLY_SKIP_TOKEN → 跳过 ping-pong 后续轮次
bool is_reply_skip(const std::string& reply);

/// 对齐 OpenClaw: isSilentReply(reply: string) → boolean
/// 检测回复中是否包含 SILENT_REPLY_TOKEN → 不转发给原始 channel
bool is_silent_reply(const std::string& reply);

/// 对齐 OpenClaw: isHeartbeat(reply: string) → boolean
/// 检测回复中是否包含 HEARTBEAT_TOKEN → 心跳消息
bool is_heartbeat(const std::string& reply);

/// 对齐 OpenClaw: isNonDeliverableSessionsReply(reply: string) → boolean
/// 如果回复中包含以上任一种控制 token，则不应投递
bool is_non_deliverable_sessions_reply(const std::string& reply);

/// 去除所有控制 token（用于投递前清洗）
std::string strip_support_tokens(const std::string& reply);

}  // namespace support_tokens
}  // namespace tools
}  // namespace hunterclaw
