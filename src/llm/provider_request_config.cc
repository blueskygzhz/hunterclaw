// ============================================================================
// provider_request_config.cc
// 严格对齐 OpenClaw: provider-request-config.ts (714行)
// ============================================================================

#include "hunterclaw/llm/provider_request_config.h"
#include "hunterclaw/llm/model_selection.h"
#include <algorithm>
#include <set>

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

const std::set<std::string> FORBIDDEN_HEADER_KEYS = {"__proto__", "prototype", "constructor"};

} // namespace

// ============================================================================
// Header Merging (对齐 provider-request-config.ts:349-375)
// ============================================================================

std::map<std::string, std::string> merge_provider_request_headers(
    const std::vector<std::map<std::string, std::string>>& header_sets
) {
    std::map<std::string, std::string> merged;
    std::map<std::string, std::string> key_by_lower;
    
    for (const auto& headers : header_sets) {
        for (const auto& [key, value] : headers) {
            auto lower_key = to_lower(key);
            if (FORBIDDEN_HEADER_KEYS.count(lower_key)) continue;
            
            auto prev_it = key_by_lower.find(lower_key);
            if (prev_it != key_by_lower.end() && prev_it->second != key) {
                merged.erase(prev_it->second);
            }
            merged[key] = value;
            key_by_lower[lower_key] = key;
        }
    }
    
    return merged;
}

std::string normalize_base_url(const std::string& base_url, const std::string& fallback) {
    auto raw = trim(base_url);
    if (raw.empty()) raw = trim(fallback);
    if (raw.empty()) return "";
    // Strip trailing slashes
    while (!raw.empty() && raw.back() == '/') raw.pop_back();
    return raw;
}

ProviderRequestTransportOverrides merge_provider_request_overrides(
    const std::vector<ProviderRequestTransportOverrides>& overrides
) {
    ProviderRequestTransportOverrides merged;
    
    for (const auto& current : overrides) {
        // Merge headers
        for (const auto& [k, v] : current.headers) {
            merged.headers[k] = v;
        }
        // Last auth wins
        if (current.auth) merged.auth = current.auth;
        // Last proxy wins
        if (current.proxy) merged.proxy = current.proxy;
        // Last TLS wins
        if (current.tls) merged.tls = current.tls;
    }
    
    return merged;
}

// ============================================================================
// Auth/TLS/Proxy Resolution
// ============================================================================

static ResolvedProviderRequestAuthConfig resolve_auth_override(
    bool auth_header,
    const std::optional<ProviderRequestTransportOverrides>& request
) {
    ResolvedProviderRequestAuthConfig config;
    
    if (request && request->auth) {
        const auto& auth = *request->auth;
        if (auth.mode == RequestAuthMode::AuthorizationBearer && !auth.token.empty()) {
            config.configured = true;
            config.mode = RequestAuthMode::AuthorizationBearer;
            config.header_name = "Authorization";
            config.value = auth.token;
            config.inject_authorization_header = true;
            return config;
        }
        if (auth.mode == RequestAuthMode::Header && !auth.header_name.empty() && !auth.value.empty()) {
            config.configured = true;
            config.mode = RequestAuthMode::Header;
            config.header_name = auth.header_name;
            config.value = auth.value;
            config.prefix = auth.prefix;
            config.inject_authorization_header = false;
            return config;
        }
    }
    
    config.configured = false;
    config.mode = auth_header ? RequestAuthMode::AuthorizationBearer : RequestAuthMode::ProviderDefault;
    config.inject_authorization_header = auth_header;
    return config;
}

static ResolvedProviderRequestTlsConfig resolve_tls_override(
    const std::optional<ProviderRequestTlsOverride>& tls
) {
    if (!tls) return {false};
    
    // 对齐 OC: insecureSkipVerify is forbidden
    if (tls->insecure_skip_verify && *tls->insecure_skip_verify) {
        return {false};  // Silently reject in C++ (OC throws)
    }
    
    ResolvedProviderRequestTlsConfig config;
    config.configured = true;
    config.ca = tls->ca;
    config.cert = tls->cert;
    config.key = tls->key;
    config.passphrase = tls->passphrase;
    config.server_name = tls->server_name;
    if (tls->insecure_skip_verify && !*tls->insecure_skip_verify) {
        config.reject_unauthorized = true;
    }
    return config;
}

static ResolvedProviderRequestProxyConfig resolve_proxy_override(
    const std::optional<ProviderRequestTransportOverrides>& request
) {
    if (!request || !request->proxy) return {false};
    
    const auto& proxy = *request->proxy;
    ResolvedProviderRequestProxyConfig config;
    config.configured = true;
    config.mode = proxy.mode;
    config.proxy_url = trim(proxy.url);
    config.tls = resolve_tls_override(
        proxy.tls.ca || proxy.tls.cert || proxy.tls.key
        ? std::optional(proxy.tls)
        : std::nullopt);
    
    if (config.mode == ProxyMode::ExplicitProxy && config.proxy_url.empty()) {
        return {false};
    }
    
    return config;
}

// ============================================================================
// Request Config Resolution
// ============================================================================

ResolvedProviderRequestConfig resolve_provider_request_policy_config(
    const ResolveProviderRequestPolicyConfigParams& params
) {
    auto base_url = normalize_base_url(params.base_url, params.default_base_url);
    auto policy = resolve_provider_request_policy(params.provider, params.api, base_url);
    auto auth = resolve_auth_override(params.auth_header, params.request);
    auto proxy = resolve_proxy_override(params.request);
    auto tls = resolve_tls_override(params.request ? params.request->tls : std::nullopt);
    
    // Merge headers
    std::vector<std::map<std::string, std::string>> header_sets;
    if (!params.discovered_headers.empty()) header_sets.push_back(params.discovered_headers);
    if (!params.provider_headers.empty()) header_sets.push_back(params.provider_headers);
    if (!params.model_headers.empty()) header_sets.push_back(params.model_headers);
    if (params.request && !params.request->headers.empty()) {
        header_sets.push_back(params.request->headers);
    }
    auto extra_headers = merge_provider_request_headers(header_sets);
    
    // Add attribution headers
    if (policy.attribution_headers) {
        header_sets.push_back(*policy.attribution_headers);
    }
    auto all_headers = merge_provider_request_headers(header_sets);
    
    // Merge caller headers (defaults-win by default)
    if (!params.caller_headers.empty()) {
        // Caller headers go first (lower priority)
        std::vector<std::map<std::string, std::string>> final_sets;
        final_sets.push_back(params.caller_headers);
        final_sets.push_back(all_headers);
        all_headers = merge_provider_request_headers(final_sets);
    }
    
    return ResolvedProviderRequestConfig{
        params.api,
        base_url,
        all_headers,
        extra_headers,
        auth,
        proxy,
        tls,
        policy,
        params.allow_private_network
    };
}

ResolvedProviderRequestConfig resolve_provider_request_config(
    const std::string& provider,
    const std::string& api,
    const std::string& base_url,
    const std::map<std::string, std::string>& provider_headers,
    const std::map<std::string, std::string>& model_headers
) {
    ResolveProviderRequestPolicyConfigParams params;
    params.provider = provider;
    params.api = api;
    params.base_url = base_url;
    params.provider_headers = provider_headers;
    params.model_headers = model_headers;
    return resolve_provider_request_policy_config(params);
}

} // namespace llm
} // namespace hunterclaw
