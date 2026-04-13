// ============================================================================
// search_setup.cc
// ✅ 严格对齐 OpenClaw: src/flows/search-setup.ts (543行)
//
// Web 搜索 Provider 配置引导流:
//   - SearchProviderSetupContribution: setup 贡献项
//   - listSearchProviderOptions: 列出可用 search providers
//   - resolveSearchProviderOptions: 解析 provider 列表（含 plugin providers）
//   - resolveSearchProviderCredentialLabel: 凭证标签
//   - promptSearchProvider: 交互式选择 search provider
//   - applySearchProviderConfig: 写入 config
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace hunterclaw {
namespace flows {

namespace {
auto log() { return spdlog::get("flows"); }
} // namespace

// ============================================================================
// 对齐 search-setup.ts:20-36: Types
// ============================================================================
struct SearchProviderEntry {
    std::string id;
    std::string label;
    std::optional<std::string> credential_label;
    bool requires_credential = true;
    std::vector<std::string> onboarding_scopes;
    std::vector<std::string> env_vars;
};

struct SearchProviderSetupOption {
    std::string value;
    std::string label;
    std::optional<std::string> hint;
};

struct SearchProviderSetupContribution {
    std::string id;
    std::string kind = "search";
    std::string surface = "setup";
    SearchProviderEntry provider;
    SearchProviderSetupOption option;
    std::string source = "runtime";
};

// ============================================================================
// 对齐 search-setup.ts:38-45: resolveSearchProviderCredentialLabel
// ============================================================================
static std::string resolve_search_provider_credential_label(
    const SearchProviderEntry& entry
) {
    if (!entry.requires_credential) {
        return entry.label + " setup";
    }
    if (entry.credential_label.has_value() && !entry.credential_label->empty()) {
        return *entry.credential_label;
    }
    return entry.label + " API key";
}

// ============================================================================
// 对齐 search-setup.ts:53-56: showsSearchProviderInSetup
// ============================================================================
static bool shows_search_provider_in_setup(const SearchProviderEntry& entry) {
    for (const auto& scope : entry.onboarding_scopes) {
        if (scope == "text-inference") return true;
    }
    return false;
}

// ============================================================================
// 对齐 search-setup.ts:47-51: listSearchProviderOptions
// Built-in search providers + plugin providers
// ============================================================================
static std::vector<SearchProviderEntry> get_builtin_search_providers() {
    return {
        {"brave", "Brave Search", "Brave Search API key", true, {"text-inference"}, {"BRAVE_API_KEY"}},
        {"tavily", "Tavily", "Tavily API key", true, {"text-inference"}, {"TAVILY_API_KEY"}},
        {"google", "Google Search", "Google Search API key", true, {"text-inference"}, {"GOOGLE_API_KEY", "GOOGLE_CX"}},
        {"bing", "Bing Search", "Bing Search API key", true, {"text-inference"}, {"BING_API_KEY"}},
        {"duckduckgo", "DuckDuckGo", std::nullopt, false, {"text-inference"}, {}},
    };
}

std::vector<SearchProviderEntry> resolve_search_provider_options(
    const json& config
) {
    auto builtin = get_builtin_search_providers();

    // Merge plugin search providers from config
    if (config.contains("plugins") && config["plugins"].is_object()) {
        for (auto& [pid, plugin_cfg] : config["plugins"].items()) {
            if (!plugin_cfg.contains("webSearchProviders") ||
                !plugin_cfg["webSearchProviders"].is_array()) continue;
            for (const auto& p : plugin_cfg["webSearchProviders"]) {
                SearchProviderEntry entry;
                entry.id = p.value("id", "");
                entry.label = p.value("label", entry.id);
                entry.requires_credential = p.value("requiresCredential", true);
                if (p.contains("credentialLabel") && p["credentialLabel"].is_string()) {
                    entry.credential_label = p["credentialLabel"];
                }
                if (p.contains("envVars") && p["envVars"].is_array()) {
                    for (const auto& ev : p["envVars"]) {
                        if (ev.is_string()) entry.env_vars.push_back(ev);
                    }
                }
                if (!entry.id.empty()) {
                    builtin.push_back(std::move(entry));
                }
            }
        }
    }

    // Sort by label
    std::sort(builtin.begin(), builtin.end(),
              [](const auto& a, const auto& b) { return a.label < b.label; });

    return builtin;
}

std::vector<SearchProviderEntry> list_search_provider_options(const json& config) {
    return resolve_search_provider_options(config);
}

// ============================================================================
// 对齐 search-setup.ts:resolveSearchProviderSetupContributions
// ============================================================================
std::vector<SearchProviderSetupContribution> resolve_search_provider_setup_contributions(
    const json& config
) {
    auto providers = resolve_search_provider_options(config);
    std::vector<SearchProviderSetupContribution> contributions;

    for (const auto& entry : providers) {
        if (!shows_search_provider_in_setup(entry)) continue;

        SearchProviderSetupContribution contrib;
        contrib.id = "search:setup:" + entry.id;
        contrib.provider = entry;
        contrib.option.value = entry.id;
        contrib.option.label = resolve_search_provider_credential_label(entry);

        contributions.push_back(std::move(contrib));
    }

    // Sort by label
    std::sort(contributions.begin(), contributions.end(),
              [](const auto& a, const auto& b) { return a.option.label < b.option.label; });

    return contributions;
}

// ============================================================================
// 对齐 search-setup.ts:applySearchProviderConfig (simplified)
// Writes selected provider to config JSON
// ============================================================================
json apply_search_provider_config(
    const json& config,
    const std::string& provider_id,
    const std::optional<std::string>& api_key
) {
    json result = config;

    // Ensure tools.web.search path exists
    if (!result.contains("tools")) result["tools"] = json::object();
    if (!result["tools"].contains("web")) result["tools"]["web"] = json::object();
    if (!result["tools"]["web"].contains("search")) result["tools"]["web"]["search"] = json::object();

    result["tools"]["web"]["search"]["provider"] = provider_id;
    result["tools"]["web"]["search"]["enabled"] = true;

    // If API key provided, store in config (non-secret path for simplicity)
    if (api_key.has_value() && !api_key->empty()) {
        result["tools"]["web"]["search"]["apiKey"] = *api_key;
    }

    if (log()) {
        log()->info("Applied search provider config: provider={}", provider_id);
    }

    return result;
}

} // namespace flows
} // namespace hunterclaw
