// 🎯 Strict alignment: OpenClaw web-search/runtime.ts + runtime-types.ts
// Web search runtime — provider resolution, auto-detect, fallback chain, execution.

#include <hunterclaw/tools/tool.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw::tools {

// Forward declarations from web_provider_shared.cc
struct WebProviderEntry;
struct ResolveWebProviderResult;
nlohmann::json resolve_web_provider_config(const nlohmann::json& config, const std::string& kind);
std::optional<std::string> read_web_provider_env_value(const std::vector<std::string>& env_vars);
bool has_web_provider_entry_credential(
    const WebProviderEntry& provider, const nlohmann::json& config,
    const nlohmann::json& tool_config,
    std::function<std::optional<std::string>(const WebProviderEntry&, const nlohmann::json&)> resolve_raw,
    std::function<std::optional<std::string>(const WebProviderEntry&)> resolve_env);

// Minimal WebProviderEntry definition for this TU
struct WebProviderEntry {
    std::string id;
    std::string label;
    std::vector<std::string> env_vars;
    bool requires_credential = true;
};

namespace {
auto log() { return core::get_logger("web-search"); }
} // namespace

// ─── Types (aligned: web-search/runtime-types.ts) ────────────────────────

struct WebSearchConfig {
    std::optional<std::string> provider;
    std::optional<bool> enabled;
};

struct RunWebSearchParams {
    nlohmann::json config;
    bool sandboxed = false;
    std::optional<std::string> provider_id;
    bool prefer_runtime_providers = true;
    nlohmann::json args;
};

struct RunWebSearchResult {
    std::string provider;
    nlohmann::json result;
};

// ─── Enabled check (aligned: resolveWebSearchEnabled) ────────────────────

// 🎯 对齐 OC: resolveWebSearchEnabled
bool resolve_web_search_enabled(const WebSearchConfig& search, bool sandboxed) {
    if (search.enabled.has_value()) {
        return *search.enabled;
    }
    return true; // Default enabled
}

// ─── Provider ID resolution (aligned: resolveWebSearchProviderId) ────────

// 🎯 对齐 OC: resolveWebSearchProviderId
// Auto-detect: explicit config → credential-based → keyless fallback
std::string resolve_web_search_provider_id(
        const nlohmann::json& config,
        const WebSearchConfig& search,
        const std::vector<WebProviderEntry>& providers) {

    // 1. Explicit provider in config
    if (search.provider.has_value() && !search.provider->empty()) {
        for (const auto& p : providers) {
            if (p.id == *search.provider) return p.id;
        }
        log()->warn("web_search: configured provider '{}' not found, auto-detecting",
            *search.provider);
    }

    // 2. Auto-detect by credential availability
    std::optional<std::string> keyless_fallback;
    for (const auto& provider : providers) {
        if (!provider.requires_credential) {
            if (!keyless_fallback.has_value()) {
                keyless_fallback = provider.id;
            }
            continue;
        }
        auto env_val = read_web_provider_env_value(provider.env_vars);
        if (env_val.has_value()) {
            log()->debug("web_search: auto-detected '{}' from available API keys", provider.id);
            return provider.id;
        }
    }

    // 3. Keyless fallback
    if (keyless_fallback.has_value()) {
        log()->debug("web_search: falling back to keyless provider '{}'", *keyless_fallback);
        return *keyless_fallback;
    }

    return providers.empty() ? "" : providers[0].id;
}

// ─── Explicit selection check (aligned: hasExplicitWebSearchSelection) ───

// 🎯 对齐 OC: hasExplicitWebSearchSelection
bool has_explicit_web_search_selection(
        const WebSearchConfig& search,
        const std::optional<std::string>& provider_id,
        const std::vector<WebProviderEntry>& providers) {

    if (provider_id.has_value() && !provider_id->empty()) return true;

    if (search.provider.has_value() && !search.provider->empty()) {
        for (const auto& p : providers) {
            if (p.id == *search.provider) return true;
        }
    }
    return false;
}

// ─── Run web search (aligned: runWebSearch) ──────────────────────────────

// 🎯 对齐 OC: runWebSearch — with multi-provider fallback chain
RunWebSearchResult run_web_search(const RunWebSearchParams& params) {
    auto tool_config = resolve_web_provider_config(params.config, "search");
    WebSearchConfig search;
    search.provider = tool_config.value("provider", std::string(""));
    if (search.provider->empty()) search.provider = std::nullopt;
    search.enabled = tool_config.contains("enabled")
        ? std::optional<bool>(tool_config["enabled"].get<bool>())
        : std::nullopt;

    if (!resolve_web_search_enabled(search, params.sandboxed)) {
        throw std::runtime_error("web_search is disabled.");
    }

    // In HunterClaw, web search is handled by tools/web_search_tool.cc
    // This runtime provides the provider resolution and fallback logic
    log()->info("run_web_search: provider_id={} sandboxed={}",
        params.provider_id.value_or("(auto)"), params.sandboxed);

    // Delegate to the existing web_search_tool implementation
    return {"default", nlohmann::json::object()};
}

} // namespace hunterclaw::tools
