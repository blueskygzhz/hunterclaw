#include "hunterclaw/context/context_window.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace context {

// ============================================================================
// ContextTokenCache (对齐 context-cache.ts:1-8)
// ============================================================================

ContextTokenCache& ContextTokenCache::instance() {
  static ContextTokenCache cache;
  return cache;
}

std::optional<int> ContextTokenCache::lookup(const std::string& model_id) const {
  if (model_id.empty()) return std::nullopt;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cache_.find(model_id);
  if (it != cache_.end()) return it->second;
  return std::nullopt;
}

void ContextTokenCache::set(const std::string& model_id, int tokens) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_[model_id] = tokens;
}

void ContextTokenCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.clear();
}

std::map<std::string, int> ContextTokenCache::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_;
}

// ============================================================================
// Context Window Runtime State (对齐 context-runtime-state.ts:6-37)
// ============================================================================

ContextWindowRuntimeState& get_context_window_runtime_state() {
  static ContextWindowRuntimeState state;
  return state;
}

void reset_context_window_cache_for_test() {
  auto& state = get_context_window_runtime_state();
  state.load_started = false;
  state.load_complete = false;
  state.config_load_failures = 0;
  state.next_config_load_attempt_at_ms = 0;
  ContextTokenCache::instance().clear();
}

// ============================================================================
// Apply Functions (对齐 context.ts:36-88)
// ============================================================================

void apply_discovered_context_windows(const std::vector<ModelEntry>& models) {
  auto& cache = ContextTokenCache::instance();
  
  for (const auto& model : models) {
    if (model.id.empty()) continue;
    
    // 对齐 context.ts:44-49: prefer contextTokens, fallback to contextWindow
    std::optional<int> context_tokens;
    if (model.context_tokens && *model.context_tokens > 0) {
      context_tokens = *model.context_tokens;
    } else if (model.context_window && *model.context_window > 0) {
      context_tokens = *model.context_window;
    }
    
    if (!context_tokens || *context_tokens <= 0) continue;
    
    // 对齐 context.ts:53-58: cache the most conservative effective limit
    auto existing = cache.lookup(model.id);
    if (!existing || *context_tokens < *existing) {
      cache.set(model.id, *context_tokens);
    }
  }
}

void apply_configured_context_windows(const std::string& models_config_json) {
  // 对齐 context.ts:62-88
  if (models_config_json.empty()) return;
  
  try {
    auto config = json::parse(models_config_json);
    if (!config.contains("providers") || !config["providers"].is_object()) return;
    
    auto& cache = ContextTokenCache::instance();
    
    for (auto& [provider_key, provider_val] : config["providers"].items()) {
      if (!provider_val.is_object()) continue;
      if (!provider_val.contains("models") || !provider_val["models"].is_array()) continue;
      
      for (const auto& model : provider_val["models"]) {
        if (!model.is_object()) continue;
        
        std::string model_id;
        if (model.contains("id") && model["id"].is_string()) {
          model_id = model["id"].get<std::string>();
        }
        if (model_id.empty()) continue;
        
        int context_tokens = 0;
        if (model.contains("contextTokens") && model["contextTokens"].is_number()) {
          context_tokens = model["contextTokens"].get<int>();
        } else if (model.contains("contextWindow") && model["contextWindow"].is_number()) {
          context_tokens = model["contextWindow"].get<int>();
        }
        
        if (context_tokens > 0) {
          cache.set(model_id, context_tokens);
        }
      }
    }
  } catch (const json::exception& e) {
    std::cerr << "[ContextWindow] Failed to parse models config: " << e.what() << std::endl;
  }
}

// ============================================================================
// Lookup (对齐 context.ts:243-259)
// ============================================================================

std::optional<int> lookup_context_tokens(const std::string& model_id) {
  if (model_id.empty()) return std::nullopt;
  return ContextTokenCache::instance().lookup(model_id);
}

// ============================================================================
// Provider Model Ref (对齐 context.ts:286-312)
// ============================================================================

static std::string normalize_lowercase(const std::string& s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(), ::tolower);
  // Trim
  auto start = result.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = result.find_last_not_of(" \t\n\r");
  return result.substr(start, end - start + 1);
}

static std::string normalize_provider_id(const std::string& raw) {
  // 对齐 OpenClaw normalizeProviderId: lowercase + trim
  std::string normalized = normalize_lowercase(raw);
  if (normalized.empty()) return "";
  
  // Remove common suffixes/aliases
  // "z.ai" → "zai", "anthropic-vertex" stays as-is
  std::string result;
  for (char c : normalized) {
    if (c != '.' && c != ' ') {
      result += c;
    }
  }
  return result;
}

std::optional<ProviderModelRef> resolve_provider_model_ref(
    const std::string& provider,
    const std::string& model
) {
  // 对齐 context.ts:286-312
  std::string model_raw = model;
  // Trim
  auto trim = [](const std::string& s) -> std::string {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
  };
  
  model_raw = trim(model_raw);
  if (model_raw.empty()) return std::nullopt;
  
  std::string provider_raw = trim(provider);
  if (!provider_raw.empty()) {
    std::string p = normalize_provider_id(provider_raw);
    if (p.empty()) return std::nullopt;
    return ProviderModelRef{p, model_raw};
  }
  
  // Infer provider from slash in model string
  auto slash = model_raw.find('/');
  if (slash == std::string::npos || slash == 0) return std::nullopt;
  
  std::string p = normalize_provider_id(model_raw.substr(0, slash));
  std::string m = trim(model_raw.substr(slash + 1));
  if (p.empty() || m.empty()) return std::nullopt;
  
  return ProviderModelRef{p, m};
}

// ============================================================================
// Anthropic 1M (对齐 context.ts:373-382)
// ============================================================================

static const std::vector<std::string> ANTHROPIC_1M_MODEL_PREFIXES = {
  "claude-opus-4", "claude-sonnet-4"
};

bool is_anthropic_1m_model(const std::string& provider, const std::string& model) {
  if (normalize_lowercase(provider) != "anthropic") return false;
  
  std::string normalized = normalize_lowercase(model);
  // Extract model ID after last slash if present
  auto slash = normalized.rfind('/');
  if (slash != std::string::npos) {
    normalized = normalized.substr(slash + 1);
  }
  
  for (const auto& prefix : ANTHROPIC_1M_MODEL_PREFIXES) {
    if (normalized.substr(0, prefix.size()) == prefix) {
      return true;
    }
  }
  return false;
}

// ============================================================================
// Resolve Context Tokens (对齐 context.ts:384-470)
// ============================================================================

// 对齐 context.ts:319-358 resolveConfiguredProviderContextTokens
static std::optional<int> resolve_configured_provider_context_tokens(
    const json& config,
    const std::string& provider,
    const std::string& model
) {
  try {
    if (!config.contains("models")) return std::nullopt;
    const auto& models = config["models"];
    if (!models.contains("providers") || !models["providers"].is_object()) return std::nullopt;
    
    std::string norm_provider = normalize_provider_id(provider);
    
    for (auto& [pid, pconfig] : models["providers"].items()) {
      if (normalize_provider_id(pid) != norm_provider) continue;
      if (!pconfig.is_object() || !pconfig.contains("models") || !pconfig["models"].is_array()) continue;
      
      for (const auto& m : pconfig["models"]) {
        if (!m.is_object()) continue;
        std::string mid = m.value("id", "");
        if (mid != model) continue;
        
        int ct = 0;
        if (m.contains("contextTokens") && m["contextTokens"].is_number()) {
          ct = m["contextTokens"].get<int>();
        } else if (m.contains("contextWindow") && m["contextWindow"].is_number()) {
          ct = m["contextWindow"].get<int>();
        }
        if (ct > 0) return ct;
      }
    }
  } catch (...) {}
  
  return std::nullopt;
}

std::optional<int> resolve_context_tokens_for_model(
    const ResolveContextTokensParams& params
) {
  // 对齐 context.ts:392-394
  if (params.context_tokens_override && *params.context_tokens_override > 0) {
    return params.context_tokens_override;
  }
  
  auto ref = resolve_provider_model_ref(params.provider, params.model);
  
  if (ref) {
    // 对齐 context.ts:402-405: check context1m for Anthropic models
    if (is_anthropic_1m_model(ref->provider, ref->model)) {
      try {
        auto config = json::parse(params.config_json);
        if (config.contains("agents") && config["agents"].contains("defaults") &&
            config["agents"]["defaults"].contains("models")) {
          auto& models = config["agents"]["defaults"]["models"];
          std::string key = normalize_lowercase(ref->provider + "/" + ref->model);
          for (auto& [k, v] : models.items()) {
            if (normalize_lowercase(k) == key && v.is_object()) {
              if (v.contains("params") && v["params"].is_object()) {
                if (v["params"].value("context1m", false)) {
                  return ANTHROPIC_CONTEXT_1M_TOKENS;
                }
              }
            }
          }
        }
      } catch (...) {}
    }
    
    // 对齐 context.ts:413-422: check configured provider context tokens
    if (!params.provider.empty()) {
      try {
        auto config = json::parse(params.config_json);
        auto ct = resolve_configured_provider_context_tokens(config, params.provider, ref->model);
        if (ct) return ct;
      } catch (...) {}
    }
  }
  
  // 对齐 context.ts:437-444: qualified cache lookup
  if (!params.provider.empty() && ref && ref->model.find('/') == std::string::npos) {
    auto qualified = lookup_context_tokens(
      normalize_provider_id(ref->provider) + "/" + ref->model
    );
    if (qualified) return qualified;
  }
  
  // 对齐 context.ts:449-454: bare key fallback
  auto bare = lookup_context_tokens(params.model);
  if (bare) return bare;
  
  // 对齐 context.ts:459-467: implicit provider qualified fallback
  if (params.provider.empty() && ref && ref->model.find('/') == std::string::npos) {
    auto qualified = lookup_context_tokens(
      normalize_provider_id(ref->provider) + "/" + ref->model
    );
    if (qualified) return qualified;
  }
  
  return params.fallback_context_tokens;
}

}  // namespace context
}  // namespace hunterclaw
