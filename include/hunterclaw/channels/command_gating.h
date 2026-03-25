// Aligned with openclaw: src/channels/command-gating.ts
#pragma once

#include <vector>
#include <string>

namespace hunterclaw {
namespace channels {

// ============================================================================
// CommandGating
// Aligned with OpenClaw: command-gating.ts
// ============================================================================

// Aligned with OpenClaw: CommandAuthorizer
struct CommandAuthorizer {
    bool configured = false;
    bool allowed = false;
};

// Aligned with OpenClaw: CommandGatingModeWhenAccessGroupsOff
enum class CommandGatingModeWhenAccessGroupsOff {
    Allow,       // "allow"
    Deny,        // "deny"
    Configured   // "configured"
};

/**
 * Resolve whether a command is authorized from a list of authorizers.
 * Aligned with OpenClaw: resolveCommandAuthorizedFromAuthorizers()
 */
inline bool resolve_command_authorized_from_authorizers(
    bool use_access_groups,
    const std::vector<CommandAuthorizer>& authorizers,
    CommandGatingModeWhenAccessGroupsOff mode = CommandGatingModeWhenAccessGroupsOff::Allow)
{
    if (!use_access_groups) {
        if (mode == CommandGatingModeWhenAccessGroupsOff::Allow) return true;
        if (mode == CommandGatingModeWhenAccessGroupsOff::Deny) return false;
        // "configured" mode: check if any authorizer is configured
        bool any_configured = false;
        for (const auto& a : authorizers) {
            if (a.configured) { any_configured = true; break; }
        }
        if (!any_configured) return true;
        for (const auto& a : authorizers) {
            if (a.configured && a.allowed) return true;
        }
        return false;
    }
    // use_access_groups = true
    for (const auto& a : authorizers) {
        if (a.configured && a.allowed) return true;
    }
    return false;
}

// Aligned with OpenClaw: resolveControlCommandGate result
struct ControlCommandGateResult {
    bool command_authorized = false;
    bool should_block = false;
};

/**
 * Resolve control command gate.
 * Aligned with OpenClaw: resolveControlCommandGate()
 */
inline ControlCommandGateResult resolve_control_command_gate(
    bool use_access_groups,
    const std::vector<CommandAuthorizer>& authorizers,
    bool allow_text_commands,
    bool has_control_command,
    CommandGatingModeWhenAccessGroupsOff mode = CommandGatingModeWhenAccessGroupsOff::Allow)
{
    bool command_authorized = resolve_command_authorized_from_authorizers(
        use_access_groups, authorizers, mode);
    bool should_block = allow_text_commands && has_control_command && !command_authorized;
    return {command_authorized, should_block};
}

/**
 * Resolve dual text control command gate.
 * Aligned with OpenClaw: resolveDualTextControlCommandGate()
 */
inline ControlCommandGateResult resolve_dual_text_control_command_gate(
    bool use_access_groups,
    bool primary_configured,
    bool primary_allowed,
    bool secondary_configured,
    bool secondary_allowed,
    bool has_control_command,
    CommandGatingModeWhenAccessGroupsOff mode = CommandGatingModeWhenAccessGroupsOff::Allow)
{
    return resolve_control_command_gate(
        use_access_groups,
        {
            {primary_configured, primary_allowed},
            {secondary_configured, secondary_allowed},
        },
        /*allow_text_commands=*/true,
        has_control_command,
        mode
    );
}

} // namespace channels
} // namespace hunterclaw
