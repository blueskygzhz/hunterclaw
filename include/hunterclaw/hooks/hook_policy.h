#pragma once
// Aligned with openclaw: src/hooks/policy.ts (144 lines)
// Hook source priority, enable state resolution, collision handling

#include "hunterclaw/hooks/hooks.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace hooks {

// ============================================================================
// Hook Source Policy (对齐 policy.ts:12-55)
// ============================================================================

struct HookSourcePolicy {
    int precedence;
    bool trusted_local_code;
    std::string default_enable_mode;  // "default-on" | "explicit-opt-in"
    std::vector<HookSource> can_override;
    std::vector<HookSource> can_be_overridden_by;
};

/// Get policy for a given hook source (对齐: getHookSourcePolicy)
HookSourcePolicy get_hook_source_policy(HookSource source);

// ============================================================================
// Hook Enable State (对齐 policy.ts:5-10)
// ============================================================================

enum class HookEnableStateReason {
    NONE,
    DISABLED_IN_CONFIG,
    WORKSPACE_HOOK_DISABLED_BY_DEFAULT
};

struct HookEnableState {
    bool enabled = true;
    HookEnableStateReason reason = HookEnableStateReason::NONE;
};

/// Resolve hook enable state from config (对齐: resolveHookEnableState)
HookEnableState resolve_hook_enable_state(
    const HookEntry& entry,
    const std::optional<core::Config>& config = std::nullopt);

/// Resolve hook config for a specific hook key (对齐: resolveHookConfig)
std::optional<core::JsonValue> resolve_hook_config(
    const std::optional<core::Config>& config,
    const std::string& hook_key);

// ============================================================================
// Hook Entry Resolution (对齐 policy.ts:109-143)
// ============================================================================

struct HookResolutionCollision {
    std::string name;
    HookEntry kept;
    HookEntry ignored;
};

using CollisionCallback = std::function<void(const HookResolutionCollision&)>;

/// Check if candidate can override existing hook (对齐: canOverrideHook)
bool can_override_hook(const HookEntry& candidate, const HookEntry& existing);

/// Resolve hook entries with priority-based dedup (对齐: resolveHookEntries)
std::vector<HookEntry> resolve_hook_entries(
    const std::vector<HookEntry>& entries,
    CollisionCallback on_collision = nullptr);

}  // namespace hooks
}  // namespace hunterclaw
