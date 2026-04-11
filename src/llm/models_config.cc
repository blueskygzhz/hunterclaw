// ============================================================================
// models_config.cc
// 严格对齐 OpenClaw: models-config.ts 系列
// ============================================================================

#include "hunterclaw/llm/models_config.h"
#include "hunterclaw/llm/model_auth.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>

using json = nlohmann::json;

namespace hunterclaw {
namespace llm {

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::optional<std::string> get_env(const std::string& name) {
    const char* val = std::getenv(name.c_str());
    if (!val || val[0] == '\0') return std::nullopt;
    auto trimmed = trim(val);
    return trimmed.empty() ? std::nullopt : std::optional(trimmed);
}

ModelDefinitionConfig parse_model_definition(const json& j) {
    ModelDefinitionConfig m;
    m.id = j.value("id", "");
    if (j.contains("name")) m.name = j["name"].get<std::string>();
    if (j.contains("api")) m.api = j["api"].get<std::string>();
    if (j.contains("baseUrl")) m.base_url = j["baseUrl"].get<std::string>();
    if (j.contains("contextWindow") && j["contextWindow"].is_number()) {
        m.context_window = j["contextWindow"].get<int>();
    }
    if (j.contains("maxTokens") && j["maxTokens"].is_number()) {
        m.max_tokens = j["maxTokens"].get<int>();
    }
    if (j.contains("reasoning") && j["reasoning"].is_boolean()) {
        m.reasoning = j["reasoning"].get<bool>();
    }
    if (j.contains("input") && j["input"].is_array()) {
        for (const auto& item : j["input"]) {
            if (item.is_string()) m.input.push_back(item.get<std::string>());
        }
    }
    if (j.contains("headers") && j["headers"].is_object()) {
        for (auto it = j["headers"].begin(); it != j["headers"].end(); ++it) {
            if (it.value().is_string()) {
                m.headers[it.key()] = it.value().get<std::string>();
            }
        }
    }
    return m;
}

ModelProviderConfig parse_provider_config(const json& j) {
    ModelProviderConfig p;
    if (j.contains("apiKey") && j["apiKey"].is_string()) p.api_key = j["apiKey"].get<std::string>();
    if (j.contains("api") && j["api"].is_string()) p.api = j["api"].get<std::string>();
    if (j.contains("baseUrl") && j["baseUrl"].is_string()) p.base_url = j["baseUrl"].get<std::string>();
    if (j.contains("auth") && j["auth"].is_string()) p.auth = j["auth"].get<std::string>();
    if (j.contains("authHeader") && j["authHeader"].is_boolean()) p.auth_header = j["authHeader"].get<bool>();
    if (j.contains("models") && j["models"].is_array()) {
        for (const auto& m : j["models"]) {
            p.models.push_back(parse_model_definition(m));
        }
    }
    if (j.contains("headers") && j["headers"].is_object()) {
        for (auto it = j["headers"].begin(); it != j["headers"].end(); ++it) {
            if (it.value().is_string()) {
                p.headers[it.key()] = it.value().get<std::string>();
            }
        }
    }
    return p;
}

} // namespace

// ============================================================================
// Load/Write (对齐 models-config.ts)
// ============================================================================

bool ensure_models_json(const core::Config& cfg, const std::string& agent_dir) {
    (void)cfg;
    (void)agent_dir;
    // In C++, models.json is ensured by the daemon/gateway startup
    return true;
}

std::optional<ModelsConfig> load_models_config(const std::string& models_json_path) {
    try {
        std::ifstream file(models_json_path);
        if (!file.is_open()) return std::nullopt;
        
        json j;
        file >> j;
        
        ModelsConfig config;
        if (j.contains("providers") && j["providers"].is_object()) {
            for (auto it = j["providers"].begin(); it != j["providers"].end(); ++it) {
                config.providers[it.key()] = parse_provider_config(it.value());
            }
        }
        return config;
    } catch (...) {
        return std::nullopt;
    }
}

bool write_models_config(const std::string& models_json_path, const ModelsConfig& config) {
    try {
        json j;
        json providers = json::object();
        for (const auto& [id, provider] : config.providers) {
            json p;
            if (provider.api_key) p["apiKey"] = *provider.api_key;
            if (provider.api) p["api"] = *provider.api;
            if (provider.base_url) p["baseUrl"] = *provider.base_url;
            if (provider.auth) p["auth"] = *provider.auth;
            if (provider.auth_header) p["authHeader"] = *provider.auth_header;
            if (!provider.models.empty()) {
                json models = json::array();
                for (const auto& m : provider.models) {
                    json mj;
                    mj["id"] = m.id;
                    if (m.name) mj["name"] = *m.name;
                    if (m.api) mj["api"] = *m.api;
                    if (m.context_window) mj["contextWindow"] = *m.context_window;
                    if (m.max_tokens) mj["maxTokens"] = *m.max_tokens;
                    if (m.reasoning) mj["reasoning"] = *m.reasoning;
                    if (!m.input.empty()) {
                        mj["input"] = json(m.input);
                    }
                    models.push_back(mj);
                }
                p["models"] = models;
            }
            if (!provider.headers.empty()) {
                p["headers"] = json(provider.headers);
            }
            providers[id] = p;
        }
        j["providers"] = providers;
        
        std::ofstream file(models_json_path);
        if (!file.is_open()) return false;
        file << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Implicit Provider Discovery (对齐 models-config.providers.implicit.ts)
// ============================================================================

std::vector<DiscoveredProviderConfig> resolve_implicit_providers() {
    std::vector<DiscoveredProviderConfig> providers;
    
    auto candidates = resolve_provider_env_api_key_candidates();
    
    // 对齐 OC: resolveImplicitProviders — check env vars for each provider
    struct ImplicitProviderDef {
        std::string provider_id;
        std::string api;
        std::string base_url;
    };
    
    const std::vector<ImplicitProviderDef> implicit_defs = {
        {"anthropic", "anthropic-messages", "https://api.anthropic.com/v1"},
        {"openai", "openai-responses", "https://api.openai.com/v1"},
        {"google", "google-generative-ai", "https://generativelanguage.googleapis.com"},
        {"deepseek", "openai-completions", "https://api.deepseek.com"},
        {"openrouter", "openai-completions", "https://openrouter.ai/api/v1"},
        {"groq", "openai-completions", "https://api.groq.com/openai/v1"},
        {"mistral", "openai-completions", "https://api.mistral.ai/v1"},
        {"xai", "openai-completions", "https://api.x.ai/v1"},
        {"together", "openai-completions", "https://api.together.xyz/v1"},
        {"cerebras", "openai-completions", "https://api.cerebras.ai/v1"},
        {"perplexity", "openai-completions", "https://api.perplexity.ai"},
        {"fireworks", "openai-completions", "https://api.fireworks.ai/inference/v1"},
    };
    
    for (const auto& def : implicit_defs) {
        auto it = candidates.find(def.provider_id);
        if (it == candidates.end()) continue;
        
        bool has_key = false;
        std::string found_key;
        for (const auto& env_var : it->second) {
            auto val = get_env(env_var);
            if (val) {
                has_key = true;
                found_key = *val;
                break;
            }
        }
        if (!has_key) continue;
        
        providers.push_back(DiscoveredProviderConfig{
            def.provider_id,
            def.api,
            def.base_url,
            found_key,
            {}  // models discovered separately
        });
    }
    
    // Ollama: always include (local, no auth needed)
    providers.push_back(DiscoveredProviderConfig{
        "ollama",
        "openai-completions",
        get_env("OLLAMA_BASE_URL").value_or("http://127.0.0.1:11434"),
        std::nullopt,
        {}
    });
    
    return providers;
}

// ============================================================================
// Provider Normalization (对齐 models-config.providers.normalize.ts)
// ============================================================================

ModelsConfig normalize_provider_config_keys(const ModelsConfig& config) {
    ModelsConfig normalized;
    for (const auto& [id, provider] : config.providers) {
        auto norm_id = normalize_provider_id(id);
        normalized.providers[norm_id] = provider;
    }
    return normalized;
}

// ============================================================================
// Config Merging (对齐 models-config.merge.ts)
// ============================================================================

ModelsConfig merge_models_config(const ModelsConfig& target, const ModelsConfig& source) {
    ModelsConfig merged = target;
    
    for (const auto& [id, source_provider] : source.providers) {
        auto it = merged.providers.find(id);
        if (it == merged.providers.end()) {
            // New provider from source
            merged.providers[id] = source_provider;
        } else {
            // Merge: source overrides target fields
            auto& existing = it->second;
            if (source_provider.api_key) existing.api_key = source_provider.api_key;
            if (source_provider.api) existing.api = source_provider.api;
            if (source_provider.base_url) existing.base_url = source_provider.base_url;
            if (source_provider.auth) existing.auth = source_provider.auth;
            if (source_provider.auth_header) existing.auth_header = source_provider.auth_header;
            
            // Merge models (source models override by ID)
            if (!source_provider.models.empty()) {
                std::map<std::string, ModelDefinitionConfig> model_map;
                for (const auto& m : existing.models) {
                    model_map[m.id] = m;
                }
                for (const auto& m : source_provider.models) {
                    model_map[m.id] = m;
                }
                existing.models.clear();
                for (const auto& [_, m] : model_map) {
                    existing.models.push_back(m);
                }
            }
            
            // Merge headers
            for (const auto& [k, v] : source_provider.headers) {
                existing.headers[k] = v;
            }
        }
    }
    
    return merged;
}

// ============================================================================
// Static Provider Defaults (对齐 models-config.providers.static.ts)
// ============================================================================

std::map<std::string, ModelProviderConfig> resolve_static_provider_defaults() {
    std::map<std::string, ModelProviderConfig> defaults;
    
    // Anthropic
    defaults["anthropic"] = ModelProviderConfig{
        std::nullopt, "anthropic-messages", "https://api.anthropic.com/v1"
    };
    // OpenAI
    defaults["openai"] = ModelProviderConfig{
        std::nullopt, "openai-responses", "https://api.openai.com/v1"
    };
    // Google
    defaults["google"] = ModelProviderConfig{
        std::nullopt, "google-generative-ai", "https://generativelanguage.googleapis.com"
    };
    // Ollama
    defaults["ollama"] = ModelProviderConfig{
        OLLAMA_LOCAL_AUTH_MARKER, "openai-completions", "http://127.0.0.1:11434"
    };
    
    return defaults;
}

// ============================================================================
// Config Plan (对齐 models-config.plan.ts)
// ============================================================================

ModelsConfigPlan compute_models_config_plan(
    const ModelsConfig& current,
    const ModelsConfig& desired
) {
    ModelsConfigPlan plan;
    
    std::set<std::string> current_ids;
    for (const auto& [id, _] : current.providers) {
        current_ids.insert(id);
    }
    
    for (const auto& [id, _] : desired.providers) {
        if (current_ids.count(id) == 0) {
            plan.providers_to_add.push_back(id);
            plan.needs_write = true;
        } else {
            // TODO: deep comparison for updates
            plan.providers_to_update.push_back(id);
        }
    }
    
    for (const auto& [id, _] : current.providers) {
        if (desired.providers.count(id) == 0) {
            plan.providers_to_remove.push_back(id);
            plan.needs_write = true;
        }
    }
    
    return plan;
}

} // namespace llm
} // namespace hunterclaw
