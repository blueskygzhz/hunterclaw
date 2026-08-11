#pragma once

// ============================================================================
// acp_tool_status.h
// 🎯 严格对齐 OpenClaw: src/acp/tool-status.ts
//
// 职责:
//   - 工具调用结果分类（completed / failed / cancelled）
//   - 工具权限级别判定（read/write/execute）
//   - 工具结果质量评估（success/failure/cancelled outcome）
// ============================================================================

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace acp {

// ── Tool Result Outcome ───────────────────────────────────────────────────────

// 🎯 对齐 tool-status.ts: ToolResultOutcome = { kind, status, error? }
enum class ToolResultKind {
    Completed,
    Failed,
    Cancelled,
};

struct ToolResultOutcome {
    ToolResultKind kind = ToolResultKind::Completed;
    std::optional<std::string> error_code;
    std::optional<std::string> error_message;
};

// ── Tool Permission Kind ──────────────────────────────────────────────────────

// 🎯 对齐 tool-status.ts: ToolPermissionKind
enum class ToolPermissionKind {
    Read,
    Write,
    Execute,
    Unknown,
};

// ── Public API ────────────────────────────────────────────────────────────────

// 🎯 对齐 tool-status.ts: classifyToolResult(content)
// 解析工具返回内容判断是否成功/失败/取消:
//   - 检测 exitCode != 0 → Failed
//   - 检测 kill/cancel/abort → Cancelled
//   - 检测 error/message 字段 → Failed
//   - 默认 → Completed
ToolResultOutcome classify_tool_result(const nlohmann::json& content);

// 🎯 对齐 tool-status.ts: resolveToolKindForPermission(toolName, toolInput)
// 根据工具名和参数判断权限级别:
//   - read/search/grep 等 → Read
//   - write/edit/delete 等 → Write
//   - exec/spawn/bash 等 → Execute
//   - 无法判断 → Unknown
ToolPermissionKind resolve_tool_kind_for_permission(
    const std::string& tool_name,
    const nlohmann::json& tool_input = {});

// 🎯 对齐 tool-status.ts: classifyOutcome(status, kind)
// 基于 ToolResultOutcome 和 ToolPermissionKind 判定最终结果:
//   - Failed + Write → 写入失败
//   - Cancelled + Execute → 执行取消
//   - 统一返回结构化判定
struct ToolOutcomeVerdict {
    ToolResultKind kind;
    bool is_success = false;
    bool is_failure = false;
    bool is_cancelled = false;
};
ToolOutcomeVerdict classify_outcome(
    const ToolResultOutcome& outcome,
    ToolPermissionKind perm_kind = ToolPermissionKind::Unknown);

} // namespace acp
} // namespace hunterclaw
