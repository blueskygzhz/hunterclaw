// ============================================================================
// acp_tool_status.cc
// 🎯 严格对齐 OpenClaw: src/acp/tool-status.ts
// ============================================================================

#include "hunterclaw/acp/acp_tool_status.h"
#include <algorithm>
#include <unordered_set>

namespace hunterclaw {
namespace acp {

// ── classify_tool_result ─────────────────────────────────────────────────────
// 🎯 对齐 tool-status.ts: classifyToolResult(content)
// 解析工具返回内容判断成功/失败/取消
ToolResultOutcome classify_tool_result(const nlohmann::json& content) {
    ToolResultOutcome result;

    if (content.is_null()) {
        result.kind = ToolResultKind::Completed;
        return result;
    }

    // 处理字符串内容
    if (content.is_string()) {
        std::string s = content.get<std::string>();
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        // 检测取消标记
        if (lower.find("killed") != std::string::npos ||
            lower.find("cancelled") != std::string::npos ||
            lower.find("canceled") != std::string::npos ||
            lower.find("timed out") != std::string::npos ||
            lower.find("timeout") != std::string::npos) {
            result.kind = ToolResultKind::Cancelled;
            return result;
        }

        // 检测错误标记
        if (lower.find("error") != std::string::npos ||
            lower.find("failed") != std::string::npos ||
            lower.find("exception") != std::string::npos) {
            result.kind = ToolResultKind::Failed;
            result.error_message = s.substr(0, 200);  // truncated
            return result;
        }

        result.kind = ToolResultKind::Completed;
        return result;
    }

    // 处理 JSON 对象内容
    if (content.is_object()) {
        // exitCode != 0 → Failed
        if (content.contains("exitCode") && content["exitCode"].is_number()) {
            int code = content["exitCode"].get<int>();
            if (code != 0) {
                result.kind = ToolResultKind::Failed;
                result.error_code = std::to_string(code);
                if (content.contains("stderr") && content["stderr"].is_string()) {
                    result.error_message = content["stderr"].get<std::string>();
                }
                return result;
            }
        }

        // killed / cancelled 标记
        if (content.contains("killed") && content["killed"].is_boolean() &&
            content["killed"].get<bool>()) {
            result.kind = ToolResultKind::Cancelled;
            return result;
        }

        if (content.contains("cancelled") && content["cancelled"].is_boolean() &&
            content["cancelled"].get<bool>()) {
            result.kind = ToolResultKind::Cancelled;
            return result;
        }

        // error 字段
        if (content.contains("error") && content["error"].is_string()) {
            result.kind = ToolResultKind::Failed;
            result.error_message = content["error"].get<std::string>();
            return result;
        }

        // is_error 标记
        if (content.contains("is_error") && content["is_error"].is_boolean() &&
            content["is_error"].get<bool>()) {
            result.kind = ToolResultKind::Failed;
            return result;
        }

        // success=false
        if (content.contains("success") && content["success"].is_boolean() &&
            !content["success"].get<bool>()) {
            result.kind = ToolResultKind::Failed;
            return result;
        }
    }

    result.kind = ToolResultKind::Completed;
    return result;
}

// ── resolve_tool_kind_for_permission ─────────────────────────────────────────
// 🎯 对齐 tool-status.ts: resolveToolKindForPermission(toolName, toolInput)
ToolPermissionKind resolve_tool_kind_for_permission(
    const std::string& tool_name,
    const nlohmann::json& /*tool_input*/) {

    if (tool_name.empty()) return ToolPermissionKind::Unknown;

    std::string lower = tool_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Exec-capable tools
    static const std::unordered_set<std::string> exec_tools = {
        "exec", "spawn", "shell", "bash", "process",
        "code_execution", "run", "terminal"
    };
    if (exec_tools.count(lower)) return ToolPermissionKind::Execute;

    // Read-only tools
    static const std::unordered_set<std::string> read_tools = {
        "read", "read_file", "search", "grep", "find",
        "list_dir", "list_files", "cat", "head", "tail",
        "web_search", "web_fetch", "memory_search",
        "session_status", "sessions_history", "sessions_list",
        "sessions_search", "agents_list", "agents_wait",
        "subagents"
    };
    if (read_tools.count(lower)) return ToolPermissionKind::Read;

    // Write/mutating tools
    static const std::unordered_set<std::string> write_tools = {
        "write", "write_file", "edit", "replace", "insert",
        "delete", "remove", "create", "move", "rename",
        "patch", "append",
        "sessions_patch", "sessions_reset", "sessions_delete",
        "sessions_spawn", "sessions_send", "sessions_yield",
        "set_session_mode", "set_session_config"
    };
    if (write_tools.count(lower)) return ToolPermissionKind::Write;

    return ToolPermissionKind::Unknown;
}

// ── classify_outcome ────────────────────────────────────────────────────────
// 🎯 对齐 tool-status.ts: classifyOutcome(status, kind)
ToolOutcomeVerdict classify_outcome(
    const ToolResultOutcome& outcome,
    ToolPermissionKind /*perm_kind*/) {

    ToolOutcomeVerdict v;
    v.kind = outcome.kind;

    switch (outcome.kind) {
        case ToolResultKind::Completed:
            v.is_success = true;
            break;
        case ToolResultKind::Failed:
            v.is_failure = true;
            break;
        case ToolResultKind::Cancelled:
            v.is_cancelled = true;
            break;
    }

    return v;
}

} // namespace acp
} // namespace hunterclaw
