#pragma once
// ============================================================================
// models_config.h
// 严格对齐 OpenClaw: models-config.ts (150行)
//                   + models-config.merge.ts (200行)
//                   + models-config.providers.ts (20行)
//                   + models-config.providers.implicit.ts (300行)
//                   + models-config.providers.normalize.ts (130行)
//                   + models-config.providers.secrets.ts (400行)
//                   + models-config.providers.static.ts (100行)
//                   + models-config.providers.source-managed.ts (120行)
//                   + models-config.providers.policy.ts (30行)
//                   + models-config.plan.ts (120行)
// ============================================================================

#include "hunterclaw/llm/model_selection.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <set>

namespace hunterclaw {
namespace llm {

// ============================================================================
// Model Definition Config (对齐 config/types.ts: ModelDefinitionConfig)
// ============================================================================

struct ModelDefinitionConfig {
    std::string id;
    std::optional<std::string> name;
    std::optional<std::string> api;           // "openai-completions" | "anthropic-messages" | ...
    std::optional<std::string> base_url;
    std::optional<int> context_window;
    std::optional<int> max_tokens;
    std::optional<bool> reasoning;
    std::vector<std::string> input;           // "text" | "image" | "document"
    std::map<std::string, std::string> headers;
};

// ============================================================================
// Model Provider Config (对齐 config/types.ts: ModelProviderConfig)
// ============================================================================

struct ModelProviderConfig {
    std::optional<std::string> api_key;
    std::optional<std::string> api;
    std::optional<std::string> base_url;
    std::optional<std::string> auth;  // "api-key" | "aws-sdk" | "oauth" | "token"
    std::optional<bool> auth_header;
    std::vector<ModelDefinitionConfig> models;
    std::map<std::string, std::string> headers;
};

// ============================================================================
// Models Config (对齐 models-config.ts)
// ============================================================================

struct ModelsConfig {
    std::map<std::string, ModelProviderConfig> providers;
};

/// 对齐: ensureModelsJson — ensure models.json exists in agent dir
bool ensure_models_json(const core::Config& cfg, const std::string& agent_dir);

/// 对齐: loadModelsConfig — load models.json into ModelsConfig
std::optional<ModelsConfig> load_models_config(const std::string& models_json_path);

/// 对齐: writeModelsConfig — write ModelsConfig to models.json
bool write_models_config(const std::string& models_json_path, const ModelsConfig& config);

// ============================================================================
// Provider Discovery (对齐 models-config.providers.implicit.ts)
// ============================================================================

struct DiscoveredProviderConfig {
    std::string provider_id;
    std::string api;
    std::string base_url;
    std::optional<std::string> api_key;
    std::vector<ModelDefinitionConfig> models;
};

/// 对齐: resolveImplicitProviders — discover providers from env vars
std::vector<DiscoveredProviderConfig> resolve_implicit_providers();

// ============================================================================
// Provider Normalization (对齐 models-config.providers.normalize.ts)
// ============================================================================

/// 对齐: normalizeProviderConfigKeys — normalize provider IDs in config
ModelsConfig normalize_provider_config_keys(const ModelsConfig& config);

// ============================================================================
// Config Merging (对齐 models-config.merge.ts)
// ============================================================================

/// 对齐: mergeModelsConfig — deep merge two configs (source overrides target)
ModelsConfig merge_models_config(const ModelsConfig& target, const ModelsConfig& source);

// ============================================================================
// Static Providers (对齐 models-config.providers.static.ts)
// ============================================================================

/// 对齐: resolveStaticProviderDefaults — built-in provider defaults
std::map<std::string, ModelProviderConfig> resolve_static_provider_defaults();

// ============================================================================
// Config Plan (对齐 models-config.plan.ts)
// ============================================================================

struct ModelsConfigPlan {
    std::vector<std::string> providers_to_add;
    std::vector<std::string> providers_to_update;
    std::vector<std::string> providers_to_remove;
    bool needs_write = false;
};

/// 对齐: computeModelsConfigPlan
ModelsConfigPlan compute_models_config_plan(
    const ModelsConfig& current,
    const ModelsConfig& desired);

} // namespace llm
} // namespace hunterclaw
