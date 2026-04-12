#pragma once

// ============================================================================
// acp_client.h
// 🎯 对齐 OpenClaw: src/acp/client.ts + approval-classifier.ts + policy.ts
//                    + session-interaction-mode.ts
// ============================================================================

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

AcpApprovalClassification classify_acp_tool_approval(
    const std::string& tool_name,
    const nlohmann::json& tool_input = {});

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

struct PermissionOption {
    std::string id;
    std::string kind;
    std::string label;
};

struct PermissionRequest {
    std::string tool_name;
    nlohmann::json tool_input;
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

private:
    AcpClientOptions opts_;
    bool connected_ = false;
};

} // namespace acp
} // namespace hunterclaw
