// ============================================================================
// provider_attribution.cc
// 严格对齐 OpenClaw: provider-attribution.ts (619行)
// ============================================================================

#include "hunterclaw/llm/provider_attribution.h"
#include "hunterclaw/llm/model_selection.h"
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace llm {

namespace {

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

const std::string ATTRIBUTION_PRODUCT = "HunterClaw";
const std::string ATTRIBUTION_VERSION = "0.1.0";
const std::string ATTRIBUTION_ORIGINATOR = "hunterclaw";

// 对齐 OC: LOCAL_ENDPOINT_HOSTS
const std::set<std::string> LOCAL_HOSTS = {
    "localhost", "127.0.0.1", "::1", "[::1]"
};

bool is_local_endpoint_host(const std::string& host) {
    if (LOCAL_HOSTS.count(host)) return true;
    if (host.size() > 10 && host.substr(host.size() - 10) == ".localhost") return true;
    if (host.size() > 6 && host.substr(host.size() - 6) == ".local") return true;
    if (host.size() > 9 && host.substr(host.size() - 9) == ".internal") return true;
    return false;
}

std::string try_parse_hostname(const std::string& url) {
    // Simple URL hostname extraction
    auto proto_end = url.find("://");
    std::string after_proto = (proto_end != std::string::npos) ? url.substr(proto_end + 3) : url;
    auto path_start = after_proto.find('/');
    auto host_port = (path_start != std::string::npos) ? after_proto.substr(0, path_start) : after_proto;
    auto port_start = host_port.rfind(':');
    // Skip if it's part of IPv6 address
    if (port_start != std::string::npos && host_port.find('[') == std::string::npos) {
        return to_lower(host_port.substr(0, port_start));
    }
    return to_lower(host_port);
}

// 对齐 OC: MOONSHOT_NATIVE_BASE_URLS
const std::set<std::string> MOONSHOT_URLS = {
    "https://api.moonshot.ai/v1",
    "https://api.moonshot.cn/v1"
};

const std::set<std::string> MODELSTUDIO_URLS = {
    "https://coding-intl.dashscope.aliyuncs.com/v1",
    "https://coding.dashscope.aliyuncs.com/v1",
    "https://dashscope.aliyuncs.com/compatible-mode/v1",
    "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"
};

std::string normalize_base_url(const std::string& url) {
    auto trimmed = trim(url);
    auto lower = to_lower(trimmed);
    // Strip trailing slashes
    while (!lower.empty() && lower.back() == '/') lower.pop_back();
    return lower;
}

} // namespace

// ============================================================================
// Endpoint Class
// ============================================================================

std::string endpoint_class_to_string(ProviderEndpointClass ec) {
    switch (ec) {
        case ProviderEndpointClass::Default:            return "default";
        case ProviderEndpointClass::AnthropicPublic:    return "anthropic-public";
        case ProviderEndpointClass::CerebrasNative:     return "cerebras-native";
        case ProviderEndpointClass::ChutesNative:       return "chutes-native";
        case ProviderEndpointClass::DeepseekNative:     return "deepseek-native";
        case ProviderEndpointClass::GithubCopilotNative:return "github-copilot-native";
        case ProviderEndpointClass::GroqNative:         return "groq-native";
        case ProviderEndpointClass::MistralPublic:      return "mistral-public";
        case ProviderEndpointClass::MoonshotNative:     return "moonshot-native";
        case ProviderEndpointClass::ModelstudioNative:  return "modelstudio-native";
        case ProviderEndpointClass::OpenAIPublic:       return "openai-public";
        case ProviderEndpointClass::OpenAICodex:        return "openai-codex";
        case ProviderEndpointClass::OpencodeNative:     return "opencode-native";
        case ProviderEndpointClass::AzureOpenAI:        return "azure-openai";
        case ProviderEndpointClass::OpenRouter:         return "openrouter";
        case ProviderEndpointClass::XAINative:          return "xai-native";
        case ProviderEndpointClass::ZAINative:          return "zai-native";
        case ProviderEndpointClass::GoogleGenerativeAI: return "google-generative-ai";
        case ProviderEndpointClass::GoogleVertex:       return "google-vertex";
        case ProviderEndpointClass::Local:              return "local";
        case ProviderEndpointClass::Custom:             return "custom";
        case ProviderEndpointClass::Invalid:            return "invalid";
    }
    return "unknown";
}

// ============================================================================
// Endpoint Resolution (对齐 provider-attribution.ts:193-275)
// ============================================================================

ProviderEndpointResolution resolve_provider_endpoint(const std::string& base_url) {
    auto trimmed = trim(base_url);
    if (trimmed.empty()) return {ProviderEndpointClass::Default};
    
    auto host = try_parse_hostname(trimmed);
    if (host.empty()) return {ProviderEndpointClass::Invalid};
    
    auto normalized = normalize_base_url(trimmed);
    
    if (MOONSHOT_URLS.count(normalized))    return {ProviderEndpointClass::MoonshotNative, host};
    if (MODELSTUDIO_URLS.count(normalized)) return {ProviderEndpointClass::ModelstudioNative, host};
    if (host == "api.openai.com")           return {ProviderEndpointClass::OpenAIPublic, host};
    if (host == "api.anthropic.com")        return {ProviderEndpointClass::AnthropicPublic, host};
    if (host == "api.mistral.ai")           return {ProviderEndpointClass::MistralPublic, host};
    if (host == "api.cerebras.ai")          return {ProviderEndpointClass::CerebrasNative, host};
    if (host == "llm.chutes.ai")            return {ProviderEndpointClass::ChutesNative, host};
    if (host == "api.deepseek.com")         return {ProviderEndpointClass::DeepseekNative, host};
    if (host.size() > 18 && host.substr(host.size() - 18) == ".githubcopilot.com")
        return {ProviderEndpointClass::GithubCopilotNative, host};
    if (host == "api.groq.com")             return {ProviderEndpointClass::GroqNative, host};
    if (host == "chatgpt.com")              return {ProviderEndpointClass::OpenAICodex, host};
    if (host == "opencode.ai" || (host.size() > 11 && host.substr(host.size() - 11) == ".opencode.ai"))
        return {ProviderEndpointClass::OpencodeNative, host};
    if (host == "openrouter.ai" || (host.size() > 14 && host.substr(host.size() - 14) == ".openrouter.ai"))
        return {ProviderEndpointClass::OpenRouter, host};
    if (host == "api.x.ai" || host == "api.grok.x.ai")
        return {ProviderEndpointClass::XAINative, host};
    if (host == "api.z.ai")
        return {ProviderEndpointClass::ZAINative, host};
    if (host.size() > 17 && host.substr(host.size() - 17) == ".openai.azure.com")
        return {ProviderEndpointClass::AzureOpenAI, host};
    if (host == "generativelanguage.googleapis.com")
        return {ProviderEndpointClass::GoogleGenerativeAI, host};
    if (host == "aiplatform.googleapis.com")
        return {ProviderEndpointClass::GoogleVertex, host, "global"};
    // Regional Google Vertex: <region>-aiplatform.googleapis.com
    if (host.size() > 27 && host.substr(host.size() - 27) == "-aiplatform.googleapis.com") {
        auto region = host.substr(0, host.size() - 27);
        return {ProviderEndpointClass::GoogleVertex, host, region};
    }
    if (is_local_endpoint_host(host))
        return {ProviderEndpointClass::Local, host};
    
    return {ProviderEndpointClass::Custom, host};
}

// ============================================================================
// Known Provider Family (对齐 provider-attribution.ts:277-317)
// ============================================================================

std::string resolve_known_provider_family(const std::string& provider) {
    auto normalized = normalize_provider_id(provider);
    if (normalized == "openai" || normalized == "openai-codex" ||
        normalized == "azure-openai" || normalized == "azure-openai-responses")
        return "openai-family";
    if (normalized == "openrouter") return "openrouter";
    if (normalized == "anthropic")  return "anthropic";
    if (normalized == "chutes")     return "chutes";
    if (normalized == "deepseek")   return "deepseek";
    if (normalized == "google")     return "google";
    if (normalized == "xai")        return "xai";
    if (normalized == "zai")        return "zai";
    if (normalized == "moonshot" || normalized == "kimi") return "moonshot";
    if (normalized == "qwen" || normalized == "dashscope") return "modelstudio";
    if (normalized == "github-copilot") return "github-copilot";
    if (normalized == "groq")       return "groq";
    if (normalized == "mistral")    return "mistral";
    if (normalized == "together")   return "together";
    return normalized.empty() ? "unknown" : normalized;
}

// ============================================================================
// Attribution Policies (对齐 provider-attribution.ts:328-442)
// ============================================================================

std::vector<ProviderAttributionPolicy> list_provider_attribution_policies() {
    std::vector<ProviderAttributionPolicy> policies;
    
    // OpenRouter (vendor-documented)
    policies.push_back({
        "openrouter", true, "vendor-documented", "request-headers",
        ATTRIBUTION_PRODUCT, ATTRIBUTION_VERSION,
        {{"HTTP-Referer", "https://hunterclaw.ai"},
         {"X-OpenRouter-Title", ATTRIBUTION_PRODUCT},
         {"X-OpenRouter-Categories", "cli-agent"}},
        "https://openrouter.ai/docs/app-attribution"
    });
    
    // OpenAI (vendor-hidden-api-spec)
    policies.push_back({
        "openai", true, "vendor-hidden-api-spec", "request-headers",
        ATTRIBUTION_PRODUCT, ATTRIBUTION_VERSION,
        {{"originator", ATTRIBUTION_ORIGINATOR},
         {"version", ATTRIBUTION_VERSION},
         {"User-Agent", ATTRIBUTION_ORIGINATOR + "/" + ATTRIBUTION_VERSION}}
    });
    
    // OpenAI Codex
    policies.push_back({
        "openai-codex", true, "vendor-hidden-api-spec", "request-headers",
        ATTRIBUTION_PRODUCT, ATTRIBUTION_VERSION,
        {{"originator", ATTRIBUTION_ORIGINATOR},
         {"version", ATTRIBUTION_VERSION},
         {"User-Agent", ATTRIBUTION_ORIGINATOR + "/" + ATTRIBUTION_VERSION}}
    });
    
    // SDK-hook-only policies (not enabled by default)
    for (const auto& [prov, hook] : std::vector<std::pair<std::string,std::string>>{
        {"anthropic", "default-headers"},
        {"google", "user-agent-extra"},
        {"groq", "default-headers"},
        {"mistral", "custom-user-agent"},
        {"together", "default-headers"}
    }) {
        policies.push_back({
            prov, false, "vendor-sdk-hook-only", hook,
            ATTRIBUTION_PRODUCT, ATTRIBUTION_VERSION, {}
        });
    }
    
    return policies;
}

std::optional<ProviderAttributionPolicy> resolve_provider_attribution_policy(
    const std::string& provider
) {
    auto normalized = normalize_provider_id(provider);
    for (const auto& policy : list_provider_attribution_policies()) {
        if (policy.provider == normalized) return policy;
    }
    return std::nullopt;
}

std::optional<std::map<std::string, std::string>> resolve_provider_attribution_headers(
    const std::string& provider
) {
    auto policy = resolve_provider_attribution_policy(provider);
    if (!policy || !policy->enabled_by_default) return std::nullopt;
    return policy->headers.empty() ? std::nullopt : std::optional(policy->headers);
}

// ============================================================================
// Request Policy (对齐 provider-attribution.ts:463-526)
// ============================================================================

ProviderRequestPolicyResolution resolve_provider_request_policy(
    const std::string& provider,
    const std::string& api,
    const std::string& base_url
) {
    auto normalized = normalize_provider_id(provider);
    auto policy = resolve_provider_attribution_policy(normalized);
    auto endpoint = resolve_provider_endpoint(base_url);
    auto ec = endpoint.endpoint_class;
    auto lower_api = to_lower(trim(api));
    
    bool uses_configured = ec != ProviderEndpointClass::Default;
    bool uses_native_openai = ec == ProviderEndpointClass::OpenAIPublic ||
                              ec == ProviderEndpointClass::OpenAICodex ||
                              ec == ProviderEndpointClass::AzureOpenAI;
    bool uses_openai_public = ec == ProviderEndpointClass::OpenAIPublic;
    bool uses_codex = ec == ProviderEndpointClass::OpenAICodex;
    bool uses_verified = uses_openai_public || uses_codex;
    bool uses_proxy_like = uses_configured && !uses_native_openai;
    
    std::optional<std::string> attribution_provider;
    if (normalized == "openai" && uses_openai_public &&
        (lower_api == "openai-completions" || lower_api == "openai-responses")) {
        attribution_provider = "openai";
    } else if (normalized == "openai-codex" && uses_codex &&
               (lower_api == "openai-codex-responses" || lower_api == "openai-responses")) {
        attribution_provider = "openai-codex";
    } else if (normalized == "openrouter" && policy && policy->enabled_by_default) {
        if (ec == ProviderEndpointClass::OpenRouter || ec == ProviderEndpointClass::Default) {
            attribution_provider = "openrouter";
        }
    }
    
    auto attr_headers = attribution_provider
        ? resolve_provider_attribution_headers(*attribution_provider)
        : std::nullopt;
    
    return ProviderRequestPolicyResolution{
        normalized.empty() ? std::nullopt : std::optional(normalized),
        policy,
        ec,
        uses_configured,
        resolve_known_provider_family(normalized),
        attribution_provider,
        attr_headers,
        attribution_provider.has_value() && policy && policy->verification == "vendor-hidden-api-spec",
        uses_native_openai,
        ec == ProviderEndpointClass::Default ? (normalized == "openai") : uses_native_openai,
        uses_verified,
        uses_proxy_like
    };
}

// ============================================================================
// Request Capabilities (对齐 provider-attribution.ts:535-618)
// ============================================================================

ProviderRequestCapabilities resolve_provider_request_capabilities(
    const std::string& provider,
    const std::string& api,
    const std::string& base_url,
    const std::string& model_id
) {
    auto policy = resolve_provider_request_policy(provider, api, base_url);
    auto ec = policy.endpoint_class;
    auto lower_api = to_lower(trim(api));
    auto normalized = policy.provider.value_or("");
    auto lower_model = to_lower(trim(model_id));
    
    bool is_known_native =
        ec == ProviderEndpointClass::AnthropicPublic ||
        ec == ProviderEndpointClass::CerebrasNative ||
        ec == ProviderEndpointClass::ChutesNative ||
        ec == ProviderEndpointClass::DeepseekNative ||
        ec == ProviderEndpointClass::GithubCopilotNative ||
        ec == ProviderEndpointClass::GroqNative ||
        ec == ProviderEndpointClass::MistralPublic ||
        ec == ProviderEndpointClass::MoonshotNative ||
        ec == ProviderEndpointClass::ModelstudioNative ||
        ec == ProviderEndpointClass::OpenAIPublic ||
        ec == ProviderEndpointClass::OpenAICodex ||
        ec == ProviderEndpointClass::OpencodeNative ||
        ec == ProviderEndpointClass::AzureOpenAI ||
        ec == ProviderEndpointClass::OpenRouter ||
        ec == ProviderEndpointClass::XAINative ||
        ec == ProviderEndpointClass::ZAINative ||
        ec == ProviderEndpointClass::GoogleGenerativeAI ||
        ec == ProviderEndpointClass::GoogleVertex;
    
    // Moonshot compat family
    std::optional<std::string> compat_family;
    static const std::set<std::string> MOONSHOT_PROVIDERS = {"moonshot", "kimi"};
    if (MOONSHOT_PROVIDERS.count(normalized)) {
        compat_family = "moonshot";
    } else if (normalized == "ollama" && lower_model.find("kimi-k") != std::string::npos &&
               lower_model.find(":cloud") != std::string::npos) {
        compat_family = "moonshot";
    }
    
    static const std::set<std::string> RESPONSES_APIS = {
        "openai-responses", "azure-openai-responses"
    };
    static const std::set<std::string> RESPONSES_PROVIDERS = {
        "openai", "azure-openai", "azure-openai-responses"
    };
    
    ProviderRequestCapabilities caps;
    // Copy policy fields
    caps.provider = policy.provider;
    caps.policy = policy.policy;
    caps.endpoint_class = policy.endpoint_class;
    caps.uses_configured_base_url = policy.uses_configured_base_url;
    caps.known_provider_family = policy.known_provider_family;
    caps.attribution_provider = policy.attribution_provider;
    caps.attribution_headers = policy.attribution_headers;
    caps.allows_hidden_attribution = policy.allows_hidden_attribution;
    caps.uses_known_native_openai_endpoint = policy.uses_known_native_openai_endpoint;
    caps.uses_known_native_openai_route = policy.uses_known_native_openai_route;
    caps.uses_verified_openai_attribution_host = policy.uses_verified_openai_attribution_host;
    caps.uses_explicit_proxy_like_endpoint = policy.uses_explicit_proxy_like_endpoint;
    
    // Capability-specific fields
    caps.is_known_native_endpoint = is_known_native;
    caps.allows_openai_service_tier =
        (normalized == "openai" && lower_api == "openai-responses" && ec == ProviderEndpointClass::OpenAIPublic) ||
        (normalized == "openai-codex" && 
         (lower_api == "openai-codex-responses" || lower_api == "openai-responses") &&
         ec == ProviderEndpointClass::OpenAICodex);
    caps.supports_openai_reasoning_compat_payload =
        !normalized.empty() && !lower_api.empty() &&
        !policy.uses_explicit_proxy_like_endpoint &&
        (normalized == "openai" || normalized == "openai-codex" ||
         normalized == "azure-openai" || normalized == "azure-openai-responses") &&
        (lower_api == "openai-completions" || lower_api == "openai-responses" ||
         lower_api == "openai-codex-responses" || lower_api == "azure-openai-responses");
    caps.allows_anthropic_service_tier =
        normalized == "anthropic" && lower_api == "anthropic-messages" &&
        (ec == ProviderEndpointClass::Default || ec == ProviderEndpointClass::AnthropicPublic);
    caps.supports_responses_store_field = !lower_api.empty() && RESPONSES_APIS.count(lower_api) > 0;
    caps.allows_responses_store =
        !normalized.empty() && !lower_api.empty() &&
        RESPONSES_APIS.count(lower_api) > 0 && RESPONSES_PROVIDERS.count(normalized) > 0 &&
        policy.uses_known_native_openai_endpoint;
    caps.should_strip_responses_prompt_cache =
        !lower_api.empty() && RESPONSES_APIS.count(lower_api) > 0 && policy.uses_explicit_proxy_like_endpoint;
    caps.supports_native_streaming_usage_compat =
        ec == ProviderEndpointClass::MoonshotNative || ec == ProviderEndpointClass::ModelstudioNative;
    caps.compatibility_family = compat_family;
    
    return caps;
}

} // namespace llm
} // namespace hunterclaw
