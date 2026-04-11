#pragma once
// ============================================================================
// model_selection.h
// 严格对齐 OpenClaw: model-selection.ts (899行)
//                   + model-selection-normalize.ts (80行)
//                   + provider-id.ts (61行)
//                   + model-ref-profile.ts (40行)
//                   + model-ref-shared.ts (80行)
// ============================================================================

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace llm {

// ============================================================================
// ThinkLevel (对齐 model-selection.ts:45)
// ============================================================================
enum class ThinkLevel {
    Off,
    Minimal,
    Low,
    Medium,
    High,
    XHigh,
    Adaptive
};

std::string think_level_to_string(ThinkLevel level);
std::optional<ThinkLevel> think_level_from_string(const std::string& s);

// ============================================================================
// ModelRef (对齐 model-selection-normalize.ts:ModelRef)
// ============================================================================
struct ModelRef {
    std::string provider;
    std::string model;
    
    bool operator==(const ModelRef& other) const {
        return provider == other.provider && model == other.model;
    }
    bool operator!=(const ModelRef& other) const { return !(*this == other); }
};

// ============================================================================
// Provider ID Normalization (对齐 provider-id.ts:3-28)
// ============================================================================

/// Normalize provider ID to canonical form.
/// 对齐: normalizeProviderId — modelstudio→qwen, z.ai→zai, bedrock→amazon-bedrock, etc.
std::string normalize_provider_id(const std::string& provider);

/// 对齐: normalizeProviderIdForAuth
std::string normalize_provider_id_for_auth(const std::string& provider);

/// 对齐: findNormalizedProviderValue
std::optional<std::string> find_normalized_provider_value(
    const std::map<std::string, std::string>& entries,
    const std::string& provider);

/// 对齐: findNormalizedProviderKey
std::optional<std::string> find_normalized_provider_key(
    const std::map<std::string, std::string>& entries,
    const std::string& provider);

// ============================================================================
// ModelRef Parsing (对齐 model-selection-normalize.ts)
// ============================================================================

/// 对齐: parseModelRef(raw, defaultProvider, opts?)
std::optional<ModelRef> parse_model_ref(
    const std::string& raw,
    const std::string& default_provider);

/// 对齐: normalizeModelRef(provider, model) → ModelRef
ModelRef normalize_model_ref(const std::string& provider, const std::string& model);

/// 对齐: modelKey(provider, model) → "provider::model" (lowercase)
std::string model_key(const std::string& provider, const std::string& model);

/// 对齐: legacyModelKey — "provider/model"
std::string legacy_model_key(const std::string& provider, const std::string& model);

// ============================================================================
// Model Ref Profile (对齐 model-ref-profile.ts)
// ============================================================================

struct SplitAuthProfileResult {
    std::string model;
    std::optional<std::string> profile;
};

/// 对齐: splitTrailingAuthProfile("model@profile") → {model, profile}
SplitAuthProfileResult split_trailing_auth_profile(const std::string& raw);

// ============================================================================
// Model Alias Index (对齐 model-selection.ts:47-50)
// ============================================================================

struct ModelAliasEntry {
    std::string alias;
    ModelRef ref;
};

struct ModelAliasIndex {
    std::map<std::string, ModelAliasEntry> by_alias;  // lowercase alias → entry
    std::map<std::string, std::vector<std::string>> by_key;  // model_key → aliases
};

/// 对齐: buildModelAliasIndex
ModelAliasIndex build_model_alias_index(
    const core::Config& cfg,
    const std::string& default_provider);

// ============================================================================
// Model Selection (对齐 model-selection.ts 核心函数)
// ============================================================================

/// 对齐: resolveModelRefFromString
struct ResolvedModelRefResult {
    ModelRef ref;
    std::optional<std::string> alias;
};

std::optional<ResolvedModelRefResult> resolve_model_ref_from_string(
    const std::string& raw,
    const std::string& default_provider,
    const ModelAliasIndex* alias_index = nullptr);

/// 对齐: resolveConfiguredModelRef
ModelRef resolve_configured_model_ref(
    const core::Config& cfg,
    const std::string& default_provider,
    const std::string& default_model);

/// 对齐: resolveDefaultModelForAgent
ModelRef resolve_default_model_for_agent(
    const core::Config& cfg,
    const std::string& agent_id = "");

/// 对齐: resolvePersistedOverrideModelRef
std::optional<ModelRef> resolve_persisted_override_model_ref(
    const std::string& default_provider,
    const std::optional<std::string>& override_provider,
    const std::optional<std::string>& override_model);

/// 对齐: resolvePersistedModelRef
std::optional<ModelRef> resolve_persisted_model_ref(
    const std::string& default_provider,
    const std::optional<std::string>& runtime_provider,
    const std::optional<std::string>& runtime_model,
    const std::optional<std::string>& override_provider,
    const std::optional<std::string>& override_model);

/// 对齐: resolvePersistedSelectedModelRef
std::optional<ModelRef> resolve_persisted_selected_model_ref(
    const std::string& default_provider,
    const std::optional<std::string>& runtime_provider,
    const std::optional<std::string>& runtime_model,
    const std::optional<std::string>& override_provider,
    const std::optional<std::string>& override_model);

// ============================================================================
// Allowed Model Set (对齐 model-selection.ts:564-657)
// ============================================================================

struct ModelCatalogEntry;  // Forward declaration from model_catalog.h

struct AllowedModelSet {
    bool allow_any = true;
    std::vector<std::string> allowed_keys;
    std::set<std::string> allowed_key_set;
};

/// 对齐: buildConfiguredAllowlistKeys
std::optional<std::set<std::string>> build_configured_allowlist_keys(
    const core::Config& cfg,
    const std::string& default_provider);

/// 对齐: resolveAllowlistModelKey
std::optional<std::string> resolve_allowlist_model_key(
    const std::string& raw,
    const std::string& default_provider);

// ============================================================================
// Model Ref Status (对齐 model-selection.ts:697-724)
// ============================================================================

struct ModelRefStatus {
    std::string key;
    bool in_catalog = false;
    bool allow_any = false;
    bool allowed = false;
};

// ============================================================================
// Thinking / Reasoning Defaults (对齐 model-selection.ts:779-851)
// ============================================================================

/// 对齐: resolveThinkingDefault
ThinkLevel resolve_thinking_default(
    const core::Config& cfg,
    const std::string& provider,
    const std::string& model);

/// 对齐: resolveReasoningDefault
bool resolve_reasoning_default(
    const std::string& provider,
    const std::string& model);

/// 对齐: normalizeModelSelection
std::optional<std::string> normalize_model_selection(const std::string& value);

// ============================================================================
// Default Constants (对齐 defaults.ts)
// ============================================================================

extern const std::string DEFAULT_PROVIDER;  // "anthropic"
extern const std::string DEFAULT_MODEL;     // "claude-sonnet-4-20250514"

} // namespace llm
} // namespace hunterclaw
