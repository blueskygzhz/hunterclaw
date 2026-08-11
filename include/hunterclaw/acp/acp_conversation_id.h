#pragma once

// ============================================================================
// acp_conversation_id.h
// 🎯 严格对齐 OpenClaw: src/acp/conversation-id.ts
//
// 职责:
//   - 会话 ID 规范化（去除前缀、校验格式）
//   - 从 metadata 中提取 conversationId
//   - 确定性 ID 生成（SHA-256 hash，对齐 OC）
// ============================================================================

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace acp {

// ── Conversation ID ───────────────────────────────────────────────────────────

// 🎯 对齐 conversation-id.ts: normalizeConversationId(id)
// 去平台前缀（cursor: / windsurf: / codex: / acp:），
// 返回纯 session ID 部分。
// 输入无效或空字符串返回 nullopt。
std::optional<std::string> normalize_conversation_id(const std::string& id);

// 🎯 对齐 conversation-id.ts: conversationIdFromMetadata(meta)
// 从 ACP session metadata JSON 中提取 conversationId 字段，
// 并按 ACP 前缀规则规范化。
std::optional<std::string> conversation_id_from_metadata(
    const nlohmann::json& meta);

// 🎯 对齐 conversation-id.ts: generateDeterministicConversationId(sessionKey)
// 基于 session_key SHA-256 生成确定性 conversation ID（"acp:sha256hex16"）
// 用于跨客户端恢复同一会话。
std::string generate_deterministic_conversation_id(
    const std::string& session_key);

} // namespace acp
} // namespace hunterclaw
