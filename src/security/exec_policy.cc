// 🎯 Strict alignment: OpenClaw node-host/exec-policy.ts
// System run execution policy evaluation — allowlist, deny, approval flow.

#include <hunterclaw/security/audit.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw::security {

namespace {
auto log() { return core::get_logger("exec-policy"); }
} // namespace

// ─── Types (aligned: exec-policy.ts) ─────────────────────────────────────

enum class ExecApprovalDecision {
    kNone,        // null in OC
    kAllowOnce,   // "allow-once"
    kAllowAlways  // "allow-always"
};

enum class ExecSecurity {
    kAllow,       // No restrictions
    kAllowlist,   // Only allowlisted commands
    kDeny         // All execution denied
};

enum class ExecAsk {
    kNever,       // Never ask for approval
    kOnMiss,      // Ask on allowlist miss
    kAlways       // Always ask
};

enum class SystemRunDeniedReason {
    kSecurityDeny,
    kApprovalRequired,
    kAllowlistMiss,
    kExecutionPlanMiss,
    kCompanionUnavailable,
    kPermissionScreenRecording
};

struct SystemRunPolicyDecision {
    bool allowed = false;
    bool analysis_ok = false;
    bool allowlist_satisfied = false;
    bool shell_wrapper_blocked = false;
    bool windows_shell_wrapper_blocked = false;
    bool requires_ask = false;
    ExecApprovalDecision approval_decision = ExecApprovalDecision::kNone;
    bool approved_by_ask = false;

    // Only set when !allowed
    std::optional<std::string> event_reason;
    std::optional<std::string> error_message;
};

// ─── Resolve approval decision (aligned: resolveExecApprovalDecision) ────

ExecApprovalDecision resolve_exec_approval_decision(const std::string& value) {
    // 🎯 对齐 OC: resolveExecApprovalDecision
    if (value == "allow-once") return ExecApprovalDecision::kAllowOnce;
    if (value == "allow-always") return ExecApprovalDecision::kAllowAlways;
    return ExecApprovalDecision::kNone;
}

// ─── Format allowlist miss message (aligned: formatSystemRunAllowlistMissMessage)

std::string format_system_run_allowlist_miss_message(
        bool shell_wrapper_blocked = false,
        bool windows_shell_wrapper_blocked = false) {
    // 🎯 对齐 OC: formatSystemRunAllowlistMissMessage
    if (windows_shell_wrapper_blocked) {
        return "SYSTEM_RUN_DENIED: allowlist miss "
               "(Windows shell wrappers like cmd.exe /c require approval; "
               "approve once/always or run with --ask on-miss|always)";
    }
    if (shell_wrapper_blocked) {
        return "SYSTEM_RUN_DENIED: allowlist miss "
               "(shell wrappers like sh/bash/zsh -c require approval; "
               "approve once/always or run with --ask on-miss|always)";
    }
    return "SYSTEM_RUN_DENIED: allowlist miss";
}

// ─── Requires exec approval (aligned: requiresExecApproval in exec-approvals.ts)

bool requires_exec_approval(
        ExecAsk ask,
        ExecSecurity security,
        bool analysis_ok,
        bool allowlist_satisfied,
        bool durable_approval_satisfied = false) {
    // 🎯 对齐 OC: requiresExecApproval
    if (ask == ExecAsk::kAlways) return true;
    if (ask == ExecAsk::kNever) return false;

    // ask == kOnMiss: require approval when allowlist check fails
    if (security == ExecSecurity::kAllowlist) {
        if (!analysis_ok || !allowlist_satisfied) {
            return !durable_approval_satisfied;
        }
    }
    return false;
}

// ─── Main policy evaluation (aligned: evaluateSystemRunPolicy) ───────────

SystemRunPolicyDecision evaluate_system_run_policy(
        ExecSecurity security,
        ExecAsk ask,
        bool analysis_ok,
        bool allowlist_satisfied,
        ExecApprovalDecision approval_decision,
        bool approved,
        bool is_windows,
        bool cmd_invocation,
        bool shell_wrapper_invocation,
        bool durable_approval_satisfied) {

    // 🎯 对齐 OC: evaluateSystemRunPolicy

    // POSIX node execution intentionally uses /bin/sh -lc as transport wrapper.
    // Keep allowlist decisions based on analyzed inner shell payload.
    // Windows cmd.exe /c wrappers still require explicit approval.
    bool windows_shell_wrapper_blocked =
        security == ExecSecurity::kAllowlist &&
        shell_wrapper_invocation &&
        is_windows &&
        cmd_invocation;
    bool shell_wrapper_blocked = windows_shell_wrapper_blocked;

    // Override analysis/allowlist if shell wrapper is blocked
    bool effective_analysis_ok = shell_wrapper_blocked ? false : analysis_ok;
    bool effective_allowlist_satisfied = shell_wrapper_blocked ? false : allowlist_satisfied;
    bool approved_by_ask = (approval_decision != ExecApprovalDecision::kNone) || approved;

    SystemRunPolicyDecision result;
    result.analysis_ok = effective_analysis_ok;
    result.allowlist_satisfied = effective_allowlist_satisfied;
    result.shell_wrapper_blocked = shell_wrapper_blocked;
    result.windows_shell_wrapper_blocked = windows_shell_wrapper_blocked;
    result.approval_decision = approval_decision;
    result.approved_by_ask = approved_by_ask;

    // 1. Security = deny → always deny
    if (security == ExecSecurity::kDeny) {
        result.allowed = false;
        result.event_reason = "security=deny";
        result.error_message = "SYSTEM_RUN_DISABLED: security=deny";
        result.requires_ask = false;
        return result;
    }

    // 2. Check if approval is required
    bool requires_ask_val = requires_exec_approval(
        ask, security, effective_analysis_ok, effective_allowlist_satisfied,
        durable_approval_satisfied);
    result.requires_ask = requires_ask_val;

    if (requires_ask_val && !approved_by_ask) {
        result.allowed = false;
        result.event_reason = "approval-required";
        result.error_message = "SYSTEM_RUN_DENIED: approval required";
        return result;
    }

    // 3. Allowlist mode: check analysis + allowlist
    if (security == ExecSecurity::kAllowlist &&
        (!effective_analysis_ok || !effective_allowlist_satisfied) &&
        !approved_by_ask) {

        if (durable_approval_satisfied) {
            // Durable approval overrides allowlist miss
            result.allowed = true;
            return result;
        }

        result.allowed = false;
        result.event_reason = "allowlist-miss";
        result.error_message = format_system_run_allowlist_miss_message(
            shell_wrapper_blocked, windows_shell_wrapper_blocked);
        return result;
    }

    // 4. Allow
    result.allowed = true;
    return result;
}

// ─── Convenience: evaluate from config JSON ──────────────────────────────

SystemRunPolicyDecision evaluate_system_run_policy_from_config(
        const nlohmann::json& config,
        const std::string& command,
        bool analysis_ok,
        bool allowlist_satisfied) {

    // Parse security mode
    auto security_str = config.value("/security/exec"_json_pointer, std::string("allowlist"));
    ExecSecurity security = ExecSecurity::kAllowlist;
    if (security_str == "allow" || security_str == "off") {
        security = ExecSecurity::kAllow;
    } else if (security_str == "deny") {
        security = ExecSecurity::kDeny;
    }

    // Parse ask mode
    auto ask_str = config.value("/security/ask"_json_pointer, std::string("on-miss"));
    ExecAsk ask = ExecAsk::kOnMiss;
    if (ask_str == "never") {
        ask = ExecAsk::kNever;
    } else if (ask_str == "always") {
        ask = ExecAsk::kAlways;
    }

    // Detect shell wrapper invocation
    bool shell_wrapper = command.find("sh -c") != std::string::npos ||
                         command.find("bash -c") != std::string::npos ||
                         command.find("zsh -c") != std::string::npos;

    log()->debug("evaluate_system_run_policy: cmd='{}' security={} ask={} analysis_ok={} allowlist={}",
        command.substr(0, 80), security_str, ask_str, analysis_ok, allowlist_satisfied);

    return evaluate_system_run_policy(
        security, ask,
        analysis_ok, allowlist_satisfied,
        ExecApprovalDecision::kNone,
        false, // approved
        false, // is_windows
        false, // cmd_invocation
        shell_wrapper,
        false  // durable_approval_satisfied
    );
}

} // namespace hunterclaw::security
