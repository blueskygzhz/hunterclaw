// ============================================================================
// model_selection.cc
// 严格对齐 OpenClaw: model-selection.ts (899行)
//                   + model-selection-normalize.ts (80行)
//                   + provider-id.ts (61行)
// ============================================================================

#include "hunterclaw/llm/model_selection.h"
#include "hunterclaw/react/model_catalog.h"  // ✅ 对齐 OC: import { loadModelCatalog } from "../agents/model-catalog.js"
#include <algorithm>
#include <sstream>
#include <cctype>

namespace hunterclaw {
namespace llm {

// ============================================================================
// Default Constants
// ============================================================================

const std::string DEFAULT_PROVIDER = "anthropic";
const std::string DEFAULT_MODEL = "claude-sonnet-4-20250514";

// ============================================================================
// Utility
// ============================================================================

namespace {

std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::string normalize_lowercase(const std::string& s) {
    return to_lower(trim(s));
}

} // namespace

// ============================================================================
// ThinkLevel
// ============================================================================

std::string think_level_to_string(ThinkLevel level) {
    switch (level) {
        case ThinkLevel::Off:      return "off";
        case ThinkLevel::Minimal:  return "minimal";
        case ThinkLevel::Low:      return "low";
        case ThinkLevel::Medium:   return "medium";
        case ThinkLevel::High:     return "high";
        case ThinkLevel::XHigh:    return "xhigh";
        case ThinkLevel::Adaptive: return "adaptive";
    }
    return "off";
}

std::optional<ThinkLevel> think_level_from_string(const std::string& s) {
    auto lower = to_lower(trim(s));
    if (lower == "off")      return ThinkLevel::Off;
    if (lower == "minimal")  return ThinkLevel::Minimal;
    if (lower == "low")      return ThinkLevel::Low;
    if (lower == "medium")   return ThinkLevel::Medium;
    if (lower == "high")     return ThinkLevel::High;
    if (lower == "xhigh")    return ThinkLevel::XHigh;
    if (lower == "adaptive") return ThinkLevel::Adaptive;
    return std::nullopt;
}

// ============================================================================
// Provider ID Normalization (对齐 provider-id.ts:3-28)
// ============================================================================

std::string normalize_provider_id(const std::string& provider) {
    auto normalized = normalize_lowercase(provider);
    if (normalized.empty()) return normalized;
    
    // 对齐 OC: modelstudio/qwencloud → qwen
    if (normalized == "modelstudio" || normalized == "qwencloud") return "qwen";
    // 对齐 OC: z.ai/z-ai → zai
    if (normalized == "z.ai" || normalized == "z-ai") return "zai";
    // 对齐 OC: opencode-zen → opencode
    if (normalized == "opencode-zen") return "opencode";
    // 对齐 OC: opencode-go-auth → opencode-go
    if (normalized == "opencode-go-auth") return "opencode-go";
    // 对齐 OC: kimi/kimi-code/kimi-coding → kimi
    if (normalized == "kimi" || normalized == "kimi-code" || normalized == "kimi-coding") return "kimi";
    // 对齐 OC: bedrock/aws-bedrock → amazon-bedrock
    if (normalized == "bedrock" || normalized == "aws-bedrock") return "amazon-bedrock";
    // 对齐 OC: bytedance/doubao → volcengine
    if (normalized == "bytedance" || normalized == "doubao") return "volcengine";
    
    return normalized;
}

std::string normalize_provider_id_for_auth(const std::string& provider) {
    return normalize_provider_id(provider);
}

std::optional<std::string> find_normalized_provider_value(
    const std::map<std::string, std::string>& entries,
    const std::string& provider
) {
    auto provider_key = normalize_provider_id(provider);
    for (const auto& [key, value] : entries) {
        if (normalize_provider_id(key) == provider_key) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> find_normalized_provider_key(
    const std::map<std::string, std::string>& entries,
    const std::string& provider
) {
    auto provider_key = normalize_provider_id(provider);
    for (const auto& [key, _] : entries) {
        if (normalize_provider_id(key) == provider_key) {
            return key;
        }
    }
    return std::nullopt;
}

// ============================================================================
// ModelRef Parsing (对齐 model-selection-normalize.ts)
// ============================================================================

std::optional<ModelRef> parse_model_ref(
    const std::string& raw,
    const std::string& default_provider
) {
    auto trimmed = trim(raw);
    if (trimmed.empty()) return std::nullopt;
    
    // 对齐 OC: split on first "/"
    auto slash_pos = trimmed.find('/');
    if (slash_pos == std::string::npos) {
        // No provider prefix — use default
        return ModelRef{normalize_provider_id(default_provider), trimmed};
    }
    
    auto provider_raw = trimmed.substr(0, slash_pos);
    auto model_raw = trimmed.substr(slash_pos + 1);
    
    if (provider_raw.empty() || model_raw.empty()) {
        return std::nullopt;
    }
    
    return ModelRef{normalize_provider_id(provider_raw), trim(model_raw)};
}

ModelRef normalize_model_ref(const std::string& provider, const std::string& model) {
    return ModelRef{normalize_provider_id(provider), trim(model)};
}

std::string model_key(const std::string& provider, const std::string& model) {
    // 对齐 OC: modelKey → "provider::model" (lowercase)
    return normalize_lowercase(provider) + "::" + normalize_lowercase(model);
}

std::string legacy_model_key(const std::string& provider, const std::string& model) {
    // 对齐 OC: legacyModelKey → "provider/model"
    return normalize_lowercase(provider) + "/" + normalize_lowercase(model);
}

// ============================================================================
// Model Ref Profile (对齐 model-ref-profile.ts)
// ============================================================================

SplitAuthProfileResult split_trailing_auth_profile(const std::string& raw) {
    auto trimmed = trim(raw);
    if (trimmed.empty()) return {"", std::nullopt};
    
    // 对齐 OC: split on last "@" that isn't at position 0 or at end
    auto at_pos = trimmed.rfind('@');
    if (at_pos == std::string::npos || at_pos == 0 || at_pos == trimmed.size() - 1) {
        return {trimmed, std::nullopt};
    }
    
    return {
        trimmed.substr(0, at_pos),
        trimmed.substr(at_pos + 1)
    };
}

// ============================================================================
// Model Alias Index (对齐 model-selection.ts:286-317)
// ============================================================================

ModelAliasIndex build_model_alias_index(
    const core::Config& cfg,
    const std::string& default_provider
) {
    ModelAliasIndex index;
    
    // 对齐 OC: read cfg.agents.defaults.models
    if (!cfg.data.contains("agents")) return index;
    const auto& agents = cfg.data["agents"];
    if (!agents.contains("defaults")) return index;
    const auto& defaults = agents["defaults"];
    if (!defaults.contains("models")) return index;
    const auto& models = defaults["models"];
    if (!models.is_object()) return index;
    
    for (auto it = models.begin(); it != models.end(); ++it) {
        auto key_raw = it.key();
        auto parsed = parse_model_ref(key_raw, default_provider);
        if (!parsed) continue;
        
        std::string alias;
        if (it.value().is_object() && it.value().contains("alias")) {
            alias = trim(it.value()["alias"].get<std::string>());
        }
        if (alias.empty()) continue;
        
        auto alias_key = normalize_lowercase(alias);
        index.by_alias[alias_key] = ModelAliasEntry{alias, *parsed};
        
        auto mk = model_key(parsed->provider, parsed->model);
        index.by_key[mk].push_back(alias);
    }
    
    return index;
}

// ============================================================================
// Model Selection Core (对齐 model-selection.ts)
// ============================================================================

std::optional<ResolvedModelRefResult> resolve_model_ref_from_string(
    const std::string& raw,
    const std::string& default_provider,
    const ModelAliasIndex* alias_index
) {
    auto split = split_trailing_auth_profile(raw);
    auto model_str = trim(split.model);
    if (model_str.empty()) return std::nullopt;
    
    // 对齐 OC: if no "/" in model, try alias lookup
    if (model_str.find('/') == std::string::npos && alias_index) {
        auto alias_key = normalize_lowercase(model_str);
        auto it = alias_index->by_alias.find(alias_key);
        if (it != alias_index->by_alias.end()) {
            return ResolvedModelRefResult{it->second.ref, it->second.alias};
        }
    }
    
    auto parsed = parse_model_ref(model_str, default_provider);
    if (!parsed) return std::nullopt;
    
    return ResolvedModelRefResult{*parsed, std::nullopt};
}

ModelRef resolve_configured_model_ref(
    const core::Config& cfg,
    const std::string& default_provider,
    const std::string& default_model
) {
    // 对齐 OC: resolveConfiguredModelRef
    // Read agents.defaults.model (can be string or {primary: string})
    std::string raw_model;
    if (cfg.data.contains("agents")) {
        const auto& agents = cfg.data["agents"];
        if (agents.contains("defaults")) {
            const auto& defaults = agents["defaults"];
            if (defaults.contains("model")) {
                const auto& model_val = defaults["model"];
                if (model_val.is_string()) {
                    raw_model = trim(model_val.get<std::string>());
                } else if (model_val.is_object() && model_val.contains("primary")) {
                    const auto& primary = model_val["primary"];
                    if (primary.is_string()) {
                        raw_model = trim(primary.get<std::string>());
                    }
                }
            }
        }
    }
    
    if (!raw_model.empty()) {
        auto alias_index = build_model_alias_index(cfg, default_provider);
        
        // 对齐 OC: if no "/" in model, try alias first
        if (raw_model.find('/') == std::string::npos) {
            auto alias_key = normalize_lowercase(raw_model);
            auto it = alias_index.by_alias.find(alias_key);
            if (it != alias_index.by_alias.end()) {
                return it->second.ref;
            }
            // Fallback: default provider + bare model
            return ModelRef{default_provider, raw_model};
        }
        
        auto resolved = resolve_model_ref_from_string(raw_model, default_provider, &alias_index);
        if (resolved) return resolved->ref;
    }
    
    return ModelRef{default_provider, default_model};
}

ModelRef resolve_default_model_for_agent(
    const core::Config& cfg,
    const std::string& agent_id
) {
    // ✅ 对齐 OC: resolveDefaultModelForAgent + resolveAgentEffectiveModelPrimary
    // Check agent-specific model override first
    if (!agent_id.empty() && agent_id != "main" && cfg.data.contains("agents")) {
        const auto& agents = cfg.data["agents"];
        if (agents.contains("entries") && agents["entries"].is_object() &&
            agents["entries"].contains(agent_id)) {
            const auto& agent_cfg = agents["entries"][agent_id];
            if (agent_cfg.is_object() && agent_cfg.contains("model")) {
                const auto& model_val = agent_cfg["model"];
                std::string raw_model;
                if (model_val.is_string()) {
                    raw_model = trim(model_val.get<std::string>());
                } else if (model_val.is_object() && model_val.contains("primary")) {
                    raw_model = trim(model_val["primary"].get<std::string>());
                }
                if (!raw_model.empty()) {
                    auto alias_index = build_model_alias_index(cfg, DEFAULT_PROVIDER);
                    auto resolved = resolve_model_ref_from_string(
                        raw_model, DEFAULT_PROVIDER, &alias_index);
                    if (resolved) return resolved->ref;
                    // Bare model name
                    auto parsed = parse_model_ref(raw_model, DEFAULT_PROVIDER);
                    if (parsed) return *parsed;
                }
            }
        }
    }
    // Fallback to global default
    return resolve_configured_model_ref(cfg, DEFAULT_PROVIDER, DEFAULT_MODEL);
}

std::optional<ModelRef> resolve_persisted_override_model_ref(
    const std::string& default_provider,
    const std::optional<std::string>& override_provider,
    const std::optional<std::string>& override_model
) {
    auto model = override_model ? trim(*override_model) : "";
    if (model.empty()) return std::nullopt;
    
    auto provider = override_provider ? trim(*override_provider) : "";
    auto encoded = provider.empty() ? model : (provider + "/" + model);
    
    auto parsed = parse_model_ref(encoded, default_provider);
    if (parsed) return parsed;
    
    return ModelRef{
        provider.empty() ? default_provider : provider,
        model
    };
}

std::optional<ModelRef> resolve_persisted_model_ref(
    const std::string& default_provider,
    const std::optional<std::string>& runtime_provider,
    const std::optional<std::string>& runtime_model,
    const std::optional<std::string>& override_provider,
    const std::optional<std::string>& override_model
) {
    auto rt_model = runtime_model ? trim(*runtime_model) : "";
    auto rt_provider = runtime_provider ? trim(*runtime_provider) : "";
    
    if (!rt_model.empty()) {
        if (!rt_provider.empty()) {
            return ModelRef{rt_provider, rt_model};
        }
        auto parsed = parse_model_ref(rt_model, default_provider);
        if (parsed) return parsed;
        return ModelRef{default_provider, rt_model};
    }
    
    return resolve_persisted_override_model_ref(
        default_provider, override_provider, override_model);
}

std::optional<ModelRef> resolve_persisted_selected_model_ref(
    const std::string& default_provider,
    const std::optional<std::string>& runtime_provider,
    const std::optional<std::string>& runtime_model,
    const std::optional<std::string>& override_provider,
    const std::optional<std::string>& override_model
) {
    auto override_ref = resolve_persisted_override_model_ref(
        default_provider, override_provider, override_model);
    if (override_ref) return override_ref;
    
    return resolve_persisted_model_ref(
        default_provider, runtime_provider, runtime_model,
        override_provider, override_model);
}

// ============================================================================
// Allowlist (对齐 model-selection.ts:267-284)
// ============================================================================

std::optional<std::string> resolve_allowlist_model_key(
    const std::string& raw,
    const std::string& default_provider
) {
    auto parsed = parse_model_ref(raw, default_provider);
    if (!parsed) return std::nullopt;
    return model_key(parsed->provider, parsed->model);
}

std::optional<std::set<std::string>> build_configured_allowlist_keys(
    const core::Config& cfg,
    const std::string& default_provider
) {
    // 对齐 OC: buildConfiguredAllowlistKeys
    if (!cfg.data.contains("agents")) return std::nullopt;
    const auto& agents = cfg.data["agents"];
    if (!agents.contains("defaults")) return std::nullopt;
    const auto& defaults = agents["defaults"];
    if (!defaults.contains("models") || !defaults["models"].is_object()) return std::nullopt;
    
    const auto& models = defaults["models"];
    if (models.empty()) return std::nullopt;
    
    std::set<std::string> keys;
    for (auto it = models.begin(); it != models.end(); ++it) {
        auto key = resolve_allowlist_model_key(it.key(), default_provider);
        if (key) keys.insert(*key);
    }
    
    return keys.empty() ? std::nullopt : std::optional(keys);
}

// ============================================================================
// Thinking / Reasoning Defaults (对齐 model-selection.ts:779-851)
// ============================================================================

ThinkLevel resolve_thinking_default(
    const core::Config& cfg,
    const std::string& provider,
    const std::string& model
) {
    // 对齐 OC: resolveThinkingDefault
    auto normalized_provider = normalize_provider_id(provider);
    
    // Check per-model thinking config
    auto mk = model_key(provider, model);
    if (cfg.data.contains("agents")) {
        const auto& agents = cfg.data["agents"];
        if (agents.contains("defaults")) {
            const auto& defaults = agents["defaults"];
            // Check thinkingDefault
            if (defaults.contains("thinkingDefault")) {
                auto level = think_level_from_string(
                    defaults["thinkingDefault"].get<std::string>());
                if (level) return *level;
            }
            // Check per-model params.thinking
            if (defaults.contains("models") && defaults["models"].is_object()) {
                const auto& models = defaults["models"];
                if (models.contains(mk)) {
                    const auto& entry = models[mk];
                    if (entry.is_object() && entry.contains("params") &&
                        entry["params"].is_object() && entry["params"].contains("thinking")) {
                        auto level = think_level_from_string(
                            entry["params"]["thinking"].get<std::string>());
                        if (level) return *level;
                    }
                }
            }
        }
    }
    
    // 对齐 OC: resolveThinkingDefaultForModel
    // Anthropic reasoning models default to "medium"
    if (normalized_provider == "anthropic") {
        auto lower_model = to_lower(model);
        // Claude Opus/Sonnet 4+ with extended thinking → medium
        if (lower_model.find("claude-opus-4") != std::string::npos ||
            lower_model.find("claude-sonnet-4") != std::string::npos ||
            lower_model.find("claude-3-7-sonnet") != std::string::npos) {
            return ThinkLevel::Medium;
        }
    }
    // OpenAI o-series → high
    if (normalized_provider == "openai") {
        auto lower_model = to_lower(model);
        if (lower_model.find("o3") != std::string::npos ||
            lower_model.find("o4") != std::string::npos) {
            return ThinkLevel::High;
        }
    }
    // Google Gemini thinking models
    if (normalized_provider == "google") {
        auto lower_model = to_lower(model);
        if (lower_model.find("thinking") != std::string::npos ||
            lower_model.find("gemini-2.5") != std::string::npos) {
            return ThinkLevel::Medium;
        }
    }
    
    return ThinkLevel::Off;
}

bool resolve_reasoning_default(
    const std::string& provider,
    const std::string& model
) {
    // ✅ 对齐 OC: resolveReasoningDefault → loadModelCatalog() → entry.reasoning
    // 优先查 ModelCatalog，命中则使用 entry.reasoning
    auto& catalog = react::ModelCatalog::instance();
    auto entry = catalog.find_model(provider, model);
    if (entry.has_value()) {
        return entry->reasoning.value_or(false);
    }

    // Fallback: 字符串匹配 (对齐 OC 兜底逻辑)
    auto lp = to_lower(provider);
    auto lm = to_lower(model);
    
    if (lp == "anthropic") {
        if (lm.find("opus") != std::string::npos || lm.find("sonnet-4") != std::string::npos ||
            lm.find("3-7-sonnet") != std::string::npos) {
            return true;
        }
    }
    if (lp == "openai") {
        if (lm.find("o3") != std::string::npos || lm.find("o4") != std::string::npos) {
            return true;
        }
    }
    if (lp == "google") {
        if (lm.find("thinking") != std::string::npos || lm.find("2.5") != std::string::npos) {
            return true;
        }
    }
    if (lp == "deepseek" && lm.find("reasoner") != std::string::npos) {
        return true;
    }
    
    return false;
}

std::optional<std::string> normalize_model_selection(const std::string& value) {
    auto trimmed = trim(value);
    return trimmed.empty() ? std::nullopt : std::optional(trimmed);
}

} // namespace llm
} // namespace hunterclaw
