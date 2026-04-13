// ============================================================================
// provider_flow.cc
// ✅ 严格对齐 OpenClaw: src/flows/provider-flow.ts (157行)
//
// Provider Flow 抽象层：
//   - ProviderFlowScope: text-inference | image-generation
//   - ProviderSetupFlowContribution: setup surface 贡献项
//   - ProviderModelPickerFlowContribution: model-picker surface 贡献项
//   - resolveProviderSetupFlowContributions: 解析 provider setup options
//   - resolveProviderModelPickerFlowContributions: 解析 model-picker entries
//   - sortFlowContributionsByLabel: 按 label 排序
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
// 对齐 provider-flow.ts:12-14: ProviderFlowScope
// ============================================================================
enum class ProviderFlowScope {
    TextInference,
    ImageGeneration,
};

static std::string scope_to_string(ProviderFlowScope scope) {
    switch (scope) {
        case ProviderFlowScope::TextInference: return "text-inference";
        case ProviderFlowScope::ImageGeneration: return "image-generation";
    }
    return "text-inference";
}

static const ProviderFlowScope DEFAULT_PROVIDER_FLOW_SCOPE = ProviderFlowScope::TextInference;

// ============================================================================
// 对齐 provider-flow.ts types: FlowOption, FlowContribution
// ============================================================================
struct FlowOption {
    std::string value;
    std::string label;
    std::optional<std::string> hint;
    std::optional<int> assistant_priority;
    std::optional<std::string> assistant_visibility;
    struct Group {
        std::string id;
        std::string label;
        std::optional<std::string> hint;
    };
    std::optional<Group> group;
    struct Docs {
        std::string path;
    };
    std::optional<Docs> docs;
};

struct FlowContribution {
    std::string id;
    std::string kind;
    std::string surface;
    std::string source;
    int sort_priority = 0;  // for sorting
};

// ============================================================================
// 对齐 provider-flow.ts:16-38: ProviderSetupFlowContribution
// ============================================================================
struct ProviderSetupFlowContribution : FlowContribution {
    std::string provider_id;
    std::optional<std::string> plugin_id;
    FlowOption option;
    std::vector<ProviderFlowScope> onboarding_scopes;
};

struct ProviderModelPickerFlowContribution : FlowContribution {
    std::string provider_id;
    FlowOption option;
};

// ============================================================================
// 对齐 provider-flow.ts:40-45: includesProviderFlowScope
// ============================================================================
bool includes_provider_flow_scope(
    const std::vector<ProviderFlowScope>& scopes,
    ProviderFlowScope scope
) {
    if (scopes.empty()) return scope == DEFAULT_PROVIDER_FLOW_SCOPE;
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

// ============================================================================
// 对齐 types.ts: sortFlowContributionsByLabel
// ============================================================================
template<typename T>
std::vector<T> sort_flow_contributions_by_label(std::vector<T> contributions) {
    std::sort(contributions.begin(), contributions.end(),
              [](const T& a, const T& b) {
                  return a.option.label < b.option.label;
              });
    return contributions;
}

// ============================================================================
// 对齐 provider-flow.ts:66-114: resolveProviderSetupFlowContributions
// ============================================================================
std::vector<ProviderSetupFlowContribution> resolve_provider_setup_flow_contributions(
    const json& config,
    ProviderFlowScope scope
) {
    std::vector<ProviderSetupFlowContribution> contributions;

    // Extract provider list from config.providers or config.plugins
    if (!config.contains("providers") || !config["providers"].is_array()) {
        return contributions;
    }

    for (const auto& provider : config["providers"]) {
        if (!provider.contains("id") || !provider["id"].is_string()) continue;

        std::string provider_id = provider["id"];
        std::string label = provider.value("label", provider_id);
        std::string hint = provider.value("hint", "");

        // Check onboarding scopes
        std::vector<ProviderFlowScope> scopes;
        if (provider.contains("onboardingScopes") && provider["onboardingScopes"].is_array()) {
            for (const auto& s : provider["onboardingScopes"]) {
                if (s == "text-inference") scopes.push_back(ProviderFlowScope::TextInference);
                else if (s == "image-generation") scopes.push_back(ProviderFlowScope::ImageGeneration);
            }
        }

        if (!includes_provider_flow_scope(scopes, scope)) continue;

        ProviderSetupFlowContribution contrib;
        contrib.id = "provider:setup:" + provider_id;
        contrib.kind = "provider";
        contrib.surface = "setup";
        contrib.source = "runtime";
        contrib.provider_id = provider_id;
        contrib.option.value = provider_id;
        contrib.option.label = label;
        if (!hint.empty()) contrib.option.hint = hint;
        contrib.onboarding_scopes = scopes;

        contributions.push_back(std::move(contrib));
    }

    return sort_flow_contributions_by_label(contributions);
}

// ============================================================================
// 对齐 provider-flow.ts:116-154: resolveProviderModelPickerFlowContributions
// ============================================================================
std::vector<ProviderModelPickerFlowContribution> resolve_provider_model_picker_flow_contributions(
    const json& config
) {
    std::vector<ProviderModelPickerFlowContribution> contributions;

    if (!config.contains("providers") || !config["providers"].is_array()) {
        return contributions;
    }

    for (const auto& provider : config["providers"]) {
        if (!provider.contains("id") || !provider["id"].is_string()) continue;

        std::string provider_id = provider["id"];
        std::string label = provider.value("label", provider_id);

        ProviderModelPickerFlowContribution contrib;
        contrib.id = "provider:model-picker:" + provider_id;
        contrib.kind = "provider";
        contrib.surface = "model-picker";
        contrib.source = "runtime";
        contrib.provider_id = provider_id;
        contrib.option.value = provider_id;
        contrib.option.label = label;

        contributions.push_back(std::move(contrib));
    }

    return sort_flow_contributions_by_label(contributions);
}

// ============================================================================
// Convenience: resolveProviderSetupFlowOptions (对齐 L66-73)
// ============================================================================
std::vector<FlowOption> resolve_provider_setup_flow_options(
    const json& config,
    ProviderFlowScope scope
) {
    auto contribs = resolve_provider_setup_flow_contributions(config, scope);
    std::vector<FlowOption> options;
    options.reserve(contribs.size());
    for (auto& c : contribs) options.push_back(std::move(c.option));
    return options;
}

// ============================================================================
// Convenience: resolveProviderModelPickerFlowEntries (对齐 L116-124)
// ============================================================================
std::vector<FlowOption> resolve_provider_model_picker_flow_entries(const json& config) {
    auto contribs = resolve_provider_model_picker_flow_contributions(config);
    std::vector<FlowOption> entries;
    entries.reserve(contribs.size());
    for (auto& c : contribs) entries.push_back(std::move(c.option));
    return entries;
}

} // namespace flows
} // namespace hunterclaw
