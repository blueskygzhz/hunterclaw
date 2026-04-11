#include "hunterclaw/hooks/hook_policy.h"
#include <algorithm>
#include <unordered_map>

namespace hunterclaw {
namespace hooks {

// ============================================================================
// Hook Source Policy Definitions (对齐 policy.ts:26-55)
// ============================================================================

HookSourcePolicy get_hook_source_policy(HookSource source) {
    switch (source) {
        case HookSource::OPENCLAW_BUNDLED:
            return {10, true, "default-on",
                    {HookSource::OPENCLAW_BUNDLED},
                    {HookSource::OPENCLAW_MANAGED, HookSource::OPENCLAW_PLUGIN}};
        case HookSource::OPENCLAW_PLUGIN:
            return {20, true, "default-on",
                    {HookSource::OPENCLAW_BUNDLED, HookSource::OPENCLAW_PLUGIN},
                    {HookSource::OPENCLAW_MANAGED}};
        case HookSource::OPENCLAW_MANAGED:
            return {30, true, "default-on",
                    {HookSource::OPENCLAW_BUNDLED, HookSource::OPENCLAW_MANAGED, HookSource::OPENCLAW_PLUGIN},
                    {HookSource::OPENCLAW_MANAGED}};
        case HookSource::OPENCLAW_WORKSPACE:
            return {40, true, "explicit-opt-in",
                    {HookSource::OPENCLAW_WORKSPACE},
                    {HookSource::OPENCLAW_WORKSPACE}};
        default:
            return {100, false, "default-on", {}, {}};
    }
}

// ============================================================================
// Hook Config Resolution (对齐 policy.ts:61-74)
// ============================================================================

std::optional<core::JsonValue> resolve_hook_config(
    const std::optional<core::Config>& config,
    const std::string& hook_key
) {
    if (!config) return std::nullopt;
    try {
        const auto& cfg_json = config->data;
        if (!cfg_json.contains("hooks")) return std::nullopt;
        const auto& hooks = cfg_json["hooks"];
        if (!hooks.contains("internal")) return std::nullopt;
        const auto& internal = hooks["internal"];
        if (!internal.contains("entries")) return std::nullopt;
        const auto& entries = internal["entries"];
        if (!entries.contains(hook_key)) return std::nullopt;
        return entries[hook_key];
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// Hook Enable State (对齐 policy.ts:76-98)
// ============================================================================

HookEnableState resolve_hook_enable_state(
    const HookEntry& entry,
    const std::optional<core::Config>& config
) {
    // Plugin hooks always enabled
    if (entry.hook.source == HookSource::OPENCLAW_PLUGIN) {
        return {true, HookEnableStateReason::NONE};
    }
    
    // Check config override
    std::string hook_key = entry.hook.name;
    if (entry.metadata && entry.metadata->hook_key) {
        hook_key = *entry.metadata->hook_key;
    }
    auto hook_config = resolve_hook_config(config, hook_key);
    if (hook_config && hook_config->contains("enabled") 
        && (*hook_config)["enabled"].is_bool() 
        && (*hook_config)["enabled"].get<bool>() == false) {
        return {false, HookEnableStateReason::DISABLED_IN_CONFIG};
    }
    
    // Check source default enable mode
    auto policy = get_hook_source_policy(entry.hook.source);
    if (policy.default_enable_mode == "explicit-opt-in") {
        bool explicitly_enabled = hook_config && hook_config->contains("enabled") 
                                  && (*hook_config)["enabled"].is_bool()
                                  && (*hook_config)["enabled"].get<bool>() == true;
        if (!explicitly_enabled) {
            return {false, HookEnableStateReason::WORKSPACE_HOOK_DISABLED_BY_DEFAULT};
        }
    }
    
    return {true, HookEnableStateReason::NONE};
}

// ============================================================================
// Hook Override Check (对齐 policy.ts:100-107)
// ============================================================================

bool can_override_hook(const HookEntry& candidate, const HookEntry& existing) {
    auto candidate_policy = get_hook_source_policy(candidate.hook.source);
    auto existing_policy = get_hook_source_policy(existing.hook.source);
    
    auto contains = [](const std::vector<HookSource>& vec, HookSource s) {
        return std::find(vec.begin(), vec.end(), s) != vec.end();
    };
    
    return contains(candidate_policy.can_override, existing.hook.source) &&
           contains(existing_policy.can_be_overridden_by, candidate.hook.source);
}

// ============================================================================
// Hook Entry Resolution (对齐 policy.ts:109-143)
// ============================================================================

std::vector<HookEntry> resolve_hook_entries(
    const std::vector<HookEntry>& entries,
    CollisionCallback on_collision
) {
    // Sort by precedence, preserving insertion order for same precedence
    struct IndexedEntry {
        size_t index;
        const HookEntry* entry;
    };
    std::vector<IndexedEntry> ordered;
    ordered.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        ordered.push_back({i, &entries[i]});
    }
    std::sort(ordered.begin(), ordered.end(), [](const IndexedEntry& a, const IndexedEntry& b) {
        int pa = get_hook_source_policy(a.entry->hook.source).precedence;
        int pb = get_hook_source_policy(b.entry->hook.source).precedence;
        if (pa != pb) return pa < pb;
        return a.index < b.index;
    });
    
    // Merge with priority
    std::unordered_map<std::string, HookEntry> merged;
    std::vector<std::string> order; // maintain insertion order
    
    for (const auto& ie : ordered) {
        const auto& entry = *ie.entry;
        auto it = merged.find(entry.hook.name);
        if (it == merged.end()) {
            merged[entry.hook.name] = entry;
            order.push_back(entry.hook.name);
            continue;
        }
        if (can_override_hook(entry, it->second)) {
            it->second = entry;
            continue;
        }
        if (on_collision) {
            on_collision({entry.hook.name, it->second, entry});
        }
    }
    
    std::vector<HookEntry> result;
    result.reserve(order.size());
    for (const auto& name : order) {
        result.push_back(merged[name]);
    }
    return result;
}

}  // namespace hooks
}  // namespace hunterclaw
