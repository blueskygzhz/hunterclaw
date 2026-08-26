#pragma once

// ============================================================================
// acp_client.h
// 🎯 对齐 OpenClaw: src/acp/client.ts + approval-classifier.ts + policy.ts
//                    + session-interaction-mode.ts
// ============================================================================

#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace core { class Config; }
namespace acp {

// ── Approval Classification ──────────────────────────────────────────────────

enum class AcpApprovalClass {
    ReadonlyScoped,
    ReadonlySearch,
    Mutating,
    ExecCapable,
    ControlPlane,
    Interactive,
    Other,
    Unknown
};

struct AcpApprovalClassification {
    std::string tool_name;
    AcpApprovalClass approval_class = AcpApprovalClass::Unknown;
    bool auto_approve = false;
};

/// 対齐 approval-classifier.ts:197-245 classifyAcpToolApproval({ toolCall, cwd })
///
/// ⚠ 签名与此前不同：真源**不接受调用方直接给 tool_name**，而是从
/// toolCall 的 `_meta` / `rawInput` / `title` 三处各自解析再交叉校验
/// （:82-112 resolveToolNameForPermission）。这一步是**防伪造**的核心：
/// 任意两处解析结果不一致就返回 unknown（不自动批准）。
/// 直接信任外部传入的 tool_name 等于把这道闸拆了。
///
/// @param tool_call  ACP 的 toolCall 对象（含 title / _meta / rawInput / locations）
/// @param cwd        当前工作目录，用于判断路径是否在范围内
AcpApprovalClassification classify_acp_tool_approval(
    const nlohmann::json& tool_call,
    const std::string& cwd);

/// 対齐 approval-classifier.ts:82-112 resolveToolNameForPermission
/// 三源交叉校验，任一冲突或格式非法 → 返回空（fail closed）。
std::string resolve_tool_name_for_permission(const nlohmann::json& tool_call);

/// 対齐 approval-classifier.ts:66-72 normalizeToolName
/// lowercase + 长度 ≤128 + 仅允许 [a-z0-9._-]，否则返回空。
std::string normalize_tool_name(const std::string& value);

/// 対齐 approval-classifier.ts:188-194 isToolPathScopedToCwd
/// 解析 file:// 与 ~ 展开后，判断绝对路径是否位于 cwd 之内。
bool is_tool_path_scoped_to_cwd(const std::string& raw_path, const std::string& cwd);

// ── ACP Policy ───────────────────────────────────────────────────────────────

enum class AcpDispatchPolicyState {
    Enabled,
    AcpDisabled,
    DispatchDisabled
};

bool is_acp_enabled_by_policy(const core::Config& cfg);
AcpDispatchPolicyState resolve_acp_dispatch_policy_state(const core::Config& cfg);
bool is_acp_dispatch_enabled_by_policy(const core::Config& cfg);
std::string resolve_acp_dispatch_policy_message(const core::Config& cfg);
bool is_acp_agent_allowed_by_policy(const core::Config& cfg, const std::string& agent_id);

/// 対齐 policy.ts:70-81 resolveAcpAgentPolicyError
/// 允许时返回空串；拒绝时返回运维可读的原因（含归一化后的 agent id）。
std::string resolve_acp_agent_policy_message(const core::Config& cfg,
                                            const std::string& agent_id);

// ── Session Interaction Mode ─────────────────────────────────────────────────

enum class AcpSessionInteractionMode {
    Interactive,
    ParentOwnedBackground
};

struct SessionInteractionEntry {
    std::string spawned_by;
    std::string parent_session_key;
    std::string acp_mode;
};

AcpSessionInteractionMode resolve_acp_session_interaction_mode(
    const SessionInteractionEntry& entry);
bool is_parent_owned_background_acp_session(const SessionInteractionEntry& entry);

// ── Permission Resolution ────────────────────────────────────────────────────

std::string resolve_tool_kind_for_permission(
    const std::string& tool_name, AcpApprovalClass approval_class);

enum class PermissionOutcome { Selected, Cancelled };

/// 対齐 ACP 规范 RequestPermissionRequest.options[].kind
/// 合法取值仅这四个 —— "allow" / "always_allow" 之类不在规范内。
struct PermissionOption {
    std::string id;      ///< optionId
    std::string kind;    ///< allow_once | allow_always | reject_once | reject_always
    std::string label;
};

struct PermissionRequest {
    /// 対齐 client-helpers.ts:115 —— 分类须由 toolCall 自解析（防伪造），
    /// 不接受外部直接指定工具名。
    nlohmann::json tool_call;
    std::string tool_title;   ///< 対齐:114 sanitizeTerminalText(toolCall.title)
    std::vector<PermissionOption> options;
};

struct PermissionResponse {
    PermissionOutcome outcome;
    std::string option_id;
};

// ── ACP Client Connection ────────────────────────────────────────────────────

struct AcpClientOptions {
    bool interactive = true;
    std::string elevated_level;
    std::string cwd;
};

class AcpClientConnection {
public:
    AcpClientConnection();
    ~AcpClientConnection();

    bool connect(const AcpClientOptions& opts);
    void disconnect();
    bool is_connected() const;
    PermissionResponse resolve_permission(const PermissionRequest& req);

    /// 対齐 client-helpers.ts:23 PermissionResolverDeps.prompt
    /// 未设置时等价于 non-TTY 分支（一律不批准，対齐:66-69）。
    using PromptCallback =
        std::function<bool(const std::string& tool_name, const std::string& tool_title)>;
    void set_prompt_callback(PromptCallback cb) { prompt_callback_ = std::move(cb); }

private:
    AcpClientOptions opts_;
    bool connected_ = false;
    PromptCallback prompt_callback_;
};

} // namespace acp
} // namespace hunterclaw
