// 🎯 Strict alignment: OpenClaw web-fetch/runtime.ts
// Web fetch runtime — provider resolution, auto-detect, tool definition creation.
// Symmetric structure with web-search/runtime.ts.

#include <hunterclaw/tools/tool.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw::tools {

// Forward declarations from web_provider_shared.cc
nlohmann::json resolve_web_provider_config(const nlohmann::json& config, const std::string& kind);
std::optional<std::string> read_web_provider_env_value(const std::vector<std::string>& env_vars);

struct WebProviderEntry {
    std::string id;
    std::string label;
    std::vector<std::string> env_vars;
    bool requires_credential = true;
};

struct ResolveWebProviderResult {
    bool found = false;
    WebProviderEntry provider;
};

bool has_web_provider_entry_credential(
    const WebProviderEntry& provider, const nlohmann::json& config,
    const nlohmann::json& tool_config,
    std::function<std::optional<std::string>(const WebProviderEntry&, const nlohmann::json&)> resolve_raw,
    std::function<std::optional<std::string>(const WebProviderEntry&)> resolve_env);

namespace {
auto log() { return core::get_logger("web-fetch"); }
} // namespace

// ─── Types ───────────────────────────────────────────────────────────────

struct WebFetchConfig {
    std::optional<std::string> provider;
    std::optional<bool> enabled;
};

struct ResolveWebFetchParams {
    nlohmann::json config;
    bool sandboxed = false;
    std::optional<std::string> provider_id;
    bool prefer_runtime_providers = false;
};

// ─── Enabled check (aligned: resolveWebFetchEnabled) ─────────────────────

// 🎯 对齐 OC: resolveWebFetchEnabled
bool resolve_web_fetch_enabled(const WebFetchConfig& fetch, bool sandboxed) {
    if (fetch.enabled.has_value()) {
        return *fetch.enabled;
    }
    return true; // Default enabled
}

// ─── Provider ID resolution (aligned: resolveWebFetchProviderId) ─────────

// 🎯 对齐 OC: resolveWebFetchProviderId
// Same pattern as web-search: explicit config → credential → keyless
std::string resolve_web_fetch_provider_id(
        const nlohmann::json& config,
        const WebFetchConfig& fetch,
        const std::vector<WebProviderEntry>& providers) {

    // 1. Explicit provider
    if (fetch.provider.has_value() && !fetch.provider->empty()) {
        for (const auto& p : providers) {
            if (p.id == *fetch.provider) return p.id;
        }
    }

    // 2. Auto-detect: keyless first, then credential
    for (const auto& provider : providers) {
        if (!provider.requires_credential) {
            log()->debug("web_fetch: auto-detected keyless provider '{}'", provider.id);
            return provider.id;
        }
        auto env_val = read_web_provider_env_value(provider.env_vars);
        if (env_val.has_value()) {
            log()->debug("web_fetch: auto-detected '{}' from API keys", provider.id);
            return provider.id;
        }
    }

    return "";
}

// ─── Provider configured check (aligned: isWebFetchProviderConfigured) ───

// 🎯 对齐 OC: isWebFetchProviderConfigured
bool is_web_fetch_provider_configured(
        const WebProviderEntry& provider,
        const nlohmann::json& config) {
    auto fetch_config = resolve_web_provider_config(config, "fetch");
    return has_web_provider_entry_credential(
        provider, config, fetch_config,
        [](const WebProviderEntry& p, const nlohmann::json& cfg) -> std::optional<std::string> {
            return std::nullopt; // Simplified — actual impl checks config-stored values
        },
        [](const WebProviderEntry& p) -> std::optional<std::string> {
            return read_web_provider_env_value(p.env_vars);
        }
    );
}

// ─── List providers (aligned: listWebFetchProviders) ─────────────────────

// 🎯 对齐 OC: listWebFetchProviders
std::vector<WebProviderEntry> list_web_fetch_providers(const nlohmann::json& config) {
    // In production, this would resolve from plugin registry
    // For now, return empty — tools/web_fetch_tool.cc handles the actual provider list
    return {};
}

// ─── Resolve definition (aligned: resolveWebFetchDefinition) ─────────────

// 🎯 对齐 OC: resolveWebFetchDefinition
std::optional<ResolveWebProviderResult> resolve_web_fetch_definition(
        const ResolveWebFetchParams& params) {
    auto fetch_config = resolve_web_provider_config(params.config, "fetch");

    WebFetchConfig fetch;
    fetch.provider = fetch_config.value("provider", std::string(""));
    if (fetch.provider->empty()) fetch.provider = std::nullopt;
    fetch.enabled = fetch_config.contains("enabled")
        ? std::optional<bool>(fetch_config["enabled"].get<bool>())
        : std::nullopt;

    if (!resolve_web_fetch_enabled(fetch, params.sandboxed)) {
        return std::nullopt;
    }

    auto providers = list_web_fetch_providers(params.config);
    if (providers.empty()) {
        return std::nullopt;
    }

    auto provider_id = resolve_web_fetch_provider_id(
        params.config, fetch, providers);
    if (provider_id.empty()) {
        return std::nullopt;
    }

    log()->info("resolve_web_fetch_definition: resolved provider='{}'", provider_id);
    return std::nullopt; // Actual tool creation handled by web_fetch_tool.cc
}

} // namespace hunterclaw::tools
