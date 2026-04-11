#pragma once

// 严格对齐 OpenClaw agents/context.ts (471行) + context-cache.ts (9行)
// + context-runtime-state.ts (37行)
//
// Context window token 缓存: 从 model registry、config 中发现 context window 大小，
// 缓存后供 agent runtime 查询 model 的最大 context token 数。

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace context {

// ============================================================================
// Constants (对齐 context.ts:27-34)
// ============================================================================

static constexpr int ANTHROPIC_CONTEXT_1M_TOKENS = 1048576;

// ============================================================================
// Model Entry (对齐 context.ts:17)
// ============================================================================

struct ModelEntry {
  std::string id;
  std::optional<int> context_window;
  std::optional<int> context_tokens;
};

// ============================================================================
// Context Token Cache (对齐 context-cache.ts:1-8)
// ============================================================================

class ContextTokenCache {
 public:
  static ContextTokenCache& instance();

  std::optional<int> lookup(const std::string& model_id) const;
  void set(const std::string& model_id, int tokens);
  void clear();
  std::map<std::string, int> snapshot() const;

 private:
  ContextTokenCache() = default;
  mutable std::mutex mutex_;
  std::map<std::string, int> cache_;
};

// ============================================================================
// Context Window Runtime State (对齐 context-runtime-state.ts:6-37)
// ============================================================================

struct ContextWindowRuntimeState {
  bool load_started = false;
  bool load_complete = false;
  int config_load_failures = 0;
  int64_t next_config_load_attempt_at_ms = 0;
};

ContextWindowRuntimeState& get_context_window_runtime_state();

/// Reset state for testing (对齐 resetContextWindowCacheForTest)
void reset_context_window_cache_for_test();

// ============================================================================
// Discovery Functions (对齐 context.ts:36-88)
// ============================================================================

/// Apply discovered context windows from model registry entries.
/// 对齐 context.ts:36-60 applyDiscoveredContextWindows
void apply_discovered_context_windows(
    const std::vector<ModelEntry>& models
);

/// Apply configured context windows from models config JSON.
/// 对齐 context.ts:62-88 applyConfiguredContextWindows
void apply_configured_context_windows(
    const std::string& models_config_json
);

// ============================================================================
// Lookup Functions (对齐 context.ts:243-470)
// ============================================================================

/// Look up cached context tokens for a model.
/// 对齐 context.ts:243-259 lookupContextTokens
std::optional<int> lookup_context_tokens(const std::string& model_id);

/// Resolve context tokens for a specific provider+model combination.
/// 对齐 context.ts:384-470 resolveContextTokensForModel
struct ResolveContextTokensParams {
  std::string config_json;      // OpenClaw config as JSON
  std::string provider;         // e.g. "anthropic", "openai"
  std::string model;            // e.g. "claude-opus-4", "gpt-4o"
  std::optional<int> context_tokens_override;
  std::optional<int> fallback_context_tokens;
};

std::optional<int> resolve_context_tokens_for_model(
    const ResolveContextTokensParams& params
);

/// Check if a model is an Anthropic 1M context model.
/// 对齐 context.ts:373-382 isAnthropic1MModel
bool is_anthropic_1m_model(const std::string& provider, const std::string& model);

/// Resolve provider+model from a potentially qualified model string.
/// 对齐 context.ts:286-312 resolveProviderModelRef
struct ProviderModelRef {
  std::string provider;
  std::string model;
};

std::optional<ProviderModelRef> resolve_provider_model_ref(
    const std::string& provider,
    const std::string& model
);

}  // namespace context
}  // namespace hunterclaw
