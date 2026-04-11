#pragma once
// ============================================================================
// provider_request_config.h
// 严格对齐 OpenClaw: provider-request-config.ts (714行)
// 请求配置 (auth/proxy/TLS/header 合并)
// ============================================================================

#include "hunterclaw/llm/provider_attribution.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <map>
#include <optional>
#include <vector>

namespace hunterclaw {
namespace llm {

// ============================================================================
// Auth Override (对齐 provider-request-config.ts:20-33)
// ============================================================================

enum class RequestAuthMode {
    ProviderDefault,
    AuthorizationBearer,
    Header
};

struct ProviderRequestAuthOverride {
    RequestAuthMode mode = RequestAuthMode::ProviderDefault;
    std::string token;       // for AuthorizationBearer
    std::string header_name; // for Header
    std::string value;       // for Header
    std::string prefix;      // for Header
};

// ============================================================================
// TLS Override (对齐 provider-request-config.ts:35-42)
// ============================================================================

struct ProviderRequestTlsOverride {
    std::optional<std::string> ca;
    std::optional<std::string> cert;
    std::optional<std::string> key;
    std::optional<std::string> passphrase;
    std::optional<std::string> server_name;
    std::optional<bool> insecure_skip_verify;
};

// ============================================================================
// Proxy Override (对齐 provider-request-config.ts:44-53)
// ============================================================================

enum class ProxyMode {
    None,
    EnvProxy,
    ExplicitProxy
};

struct ProviderRequestProxyOverride {
    ProxyMode mode = ProxyMode::None;
    std::string url;  // for ExplicitProxy
    ProviderRequestTlsOverride tls;
};

// ============================================================================
// Transport Overrides (对齐 provider-request-config.ts:55-60)
// ============================================================================

struct ProviderRequestTransportOverrides {
    std::map<std::string, std::string> headers;
    std::optional<ProviderRequestAuthOverride> auth;
    std::optional<ProviderRequestProxyOverride> proxy;
    std::optional<ProviderRequestTlsOverride> tls;
};

// ============================================================================
// Resolved Config (对齐 provider-request-config.ts:119-128)
// ============================================================================

struct ResolvedProviderRequestAuthConfig {
    bool configured = false;
    RequestAuthMode mode = RequestAuthMode::ProviderDefault;
    std::string header_name;
    std::string value;
    std::string prefix;
    bool inject_authorization_header = false;
};

struct ResolvedProviderRequestTlsConfig {
    bool configured = false;
    std::optional<std::string> ca;
    std::optional<std::string> cert;
    std::optional<std::string> key;
    std::optional<std::string> passphrase;
    std::optional<std::string> server_name;
    std::optional<bool> reject_unauthorized;
};

struct ResolvedProviderRequestProxyConfig {
    bool configured = false;
    ProxyMode mode = ProxyMode::None;
    std::string proxy_url;
    ResolvedProviderRequestTlsConfig tls;
};

struct ResolvedProviderRequestConfig {
    std::string api;
    std::string base_url;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> extra_headers;
    ResolvedProviderRequestAuthConfig auth;
    ResolvedProviderRequestProxyConfig proxy;
    ResolvedProviderRequestTlsConfig tls;
    ProviderRequestPolicyResolution policy;
    bool allow_private_network = false;
};

// ============================================================================
// Header Merging (对齐 provider-request-config.ts:349-375)
// ============================================================================

/// 对齐: mergeProviderRequestHeaders
std::map<std::string, std::string> merge_provider_request_headers(
    const std::vector<std::map<std::string, std::string>>& header_sets);

/// 对齐: normalizeBaseUrl
std::string normalize_base_url(const std::string& base_url, const std::string& fallback = "");

/// 对齐: mergeProviderRequestOverrides
ProviderRequestTransportOverrides merge_provider_request_overrides(
    const std::vector<ProviderRequestTransportOverrides>& overrides);

// ============================================================================
// Request Config Resolution (对齐 provider-request-config.ts:572-663)
// ============================================================================

struct ResolveProviderRequestPolicyConfigParams {
    std::string provider;
    std::string api;
    std::string base_url;
    std::string default_base_url;
    ProviderRequestCapability capability = ProviderRequestCapability::LLM;
    ProviderRequestTransport transport = ProviderRequestTransport::HTTP;
    std::map<std::string, std::string> discovered_headers;
    std::map<std::string, std::string> provider_headers;
    std::map<std::string, std::string> model_headers;
    std::map<std::string, std::string> caller_headers;
    bool auth_header = false;
    bool allow_private_network = false;
    std::optional<ProviderRequestTransportOverrides> request;
};

/// 对齐: resolveProviderRequestPolicyConfig
ResolvedProviderRequestConfig resolve_provider_request_policy_config(
    const ResolveProviderRequestPolicyConfigParams& params);

/// 对齐: resolveProviderRequestConfig
ResolvedProviderRequestConfig resolve_provider_request_config(
    const std::string& provider,
    const std::string& api = "",
    const std::string& base_url = "",
    const std::map<std::string, std::string>& provider_headers = {},
    const std::map<std::string, std::string>& model_headers = {});

} // namespace llm
} // namespace hunterclaw
