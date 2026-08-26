#pragma once

// ============================================================================
// acp_conversation_id.h
// 🎯 严格对齐 OpenClaw: src/acp/conversation-id.ts
//
// ⚠ 重要历史说明（避免重复踩坑）：
//   本文件此前包含 normalize_conversation_id / conversation_id_from_metadata /
//   generate_deterministic_conversation_id 三个函数，注释标称"严格对齐
//   conversation-id.ts"，但真源该文件**只导出一个** normalizeConversationText，
//   那三个函数在整个 openclaw 仓库里都不存在（grep 结果 0 处），属凭空发明，
//   且零外部引用（死代码）。
//
//   其中 generate_deterministic_conversation_id 尤其危险：它自造了
//   SHA-256 加盐规则（salt="openclaw-acp-conversation:"，取前 8 字节 hex，
//   前缀 "acp:"）并声称对齐真源。真源没有任何这类 ID 生成逻辑 ——
//   若曾用它生成过持久化 ID，与上游完全不兼容，且依赖 OpenSSL 属多余依赖。
//
//   真源 normalizeConversationText 有 3 个文件、共 7 处真实调用
//   （commands-acp/context.ts、conversation-binding-input.ts、
//     infra/outbound/current-conversation-bindings.ts），
//   而 HunterClaw 此前没有它。现按真源重写。
// ============================================================================

#include <string>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace acp {

/// 対齐 conversation-id.ts:2-10 normalizeConversationText(value: unknown)
///
/// 把宽松类型的元数据值收敛成 trim 后的字符串：
///   string           → trim 后返回
///   number / bool    → 转字符串再 trim（対齐 `${value}`）
///   其它（null/对象/数组/缺失）→ 空串
///
/// ⚠ 只做 trim，**不做 lowercase**。真源的小写化由调用方另外套一层
/// normalizeLowercaseStringOrEmpty 完成（見 context.ts:15、
/// conversation-binding-input.ts:30），两者职责分离。
/// 在此处顺手小写会让 accountId / threadId 这类大小写敏感的外部 ID 被破坏
/// （見 conversation-binding-input.ts:42,51 —— 它们**不**再套小写）。
std::string normalize_conversation_text(const nlohmann::json& value);

/// 字符串重载（等价于对 string 输入的 trim）
std::string normalize_conversation_text(const std::string& value);

} // namespace acp
} // namespace hunterclaw
