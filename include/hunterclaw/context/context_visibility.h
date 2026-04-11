#pragma once

// 严格对齐 OpenClaw config/context-visibility.ts (46行)
// + security/context-visibility.ts (59行)
// 上下文可见性评估。

#include <optional>
#include <string>

namespace hunterclaw {
namespace context {

// ============================================================================
// Types (对齐 security/context-visibility.ts:3-14)
// ============================================================================

/// 对齐 ContextVisibilityMode (config/types.base.ts)
enum class ContextVisibilityMode {
  ALL,               // All supplemental context included
  ALLOWLIST,         // Only from allowlisted senders
  ALLOWLIST_QUOTE    // Allowlisted senders + quoted messages
};

/// 对齐 ContextVisibilityKind
enum class ContextVisibilityKind {
  HISTORY,
  THREAD,
  QUOTE,
  FORWARDED
};

/// 对齐 ContextVisibilityDecisionReason
enum class ContextVisibilityDecisionReason {
  MODE_ALL,
  SENDER_ALLOWED,
  QUOTE_OVERRIDE,
  BLOCKED
};

/// 对齐 ContextVisibilityDecision
struct ContextVisibilityDecision {
  bool include;
  ContextVisibilityDecisionReason reason;
};

// ============================================================================
// Evaluate (对齐 security/context-visibility.ts:16-38)
// ============================================================================

/// Evaluate whether supplemental context should be included.
/// 对齐 evaluateSupplementalContextVisibility
ContextVisibilityDecision evaluate_supplemental_context_visibility(
    ContextVisibilityMode mode,
    ContextVisibilityKind kind,
    bool sender_allowed
);

/// Convenience wrapper.
/// 对齐 shouldIncludeSupplementalContext
bool should_include_supplemental_context(
    ContextVisibilityMode mode,
    ContextVisibilityKind kind,
    bool sender_allowed
);

// ============================================================================
// Config Resolution (对齐 config/context-visibility.ts:19-46)
// ============================================================================

/// Resolve the default context visibility from config.
std::optional<ContextVisibilityMode> resolve_default_context_visibility(
    const std::string& config_json
);

/// Resolve channel-specific context visibility mode.
ContextVisibilityMode resolve_channel_context_visibility_mode(
    const std::string& config_json,
    const std::string& channel,
    const std::optional<std::string>& account_id = std::nullopt,
    const std::optional<ContextVisibilityMode>& configured = std::nullopt
);

/// Convert ContextVisibilityMode to/from string.
std::string context_visibility_mode_to_string(ContextVisibilityMode mode);
std::optional<ContextVisibilityMode> string_to_context_visibility_mode(const std::string& str);

}  // namespace context
}  // namespace hunterclaw
