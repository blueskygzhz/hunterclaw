#pragma once

// ============================================================================
// acp_tool_status.h
// 🎯 严格对齐 OpenClaw: src/acp/tool-status.ts
//
// ⚠ 重要历史说明（避免重复踩坑）：
//   本文件此前包含 classify_tool_result / resolve_tool_kind_for_permission /
//   classify_outcome 三个函数，注释标称"严格对齐 tool-status.ts"，但真源
//   **完全没有**这三个函数 —— 它们是凭空发明的，且零外部引用（死代码）。
//   其中 classify_tool_result 用子串匹配 "error"/"timeout" 判定工具失败，
//   任何输出里含这些词的成功结果都会被误判为失败。
//
//   真源 tool-status.ts 只导出**一个**函数 resolveAcpToolTerminalOutcome，
//   有 2 处真实调用方（agents/command/attempt-execution.ts:1446、
//   auto-reply/reply/acp-projector.ts:353,371），而 HunterClaw 此前没有它。
//   现按真源重写。
// ============================================================================

#include <optional>
#include <string>

namespace hunterclaw {
namespace acp {

/// 対齐 tool-status.ts:11-12 AcpToolTerminalOutcome
/// 注意只有三个取值 —— 真源的映射表把 done→completed、error→failed 做了归一。
enum class AcpToolTerminalOutcome {
    Completed,
    Failed,
    Cancelled,
};

/// 対齐 tool-status.ts:14-20 resolveAcpToolTerminalOutcome(status)
///
/// 把 ACP tool_call 的 status 字段映射成终态；**非终态或未知值返回 nullopt**。
/// 调用方据"是否有返回值"判断该 tool_call 是否已结束
/// （見 acp-projector.ts:353 `resolveAcpToolTerminalOutcome(status) !== undefined`）。
///
/// 映射表（対齐:3-9，逐条一致，不可增减）：
///   completed → Completed
///   done      → Completed
///   failed    → Failed
///   error     → Failed
///   cancelled → Cancelled
///
/// ⚠ 注意 "canceled"（单 l 美式拼写）**不在**映射表内 —— 真源只认 "cancelled"。
/// ⚠ 判定是 lowercase 后的**严格相等**（対齐:15 normalizeOptionalLowercaseString），
///   不是子串包含。
std::optional<AcpToolTerminalOutcome> resolve_acp_tool_terminal_outcome(
    const std::string& status);

/// 便捷谓词：该 status 是否代表 tool_call 已到终态。
/// 対齐 acp-projector.ts:353,371 的 `!== undefined` 用法。
inline bool is_acp_tool_terminal_status(const std::string& status) {
    return resolve_acp_tool_terminal_outcome(status).has_value();
}

/// 対齐 AcpToolTerminalOutcome 的字面量（用于日志/事件投影）
const char* to_string(AcpToolTerminalOutcome outcome);

} // namespace acp
} // namespace hunterclaw
