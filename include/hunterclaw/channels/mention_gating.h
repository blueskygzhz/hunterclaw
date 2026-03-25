// Aligned with openclaw: src/channels/mention-gating.ts
#pragma once

namespace hunterclaw {
namespace channels {

// ============================================================================
// MentionGating
// Aligned with OpenClaw: mention-gating.ts
// ============================================================================

// Aligned with OpenClaw: MentionGateParams
struct MentionGateParams {
    bool require_mention = false;
    bool can_detect_mention = false;
    bool was_mentioned = false;
    bool implicit_mention = false;
    bool should_bypass_mention = false;
};

// Aligned with OpenClaw: MentionGateResult
struct MentionGateResult {
    bool effective_was_mentioned = false;
    bool should_skip = false;
};

// Aligned with OpenClaw: MentionGateWithBypassParams
struct MentionGateWithBypassParams {
    bool is_group = false;
    bool require_mention = false;
    bool can_detect_mention = false;
    bool was_mentioned = false;
    bool implicit_mention = false;
    bool has_any_mention = false;
    bool allow_text_commands = false;
    bool has_control_command = false;
    bool command_authorized = false;
};

// Aligned with OpenClaw: MentionGateWithBypassResult
struct MentionGateWithBypassResult : MentionGateResult {
    bool should_bypass_mention = false;
};

/**
 * Resolve mention gating.
 * Aligned with OpenClaw: resolveMentionGating()
 */
inline MentionGateResult resolve_mention_gating(const MentionGateParams& params) {
    const bool implicit = params.implicit_mention;
    const bool bypass = params.should_bypass_mention;
    const bool effective_was_mentioned = params.was_mentioned || implicit || bypass;
    const bool should_skip = params.require_mention &&
                             params.can_detect_mention &&
                             !effective_was_mentioned;
    return {effective_was_mentioned, should_skip};
}

/**
 * Resolve mention gating with bypass logic.
 * Aligned with OpenClaw: resolveMentionGatingWithBypass()
 */
inline MentionGateWithBypassResult resolve_mention_gating_with_bypass(
    const MentionGateWithBypassParams& params)
{
    const bool should_bypass_mention =
        params.is_group &&
        params.require_mention &&
        !params.was_mentioned &&
        !params.has_any_mention &&
        params.allow_text_commands &&
        params.command_authorized &&
        params.has_control_command;

    auto base = resolve_mention_gating({
        params.require_mention,
        params.can_detect_mention,
        params.was_mentioned,
        params.implicit_mention,
        should_bypass_mention,
    });

    MentionGateWithBypassResult result;
    result.effective_was_mentioned = base.effective_was_mentioned;
    result.should_skip = base.should_skip;
    result.should_bypass_mention = should_bypass_mention;
    return result;
}

} // namespace channels
} // namespace hunterclaw
