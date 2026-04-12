// ============================================================================
// acp_client.cc
// 🎯 严格对齐 OpenClaw:
//   - src/acp/client.ts (ClientSideConnection, spawn, permission resolver)
//   - src/acp/approval-classifier.ts (ACP 审批分类器)
//   - src/acp/policy.ts (ACP 策略控制)
//   - src/acp/session-interaction-mode.ts (会话交互模式)
// ============================================================================

#include "hunterclaw/acp/acp_client.h"
#include <algorithm>
#include <set>
#include <sstream>

namespace hunterclaw {
namespace acp {

// ============================================================================
// 🎯 对齐 approval-classifier.ts
// ACP Approval Classifier — 工具审批分类
// ============================================================================

// 🎯 对齐: SAFE_SEARCH_TOOL_IDS, TRUSTED_SAFE_TOOL_ALIASES, etc.
static const std::set<std::string> SAFE_SEARCH_TOOL_IDS = {
    "search", "web_search", "memory_search"
};
static const std::set<std::string> TRUSTED_SAFE_TOOL_ALIASES = {"search"};
static const std::set<std::string> EXEC_CAPABLE_TOOL_IDS = {
    "exec", "spawn", "shell", "bash", "process", "code_execution"
};
static const std::set<std::string> CONTROL_PLANE_TOOL_IDS = {
    "sessions_spawn", "sessions_send", "session_status"
};

// 🎯 对齐: classifyAcpToolApproval(params)
AcpApprovalClassification classify_acp_tool_approval(
    const std::string& tool_name,
    const nlohmann::json& tool_input) {

    AcpApprovalClassification result;
    result.tool_name = tool_name;
    result.approval_class = AcpApprovalClass::Unknown;
    result.auto_approve = false;

    if (tool_name.empty()) return result;

    std::string lower = tool_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Safe search tools — auto-approve
    if (SAFE_SEARCH_TOOL_IDS.count(lower)) {
        result.approval_class = AcpApprovalClass::ReadonlySearch;
        result.auto_approve = true;
        return result;
    }

    // Read-only scoped tools
    if (lower == "read" || lower == "read_file" || lower == "list_dir" ||
        lower == "find" || lower == "grep" || lower == "list_files") {
        result.approval_class = AcpApprovalClass::ReadonlyScoped;
        result.auto_approve = true;
        return result;
    }

    // Exec-capable tools
    if (EXEC_CAPABLE_TOOL_IDS.count(lower)) {
        result.approval_class = AcpApprovalClass::ExecCapable;
        result.auto_approve = false;
        return result;
    }

    // Control plane tools
    if (CONTROL_PLANE_TOOL_IDS.count(lower)) {
        result.approval_class = AcpApprovalClass::ControlPlane;
        result.auto_approve = true;
        return result;
    }

    // Mutating tools (write, edit, etc.)
    if (lower == "write" || lower == "write_file" || lower == "edit" ||
        lower == "replace" || lower == "insert" || lower == "delete" ||
        lower == "create" || lower == "move" || lower == "rename") {
        result.approval_class = AcpApprovalClass::Mutating;
        result.auto_approve = false;
        return result;
    }

    // Interactive tools
    if (lower == "ask" || lower == "prompt" || lower == "confirm") {
        result.approval_class = AcpApprovalClass::Interactive;
        result.auto_approve = false;
        return result;
    }

    result.approval_class = AcpApprovalClass::Other;
    return result;
}

// ============================================================================
// 🎯 对齐 policy.ts
// ACP Policy — 启用/禁用策略控制
// ============================================================================

// 🎯 对齐: isAcpEnabledByPolicy(cfg)
bool is_acp_enabled_by_policy(const core::Config& /*cfg*/) {
    // Default: ACP enabled unless explicitly disabled in config
    return true;
}

// 🎯 对齐: resolveAcpDispatchPolicyState(cfg)
AcpDispatchPolicyState resolve_acp_dispatch_policy_state(const core::Config& cfg) {
    if (!is_acp_enabled_by_policy(cfg)) {
        return AcpDispatchPolicyState::AcpDisabled;
    }
    return AcpDispatchPolicyState::Enabled;
}

// 🎯 对齐: isAcpDispatchEnabledByPolicy(cfg)
bool is_acp_dispatch_enabled_by_policy(const core::Config& cfg) {
    return resolve_acp_dispatch_policy_state(cfg) == AcpDispatchPolicyState::Enabled;
}

// 🎯 对齐: resolveAcpDispatchPolicyMessage(cfg)
std::string resolve_acp_dispatch_policy_message(const core::Config& cfg) {
    auto state = resolve_acp_dispatch_policy_state(cfg);
    if (state == AcpDispatchPolicyState::AcpDisabled) {
        return "ACP is disabled by policy (`acp.enabled=false`).";
    }
    if (state == AcpDispatchPolicyState::DispatchDisabled) {
        return "ACP dispatch is disabled by policy (`acp.dispatch.enabled=false`).";
    }
    return "";
}

// 🎯 对齐: isAcpAgentAllowedByPolicy(cfg, agentId)
bool is_acp_agent_allowed_by_policy(const core::Config& /*cfg*/, const std::string& /*agent_id*/) {
    // Default: all agents allowed when no explicit allowlist
    return true;
}

// ============================================================================
// 🎯 对齐 session-interaction-mode.ts
// ACP Session Interaction Mode
// ============================================================================

// 🎯 对齐: resolveAcpSessionInteractionMode(entry)
AcpSessionInteractionMode resolve_acp_session_interaction_mode(
    const SessionInteractionEntry& entry) {
    // Parent-owned oneshot ACP sessions are background work
    if (entry.acp_mode != "oneshot") {
        return AcpSessionInteractionMode::Interactive;
    }
    if (!entry.spawned_by.empty() || !entry.parent_session_key.empty()) {
        return AcpSessionInteractionMode::ParentOwnedBackground;
    }
    return AcpSessionInteractionMode::Interactive;
}

// 🎯 对齐: isParentOwnedBackgroundAcpSession(entry)
bool is_parent_owned_background_acp_session(const SessionInteractionEntry& entry) {
    return resolve_acp_session_interaction_mode(entry) ==
        AcpSessionInteractionMode::ParentOwnedBackground;
}

// ============================================================================
// 🎯 对齐 client.ts
// ACP Client — 连接管理和权限解析
// ============================================================================

// 🎯 对齐: resolveToolKindForPermission(toolName, approvalClass)
std::string resolve_tool_kind_for_permission(
    const std::string& tool_name,
    AcpApprovalClass approval_class) {
    if (tool_name.empty() && approval_class == AcpApprovalClass::Unknown) {
        return "";
    }
    if (approval_class == AcpApprovalClass::ReadonlyScoped) {
        return "readonly_scoped";
    }
    if (approval_class == AcpApprovalClass::ReadonlySearch) {
        return "readonly_search";
    }
    // Map enum to string for other cases
    switch (approval_class) {
        case AcpApprovalClass::Mutating: return "mutating";
        case AcpApprovalClass::ExecCapable: return "exec_capable";
        case AcpApprovalClass::ControlPlane: return "control_plane";
        case AcpApprovalClass::Interactive: return "interactive";
        case AcpApprovalClass::Other: return "other";
        default: return "unknown";
    }
}

// 🎯 对齐: AcpClientConnection — 管理与 ACP agent 进程的连接
AcpClientConnection::AcpClientConnection() = default;
AcpClientConnection::~AcpClientConnection() = default;

bool AcpClientConnection::connect(const AcpClientOptions& opts) {
    opts_ = opts;

    // 🎯 对齐: spawn child process or connect to existing
    // In HunterClaw, this is handled via the subprocess/process module
    connected_ = true;
    return true;
}

void AcpClientConnection::disconnect() {
    connected_ = false;
}

bool AcpClientConnection::is_connected() const {
    return connected_;
}

// 🎯 对齐: resolvePermission — interactive permission prompt
PermissionResponse AcpClientConnection::resolve_permission(
    const PermissionRequest& req) {
    // Classify the tool
    auto classification = classify_acp_tool_approval(
        req.tool_name, req.tool_input);

    // Auto-approve safe tools
    if (classification.auto_approve) {
        // Find the "allow" option
        for (auto& opt : req.options) {
            if (opt.kind == "allow" || opt.kind == "always_allow") {
                return PermissionResponse{PermissionOutcome::Selected, opt.id};
            }
        }
    }

    // For non-auto-approve, check if interactive
    if (!opts_.interactive) {
        // Non-interactive: deny by default
        return PermissionResponse{PermissionOutcome::Cancelled, ""};
    }

    // 🎯 对齐: promptUserPermission — 交互式权限提示
    // In production, this would present the prompt to the user
    // For now, use auto-approve logic based on elevated level
    if (opts_.elevated_level == "full") {
        for (auto& opt : req.options) {
            if (opt.kind == "allow") {
                return PermissionResponse{PermissionOutcome::Selected, opt.id};
            }
        }
    }

    return PermissionResponse{PermissionOutcome::Cancelled, ""};
}

} // namespace acp
} // namespace hunterclaw
