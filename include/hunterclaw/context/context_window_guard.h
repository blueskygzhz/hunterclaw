#pragma once

// 严格对齐 OpenClaw agents/context-window-guard.ts (82行)
// Context window 硬限制 + 警告评估

#include <optional>
#include <string>

namespace hunterclaw {
namespace context {

// ============================================================================
// Constants (对齐 context-window-guard.ts:4-5)
// ============================================================================

static constexpr int CONTEXT_WINDOW_HARD_MIN_TOKENS = 16000;
static constexpr int CONTEXT_WINDOW_WARN_BELOW_TOKENS = 32000;

// ============================================================================
// Types (对齐 context-window-guard.ts:7-12)
// ============================================================================

enum class ContextWindowSource {
  MODEL,
  MODELS_CONFIG,
  AGENT_CONTEXT_TOKENS,
  DEFAULT
};

struct ContextWindowInfo {
  int tokens;
  ContextWindowSource source;
};

// ============================================================================
// Resolve (对齐 context-window-guard.ts:22-57)
// ============================================================================

struct ResolveContextWindowParams {
  std::string config_json;              // OpenClaw config as JSON
  std::string provider;
  std::string model_id;
  std::optional<int> model_context_tokens;
  std::optional<int> model_context_window;
  int default_tokens;
};

/// Resolve the effective context window size from multiple sources.
/// Priority: modelsConfig > model metadata > agentContextTokens cap > default
ContextWindowInfo resolve_context_window_info(
    const ResolveContextWindowParams& params
);

// ============================================================================
// Guard (对齐 context-window-guard.ts:59-81)
// ============================================================================

struct ContextWindowGuardResult {
  int tokens;
  ContextWindowSource source;
  bool should_warn;
  bool should_block;
};

/// Evaluate whether the resolved context window is dangerously small.
ContextWindowGuardResult evaluate_context_window_guard(
    const ContextWindowInfo& info,
    int warn_below_tokens = CONTEXT_WINDOW_WARN_BELOW_TOKENS,
    int hard_min_tokens = CONTEXT_WINDOW_HARD_MIN_TOKENS
);

/// Convert ContextWindowSource to string.
std::string context_window_source_to_string(ContextWindowSource source);

}  // namespace context
}  // namespace hunterclaw
