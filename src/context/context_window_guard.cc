#include "hunterclaw/context/context_window_guard.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace context {

// ============================================================================
// Helpers (对齐 context-window-guard.ts:14-20)
// ============================================================================

static std::optional<int> normalize_positive_int(double value) {
  if (!std::isfinite(value)) return std::nullopt;
  int int_val = static_cast<int>(std::floor(value));
  return int_val > 0 ? std::optional<int>(int_val) : std::nullopt;
}

static std::string normalize_provider_id(const std::string& raw) {
  std::string result;
  for (char c : raw) {
    if (c != '.' && c != ' ') {
      result += static_cast<char>(::tolower(c));
    }
  }
  auto start = result.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = result.find_last_not_of(" \t\n\r");
  return result.substr(start, end - start + 1);
}

// ============================================================================
// Resolve (对齐 context-window-guard.ts:22-57)
// ============================================================================

ContextWindowInfo resolve_context_window_info(
    const ResolveContextWindowParams& params
) {
  // 1. Check modelsConfig (对齐 context-window-guard.ts:30-41)
  std::optional<int> from_models_config;
  try {
    auto cfg = json::parse(params.config_json);
    if (cfg.contains("models") && cfg["models"].is_object()) {
      auto& providers = cfg["models"]["providers"];
      if (providers.is_object()) {
        std::string norm_provider = normalize_provider_id(params.provider);
        for (auto& [pid, pval] : providers.items()) {
          if (normalize_provider_id(pid) != norm_provider) continue;
          if (!pval.is_object() || !pval.contains("models") || !pval["models"].is_array()) continue;
          for (const auto& m : pval["models"]) {
            if (!m.is_object()) continue;
            if (m.value("id", "") != params.model_id) continue;
            auto ct = normalize_positive_int(m.value("contextTokens", 0.0));
            if (!ct) ct = normalize_positive_int(m.value("contextWindow", 0.0));
            if (ct) { from_models_config = ct; break; }
          }
          if (from_models_config) break;
        }
      }
    }
  } catch (...) {}
  
  // 2. Check model metadata (对齐 context-window-guard.ts:42-44)
  auto from_model = normalize_positive_int(
    params.model_context_tokens ? static_cast<double>(*params.model_context_tokens) : 0.0
  );
  if (!from_model) {
    from_model = normalize_positive_int(
      params.model_context_window ? static_cast<double>(*params.model_context_window) : 0.0
    );
  }
  
  // 3. Build base info (对齐 context-window-guard.ts:45-49)
  ContextWindowInfo base_info;
  if (from_models_config) {
    base_info = {*from_models_config, ContextWindowSource::MODELS_CONFIG};
  } else if (from_model) {
    base_info = {*from_model, ContextWindowSource::MODEL};
  } else {
    base_info = {static_cast<int>(std::floor(params.default_tokens)), ContextWindowSource::DEFAULT};
  }
  
  // 4. Apply agentContextTokens cap (对齐 context-window-guard.ts:51-54)
  try {
    auto cfg = json::parse(params.config_json);
    if (cfg.contains("agents") && cfg["agents"].is_object() &&
        cfg["agents"].contains("defaults") && cfg["agents"]["defaults"].is_object()) {
      auto cap = normalize_positive_int(cfg["agents"]["defaults"].value("contextTokens", 0.0));
      if (cap && *cap < base_info.tokens) {
        return {*cap, ContextWindowSource::AGENT_CONTEXT_TOKENS};
      }
    }
  } catch (...) {}
  
  return base_info;
}

// ============================================================================
// Guard (对齐 context-window-guard.ts:64-81)
// ============================================================================

ContextWindowGuardResult evaluate_context_window_guard(
    const ContextWindowInfo& info,
    int warn_below_tokens,
    int hard_min_tokens
) {
  int warn_below = std::max(1, static_cast<int>(std::floor(warn_below_tokens)));
  int hard_min = std::max(1, static_cast<int>(std::floor(hard_min_tokens)));
  int tokens = std::max(0, static_cast<int>(std::floor(info.tokens)));
  
  return ContextWindowGuardResult{
    tokens,
    info.source,
    tokens > 0 && tokens < warn_below,   // shouldWarn
    tokens > 0 && tokens < hard_min       // shouldBlock
  };
}

// ============================================================================
// Utility
// ============================================================================

std::string context_window_source_to_string(ContextWindowSource source) {
  switch (source) {
    case ContextWindowSource::MODEL: return "model";
    case ContextWindowSource::MODELS_CONFIG: return "modelsConfig";
    case ContextWindowSource::AGENT_CONTEXT_TOKENS: return "agentContextTokens";
    case ContextWindowSource::DEFAULT: return "default";
  }
  return "unknown";
}

}  // namespace context
}  // namespace hunterclaw
