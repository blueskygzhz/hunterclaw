#pragma once
// ============================================================================
// Support Tokens — 严格对齐 OpenClaw sessions-send-tokens.ts + auto-reply/tokens.ts
// ============================================================================
// 用于 A2A flow (ping-pong + announce) 的控制 token:
//   - REPLY_SKIP:    回复**恰好等于**此 token → 停止 ping-pong
//   - ANNOUNCE_SKIP: 回复**恰好等于**此 token → 跳过 announce/投递
//   - NO_REPLY:      静默回复标记（openclaw SILENT_REPLY_TOKEN）
//   - HEARTBEAT_OK:  心跳确认（openclaw HEARTBEAT_TOKEN）
//
// ⚠ 关键语义（対齐 tokens.ts:52-67 isSilentReplyText）：
//   判定是 **token-only 精确匹配**（允许首尾空白、重复 token、首尾标点），
//   **不是**子串包含。openclaw#19537 专门修过这个问题：
//   若用子串包含，一条以 "...见 NO_REPLY 说明" 结尾的实质回复会被误判为静默
//   而整条丢弃。故本文件所有判定函数都必须是精确匹配语义。
// ============================================================================

#include <string>

namespace hunterclaw {
namespace tools {
namespace support_tokens {

// ── Token 常量 ──────────────────────────────────────────────────────────────
// 対齐 sessions-send-tokens.ts:9,11 与 auto-reply/tokens.ts:5,7。
// 注意这些是**裸值**，不带 "__" 包裹 —— prompt 里也是原样出现
// （见 helpers.ts:127,150 `reply exactly "REPLY_SKIP"`），
// 且 HunterClaw 其它子系统（agent_instance.cc、boot.cc、chat.cc）
// 用的同样是裸 "NO_REPLY" / "HEARTBEAT_OK"。

/// 対齐 sessions-send-tokens.ts:9 ANNOUNCE_SKIP_TOKEN = "ANNOUNCE_SKIP"
extern const std::string ANNOUNCE_SKIP_TOKEN;

/// 対齐 sessions-send-tokens.ts:11 REPLY_SKIP_TOKEN = "REPLY_SKIP"
extern const std::string REPLY_SKIP_TOKEN;

/// 対齐 auto-reply/tokens.ts:7 SILENT_REPLY_TOKEN = "NO_REPLY"
extern const std::string SILENT_REPLY_TOKEN;

/// 対齐 auto-reply/tokens.ts:5 HEARTBEAT_TOKEN = "HEARTBEAT_OK"
extern const std::string HEARTBEAT_TOKEN;

// ── 判定函数 ────────────────────────────────────────────────────────────────

/// 対齐 tokens.ts:52-67 isSilentReplyText(text, token)
///
/// 仅在 text **整体就是**该 token 时返回 true。具体规则：
///   * 忽略首尾空白
///   * 允许同一 token 以空白分隔重复出现（模型偶尔会重复吐）
///   * 大小写不敏感
///   * 允许首尾包裹标点（模型偶尔写成 `**NO_REPLY**` 或 `NO_REPLY.`）
/// 反例（均返回 false，必须投递）：
///   "好的，稍后回复 NO_REPLY 之类的标记会被忽略"
///   "REPLY_SKIP 的含义是停止对话"
bool is_silent_reply_text(const std::string& text, const std::string& token);

/// 対齐 sessions-send-tokens.ts:21-23 isAnnounceSkip(text)
/// 注意真源用的是 `text.trim() === ANNOUNCE_SKIP_TOKEN`（**区分大小写**，
/// 且不剥标点），比 isSilentReplyText 更严格，故单独实现。
bool is_announce_skip(const std::string& reply);

/// 対齐 sessions-send-tokens.ts:26-28 isReplySkip(text)
/// 同上：`text.trim() === REPLY_SKIP_TOKEN`，区分大小写。
bool is_reply_skip(const std::string& reply);

/// 対齐 tokens.ts:52 isSilentReplyText(text, SILENT_REPLY_TOKEN)
bool is_silent_reply(const std::string& reply);

/// 対齐 tokens.ts:52 isSilentReplyText(text, HEARTBEAT_TOKEN)
bool is_heartbeat(const std::string& reply);

/// 対齐 sessions-send-tokens.ts:31-33 isNonDeliverableSessionsReply(text)
///   NON_DELIVERABLE_REPLY_TOKENS.some(token => isSilentReplyText(text, token))
/// 即四个 token 各自做 **token-only** 判定，任一命中则不投递。
bool is_non_deliverable_sessions_reply(const std::string& reply);

/// 対齐 tokens.ts:236-238 stripSilentToken(text, token)
///
/// 只剥离**结尾**的 token（前面须有空白或 `*` 作为边界），保留其余正文。
/// 不做全局子串删除 —— 那会把正文里提到的 token 名字也抹掉，
/// 例如 "回复 NO_REPLY 即可静默" 会变成 "回复  即可静默"。
std::string strip_trailing_silent_token(const std::string& text,
                                        const std::string& token);

/// 投递前清洗：依次剥离四个 token 的**结尾**出现，并 trim。
/// 対齐 openclaw 的做法 —— 真源在投递路径上并不做无边界的全局删除。
std::string strip_support_tokens(const std::string& reply);

/// 対齐 sessions-send-tokens.ts:36-49 selectDeliverableSessionsReply
///
/// 在 primary / fallback 两个候选里挑可投递的那个。语义**刻意不对称**，
/// 别简化成"primary 不行就用 fallback"：
///   1. primary trim 后非空且非任何 non-deliverable token → 用 primary
///   2. primary 非空但**不是 NO_REPLY**（即 ANNOUNCE_SKIP / REPLY_SKIP /
///      HEARTBEAT_OK）→ 返回空，**禁止**回落 fallback。
///      这三个 token 是模型的明确"别发"指令，回落等于违抗指令。
///   3. 仅当 primary 为空**或**恰是 NO_REPLY 时才考虑 fallback。
///      NO_REPLY 意为"我没话说"，此时用捕获到的兜底输出是合理的。
///   4. fallback 自身也要过 non-deliverable 检查。
/// @return 可投递文本；无可投递内容时返回空串
std::string select_deliverable_sessions_reply(const std::string& primary,
                                             const std::string& fallback);

}  // namespace support_tokens
}  // namespace tools
}  // namespace hunterclaw
