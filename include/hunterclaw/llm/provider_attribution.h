#pragma once
// ============================================================================
// provider_attribution.h
// 严格对齐 OpenClaw: provider-attribution.ts (619行)
// 端点分类 / 请求能力 / 归因策略
// ============================================================================

#include <string>
#include <map>
#include <vector>
#include <optional>
#include <set>

namespace hunterclaw {
namespace llm {

// ============================================================================
// Provider Endpoint Class (对齐 provider-attribution.ts:38-60)
// ============================================================================

enum class ProviderEndpointClass {
    Default,
    AnthropicPublic,
    CerebrasNative,
    ChutesNative,
    DeepseekNative,
    GithubCopilotNative,
    GroqNative,
    MistralPublic,
    MoonshotNative,
    ModelstudioNative,
    OpenAIPublic,
    OpenAICodex,
    OpencodeNative,
    AzureOpenAI,
    OpenRouter,
    XAINative,
    ZAINative,
    GoogleGenerativeAI,
    GoogleVertex,
    Local,
    Custom,
    Invalid
};

std::string endpoint_class_to_string(ProviderEndpointClass ec);

// ============================================================================
// Provider Endpoint Resolution (对齐 provider-attribution.ts:62-275)
// ============================================================================

struct ProviderEndpointResolution {
    ProviderEndpointClass endpoint_class = ProviderEndpointClass::Default;
    std::optional<std::string> hostname;
    std::optional<std::string> google_vertex_region;
};

/// 对齐: resolveProviderEndpoint(baseUrl)
ProviderEndpointResolution resolve_provider_endpoint(const std::string& base_url);

// ============================================================================
// Provider Attribution (对齐 provider-attribution.ts:9-31)
// ============================================================================

struct ProviderAttributionPolicy {
    std::string provider;
    bool enabled_by_default = false;
    std::string verification;  // "vendor-documented" | "vendor-hidden-api-spec" | ...
    std::string hook;          // "request-headers" | "default-headers" | ...
    std::string product;
    std::string version;
    std::map<std::string, std::string> headers;
    std::optional<std::string> docs_url;
    std::optional<std::string> review_note;
};

/// 对齐: resolveProviderAttributionPolicy
std::optional<ProviderAttributionPolicy> resolve_provider_attribution_policy(
    const std::string& provider);

/// 对齐: resolveProviderAttributionHeaders
std::optional<std::map<std::string, std::string>> resolve_provider_attribution_headers(
    const std::string& provider);

/// 对齐: listProviderAttributionPolicies
std::vector<ProviderAttributionPolicy> list_provider_attribution_policies();

// ============================================================================
// Provider Request Capabilities (对齐 provider-attribution.ts:535-618)
// ============================================================================

enum class ProviderRequestTransport { Stream, WebSocket, HTTP, MediaUnderstanding };
enum class ProviderRequestCapability { LLM, Audio, Image, Video, Other };

struct ProviderRequestPolicyResolution {
    std::optional<std::string> provider;
    std::optional<ProviderAttributionPolicy> policy;
    ProviderEndpointClass endpoint_class = ProviderEndpointClass::Default;
    bool uses_configured_base_url = false;
    std::string known_provider_family;
    std::optional<std::string> attribution_provider;
    std::optional<std::map<std::string, std::string>> attribution_headers;
    bool allows_hidden_attribution = false;
    bool uses_known_native_openai_endpoint = false;
    bool uses_known_native_openai_route = false;
    bool uses_verified_openai_attribution_host = false;
    bool uses_explicit_proxy_like_endpoint = false;
};

struct ProviderRequestCapabilities : public ProviderRequestPolicyResolution {
    bool is_known_native_endpoint = false;
    bool allows_openai_service_tier = false;
    bool supports_openai_reasoning_compat_payload = false;
    bool allows_anthropic_service_tier = false;
    bool supports_responses_store_field = false;
    bool allows_responses_store = false;
    bool should_strip_responses_prompt_cache = false;
    bool supports_native_streaming_usage_compat = false;
    std::optional<std::string> compatibility_family;
};

/// 对齐: resolveProviderRequestPolicy
ProviderRequestPolicyResolution resolve_provider_request_policy(
    const std::string& provider,
    const std::string& api = "",
    const std::string& base_url = "");

/// 对齐: resolveProviderRequestCapabilities
ProviderRequestCapabilities resolve_provider_request_capabilities(
    const std::string& provider,
    const std::string& api = "",
    const std::string& base_url = "",
    const std::string& model_id = "");

/// 对齐: resolveKnownProviderFamily
std::string resolve_known_provider_family(const std::string& provider);

} // namespace llm
} // namespace hunterclaw
