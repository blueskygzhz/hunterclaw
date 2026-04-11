// ============================================================================
// model_auth.cc
// 严格对齐 OpenClaw: model-auth.ts (737行)
//                   + model-auth-env.ts (68行)
//                   + model-auth-markers.ts (100行)
//                   + model-auth-env-vars.ts
// ============================================================================

#include "hunterclaw/llm/model_auth.h"
#include "hunterclaw/llm/model_selection.h"
#include <algorithm>
#include <cstdlib>

namespace hunterclaw {
namespace llm {

// ============================================================================
// Auth Markers (对齐 model-auth-markers.ts)
// ============================================================================

const std::string OLLAMA_LOCAL_AUTH_MARKER = "ollama-local";
const std::string CUSTOM_LOCAL_AUTH_MARKER = "custom-local";
const std::string GCP_VERTEX_CREDENTIALS_MARKER = "gcp-vertex-credentials";
const std::string NON_ENV_SECRETREF_MARKER = "secretref-managed";
const std::string MINIMAX_OAUTH_MARKER = "minimax-oauth";
const std::string OAUTH_API_KEY_MARKER_PREFIX = "oauth:";

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

std::optional<std::string> get_env(const std::string& name) {
    const char* val = std::getenv(name.c_str());
    if (!val || val[0] == '\0') return std::nullopt;
    auto trimmed = trim(val);
    return trimmed.empty() ? std::nullopt : std::optional(trimmed);
}

const std::set<std::string> AWS_SDK_ENV_MARKERS = {
    "AWS_BEARER_TOKEN_BEDROCK",
    "AWS_ACCESS_KEY_ID",
    "AWS_PROFILE"
};

} // namespace

bool is_non_secret_api_key_marker(const std::string& value) {
    auto trimmed = trim(value);
    if (trimmed.empty()) return false;
    return trimmed == MINIMAX_OAUTH_MARKER ||
           is_oauth_api_key_marker(trimmed) ||
           trimmed == OLLAMA_LOCAL_AUTH_MARKER ||
           trimmed == CUSTOM_LOCAL_AUTH_MARKER ||
           trimmed == GCP_VERTEX_CREDENTIALS_MARKER ||
           trimmed == NON_ENV_SECRETREF_MARKER ||
           is_aws_sdk_auth_marker(trimmed) ||
           is_known_env_api_key_marker(trimmed);
}

bool is_known_env_api_key_marker(const std::string& value) {
    auto trimmed = trim(value);
    auto known = list_known_provider_env_api_key_names();
    return known.count(trimmed) > 0 && !is_aws_sdk_auth_marker(trimmed);
}

bool is_oauth_api_key_marker(const std::string& value) {
    return trim(value).find(OAUTH_API_KEY_MARKER_PREFIX) == 0;
}

bool is_aws_sdk_auth_marker(const std::string& value) {
    return AWS_SDK_ENV_MARKERS.count(trim(value)) > 0;
}

// ============================================================================
// Provider Auth Aliases (对齐 provider-auth-aliases.ts)
// ============================================================================

std::string resolve_provider_id_for_auth(const std::string& provider) {
    return normalize_provider_id(provider);
}

// ============================================================================
// Env API Key (对齐 model-auth-env.ts + model-auth-env-vars.ts)
// ============================================================================

std::map<std::string, std::vector<std::string>> resolve_provider_env_api_key_candidates() {
    // 对齐 OC: resolveProviderEnvApiKeyCandidates
    return {
        {"anthropic",       {"ANTHROPIC_API_KEY"}},
        {"openai",          {"OPENAI_API_KEY"}},
        {"openai-codex",    {"OPENAI_API_KEY"}},
        {"google",          {"GOOGLE_API_KEY", "GEMINI_API_KEY"}},
        {"deepseek",        {"DEEPSEEK_API_KEY"}},
        {"openrouter",      {"OPENROUTER_API_KEY"}},
        {"groq",            {"GROQ_API_KEY"}},
        {"mistral",         {"MISTRAL_API_KEY"}},
        {"xai",             {"XAI_API_KEY"}},
        {"zai",             {"ZAI_API_KEY", "Z_AI_API_KEY"}},
        {"together",        {"TOGETHER_API_KEY"}},
        {"perplexity",      {"PERPLEXITY_API_KEY"}},
        {"fireworks",       {"FIREWORKS_API_KEY"}},
        {"cerebras",        {"CEREBRAS_API_KEY"}},
        {"chutes",          {"CHUTES_API_KEY"}},
        {"kimi",            {"MOONSHOT_API_KEY"}},
        {"moonshot",        {"MOONSHOT_API_KEY"}},
        {"qwen",            {"DASHSCOPE_API_KEY"}},
        {"volcengine",      {"VOLCENGINE_API_KEY", "ARK_API_KEY"}},
        {"novita",          {"NOVITA_API_KEY"}},
        {"azure-openai",    {"AZURE_OPENAI_API_KEY", "AZURE_API_KEY"}},
        {"github-copilot",  {"GITHUB_TOKEN"}},
        {"minimax",         {"MINIMAX_API_KEY", "MINIMAX_CODE_PLAN_KEY"}},
        {"stepfun",         {"STEPFUN_API_KEY"}},
        {"nvidia",          {"NVIDIA_API_KEY"}},
    };
}

std::set<std::string> list_known_provider_env_api_key_names() {
    std::set<std::string> names;
    for (const auto& [_, vars] : resolve_provider_env_api_key_candidates()) {
        for (const auto& v : vars) {
            names.insert(v);
        }
    }
    return names;
}

std::optional<EnvApiKeyResult> resolve_env_api_key(const std::string& provider) {
    auto normalized = resolve_provider_id_for_auth(provider);
    auto candidates = resolve_provider_env_api_key_candidates();
    
    auto it = candidates.find(normalized);
    if (it == candidates.end()) return std::nullopt;
    
    for (const auto& env_var : it->second) {
        auto value = get_env(env_var);
        if (value) {
            return EnvApiKeyResult{*value, "env: " + env_var};
        }
    }
    return std::nullopt;
}

// ============================================================================
// Auth Mode
// ============================================================================

std::string auth_mode_to_string(AuthMode mode) {
    switch (mode) {
        case AuthMode::ApiKey:  return "api-key";
        case AuthMode::OAuth:   return "oauth";
        case AuthMode::Token:   return "token";
        case AuthMode::AwsSdk:  return "aws-sdk";
        case AuthMode::Unknown: return "unknown";
    }
    return "unknown";
}

std::string require_api_key(const ResolvedProviderAuth& auth) {
    if (auth.api_key.empty()) {
        throw std::runtime_error("No API key available");
    }
    return auth.api_key;
}

std::optional<std::string> resolve_aws_sdk_env_var_name(const std::string& provider) {
    auto normalized = normalize_provider_id(provider);
    if (normalized == "amazon-bedrock") {
        if (get_env("AWS_BEARER_TOKEN_BEDROCK")) return "AWS_BEARER_TOKEN_BEDROCK";
        if (get_env("AWS_ACCESS_KEY_ID")) return "AWS_ACCESS_KEY_ID";
        if (get_env("AWS_PROFILE")) return "AWS_PROFILE";
    }
    return std::nullopt;
}

// ============================================================================
// Model Auth Resolution (对齐 model-auth.ts core)
// ============================================================================

std::optional<std::string> get_custom_provider_api_key(
    const core::Config& cfg,
    const std::string& provider
) {
    // 对齐 OC: getCustomProviderApiKey — read cfg.models.providers[provider].apiKey
    if (!cfg.data.contains("models")) return std::nullopt;
    const auto& models = cfg.data["models"];
    if (!models.contains("providers") || !models["providers"].is_object()) return std::nullopt;
    
    auto normalized = normalize_provider_id(provider);
    const auto& providers = models["providers"];
    
    // Direct lookup
    for (auto it = providers.begin(); it != providers.end(); ++it) {
        if (normalize_provider_id(it.key()) == normalized) {
            if (it.value().is_object() && it.value().contains("apiKey")) {
                auto key = trim(it.value()["apiKey"].get<std::string>());
                return key.empty() ? std::nullopt : std::optional(key);
            }
        }
    }
    return std::nullopt;
}

std::optional<ResolvedCustomProviderApiKey> resolve_usable_custom_provider_api_key(
    const core::Config& cfg,
    const std::string& provider
) {
    auto custom_key = get_custom_provider_api_key(cfg, provider);
    if (!custom_key) return std::nullopt;
    
    if (!is_non_secret_api_key_marker(*custom_key)) {
        return ResolvedCustomProviderApiKey{*custom_key, "models.json"};
    }
    if (!is_known_env_api_key_marker(*custom_key)) return std::nullopt;
    
    auto env_value = get_env(*custom_key);
    if (!env_value) return std::nullopt;
    
    return ResolvedCustomProviderApiKey{*env_value, "env: " + *custom_key + " (models.json marker)"};
}

bool has_usable_custom_provider_api_key(
    const core::Config& cfg,
    const std::string& provider
) {
    return resolve_usable_custom_provider_api_key(cfg, provider).has_value();
}

ResolvedProviderAuth resolve_api_key_for_provider(
    const std::string& provider,
    const core::Config& cfg,
    const std::string& profile_id,
    const std::string& preferred_profile
) {
    (void)profile_id;
    (void)preferred_profile;
    
    auto normalized = normalize_provider_id(provider);
    
    // 对齐 OC: resolveApiKeyForProvider — multi-source resolution
    // 1. AWS SDK auth
    if (normalized == "amazon-bedrock") {
        return ResolvedProviderAuth{"", std::nullopt, "aws-sdk default chain", AuthMode::AwsSdk};
    }
    
    // 2. Custom provider API key from models.json
    auto custom = resolve_usable_custom_provider_api_key(cfg, provider);
    if (custom) {
        return ResolvedProviderAuth{custom->api_key, std::nullopt, custom->source, AuthMode::ApiKey};
    }
    
    // 3. Environment variable
    auto env_key = resolve_env_api_key(provider);
    if (env_key) {
        auto mode = env_key->source.find("OAUTH_TOKEN") != std::string::npos
            ? AuthMode::OAuth : AuthMode::ApiKey;
        return ResolvedProviderAuth{env_key->api_key, std::nullopt, env_key->source, mode};
    }
    
    // 4. Ollama local — no auth needed
    if (normalized == "ollama") {
        return ResolvedProviderAuth{OLLAMA_LOCAL_AUTH_MARKER, std::nullopt,
                                     "ollama (local, no auth)", AuthMode::ApiKey};
    }
    
    throw std::runtime_error("No API key found for provider \"" + provider + "\".");
}

bool has_available_auth_for_provider(
    const std::string& provider,
    const core::Config& cfg
) {
    try {
        resolve_api_key_for_provider(provider, cfg);
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Model Auth Mode
// ============================================================================

std::string model_auth_mode_to_string(ModelAuthMode mode) {
    switch (mode) {
        case ModelAuthMode::ApiKey:  return "api-key";
        case ModelAuthMode::OAuth:   return "oauth";
        case ModelAuthMode::Token:   return "token";
        case ModelAuthMode::Mixed:   return "mixed";
        case ModelAuthMode::AwsSdk:  return "aws-sdk";
        case ModelAuthMode::Unknown: return "unknown";
    }
    return "unknown";
}

ModelAuthMode resolve_model_auth_mode(
    const std::string& provider,
    const core::Config& cfg
) {
    auto normalized = normalize_provider_id(provider);
    if (normalized == "amazon-bedrock") return ModelAuthMode::AwsSdk;
    
    auto env_key = resolve_env_api_key(provider);
    if (env_key) {
        return env_key->source.find("OAUTH_TOKEN") != std::string::npos
            ? ModelAuthMode::OAuth : ModelAuthMode::ApiKey;
    }
    
    if (has_usable_custom_provider_api_key(cfg, provider)) return ModelAuthMode::ApiKey;
    if (normalized == "ollama") return ModelAuthMode::ApiKey;
    
    return ModelAuthMode::Unknown;
}

// ============================================================================
// Auth Label (对齐 model-auth-label.ts)
// ============================================================================

std::string resolve_model_auth_label(
    const std::string& provider,
    const core::Config& cfg
) {
    auto normalized = normalize_provider_id(provider);
    if (normalized == "amazon-bedrock") return "aws-sdk";
    
    auto env_key = resolve_env_api_key(provider);
    if (env_key) {
        if (env_key->source.find("OAUTH_TOKEN") != std::string::npos) {
            return "oauth (" + env_key->source + ")";
        }
        return "api-key (" + env_key->source + ")";
    }
    
    if (has_usable_custom_provider_api_key(cfg, provider)) {
        return "api-key (models.json)";
    }
    
    if (normalized == "ollama") return "api-key (local)";
    
    return "unknown";
}

} // namespace llm
} // namespace hunterclaw
