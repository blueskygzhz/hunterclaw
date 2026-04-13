// 🎯 Strict alignment: OpenClaw web/provider-runtime-shared.ts
// Shared runtime logic for web-search and web-fetch providers —
// config extraction, credential resolution, provider definition pipeline.

#include <hunterclaw/tools/tool.h>
#include <hunterclaw/config/config_system.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw::tools {

namespace {
auto log() { return core::get_logger("web-provider"); }
} // namespace

// ─── Types (aligned: web/provider-runtime-shared.ts) ─────────────────────

struct WebProviderEntry {
    std::string id;
    std::string label;
    std::optional<std::string> hint;
    std::optional<std::string> docs_url;
    std::vector<std::string> env_vars;
    bool requires_credential = true;
    std::optional<std::string> credential_path;
    std::optional<std::string> credential_label;
    std::optional<std::string> signup_url;
    std::optional<std::string> placeholder;
    std::optional<std::string> plugin_id;
};

struct WebProviderToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json parameters;
    std::function<nlohmann::json(const nlohmann::json&)> execute;
};

struct RuntimeWebProviderMetadata {
    std::optional<std::string> provider_configured;
    std::optional<std::string> selected_provider;
    std::optional<std::string> provider_source;
};

// ─── Config extraction (aligned: resolveWebProviderConfig) ───────────────

// 🎯 对齐 OC: resolveWebProviderConfig<TKind, TConfig>
nlohmann::json resolve_web_provider_config(
        const nlohmann::json& config,
        const std::string& kind) {
    // kind is "search" or "fetch"
    auto web_config = config.value("/tools/web"_json_pointer, nlohmann::json::object());
    if (!web_config.is_object()) return nlohmann::json{};

    auto tool_config = web_config.value(kind, nlohmann::json::object());
    if (!tool_config.is_object()) return nlohmann::json{};

    return tool_config;
}

// ─── Credential reading (aligned: readWebProviderEnvValue) ───────────────

// 🎯 对齐 OC: readWebProviderEnvValue
std::optional<std::string> read_web_provider_env_value(
        const std::vector<std::string>& env_vars) {
    for (const auto& var : env_vars) {
        auto val = std::getenv(var.c_str());
        if (val && std::string(val).find_first_not_of(' ') != std::string::npos) {
            return std::string(val);
        }
    }
    return std::nullopt;
}

// ─── Credential presence check (aligned: providerRequiresCredential) ─────

// 🎯 对齐 OC: providerRequiresCredential
bool provider_requires_credential(const WebProviderEntry& provider) {
    return provider.requires_credential;
}

// ─── Has credential check (aligned: hasWebProviderEntryCredential) ───────

// 🎯 对齐 OC: hasWebProviderEntryCredential
// Generic credential presence check with pluggable resolution
bool has_web_provider_entry_credential(
        const WebProviderEntry& provider,
        const nlohmann::json& config,
        const nlohmann::json& tool_config,
        std::function<std::optional<std::string>(const WebProviderEntry&, const nlohmann::json&)> resolve_raw,
        std::function<std::optional<std::string>(const WebProviderEntry&)> resolve_env) {

    if (!provider_requires_credential(provider)) {
        return true; // No credential needed
    }

    // 1. Check config-stored value
    auto raw = resolve_raw(provider, config);
    if (raw.has_value() && !raw->empty()) {
        // Check if it's a SecretRef (non-env source)
        // In HC, secret refs are stored as JSON objects with "source" field
        // For simplicity, treat any non-empty string as "has credential"
        return true;
    }

    // 2. Check env var
    auto from_env = resolve_env(provider);
    if (from_env.has_value()) {
        return true;
    }

    return false;
}

// ─── Provider definition resolution (aligned: resolveWebProviderDefinition) ─

struct ResolveWebProviderParams {
    nlohmann::json config;
    nlohmann::json tool_config;
    std::optional<RuntimeWebProviderMetadata> runtime_metadata;
    bool sandboxed = false;
    std::optional<std::string> provider_id;
    std::vector<WebProviderEntry> providers;

    // Callbacks
    std::function<bool(const nlohmann::json&, bool)> resolve_enabled;
    std::function<std::string(const nlohmann::json&, const nlohmann::json&,
                              const std::vector<WebProviderEntry>&)> resolve_auto_provider_id;
    std::function<std::optional<std::string>(const nlohmann::json&, const nlohmann::json&,
                              const std::vector<WebProviderEntry>&, const std::string&)> resolve_fallback_provider_id;
    std::function<std::optional<WebProviderToolDefinition>(
        const WebProviderEntry&, const nlohmann::json&,
        const nlohmann::json&, const std::optional<RuntimeWebProviderMetadata>&)> create_tool;
};

struct ResolveWebProviderResult {
    bool found = false;
    WebProviderEntry provider;
    WebProviderToolDefinition definition;
};

// 🎯 对齐 OC: resolveWebProviderDefinition
std::optional<ResolveWebProviderResult> resolve_web_provider_definition(
        const ResolveWebProviderParams& params) {

    // 1. Check enabled
    if (!params.resolve_enabled(params.tool_config, params.sandboxed)) {
        return std::nullopt;
    }

    // 2. Filter providers
    if (params.providers.empty()) {
        return std::nullopt;
    }

    // 3. Resolve provider ID
    auto auto_id = params.resolve_auto_provider_id(
        params.config, params.tool_config, params.providers);

    std::string provider_id;
    if (params.provider_id.has_value()) {
        provider_id = *params.provider_id;
    } else if (params.runtime_metadata.has_value() &&
               params.runtime_metadata->selected_provider.has_value()) {
        provider_id = *params.runtime_metadata->selected_provider;
    } else {
        provider_id = auto_id;
    }

    if (provider_id.empty()) {
        return std::nullopt;
    }

    // 4. Find provider
    const WebProviderEntry* found = nullptr;
    for (const auto& entry : params.providers) {
        if (entry.id == provider_id) {
            found = &entry;
            break;
        }
    }

    // Try fallback
    if (!found && params.resolve_fallback_provider_id) {
        auto fallback_id = params.resolve_fallback_provider_id(
            params.config, params.tool_config, params.providers, provider_id);
        if (fallback_id.has_value()) {
            for (const auto& entry : params.providers) {
                if (entry.id == *fallback_id) {
                    found = &entry;
                    break;
                }
            }
        }
    }

    if (!found) {
        return std::nullopt;
    }

    // 5. Create tool definition
    auto definition = params.create_tool(
        *found, params.config, params.tool_config, params.runtime_metadata);
    if (!definition.has_value()) {
        return std::nullopt;
    }

    return ResolveWebProviderResult{true, *found, *definition};
}

} // namespace hunterclaw::tools
