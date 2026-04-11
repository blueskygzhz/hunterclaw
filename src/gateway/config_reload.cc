// ============================================================================
// config_reload.cc - Hot Configuration Reload
// 🎯 严格对齐 OpenClaw gateway/config-reload.ts (280 行) + config-reload-plan.ts (200 行)
//
// 核心功能:
//   - diffConfigPaths: 深度比较配置变更路径
//   - resolveGatewayReloadSettings: 解析重载设置 (mode/debounce)
//   - buildGatewayReloadPlan: 构建重载计划 (channels/cron/restart)
//   - 支持 hybrid/graceful/none 三种重载模式
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ============================================================================
// diffConfigPaths (对齐 config-reload.ts:28-57)
// ============================================================================

std::vector<std::string> diff_config_paths(const json& prev, const json& next, const std::string& prefix) {
    if (prev == next) return {};
    if (prev.is_object() && next.is_object()) {
        std::set<std::string> keys;
        for (auto& [k, _] : prev.items()) keys.insert(k);
        for (auto& [k, _] : next.items()) keys.insert(k);
        std::vector<std::string> paths;
        for (const auto& key : keys) {
            auto p = prev.contains(key) ? prev[key] : json(nullptr);
            auto n = next.contains(key) ? next[key] : json(nullptr);
            if (p == json(nullptr) && n == json(nullptr)) continue;
            std::string child = prefix.empty() ? key : (prefix + "." + key);
            auto child_paths = diff_config_paths(p, n, child);
            paths.insert(paths.end(), child_paths.begin(), child_paths.end());
        }
        return paths;
    }
    if (prev.is_array() && next.is_array()) {
        if (prev == next) return {};
    }
    return {prefix.empty() ? "<root>" : prefix};
}

// ============================================================================
// Reload Settings (对齐 config-reload.ts:59-85)
// ============================================================================

struct GatewayReloadSettings {
    std::string mode = "hybrid";  // "hybrid" | "graceful" | "none"
    int debounce_ms = 300;
};

GatewayReloadSettings resolve_gateway_reload_settings(const json& config) {
    GatewayReloadSettings settings;
    if (config.contains("gateway") && config["gateway"].is_object()) {
        const auto& gw = config["gateway"];
        if (gw.contains("reload") && gw["reload"].is_object()) {
            const auto& reload = gw["reload"];
            std::string mode = reload.value("mode", "hybrid");
            if (mode == "graceful" || mode == "none" || mode == "hybrid") {
                settings.mode = mode;
            }
            if (reload.contains("debounceMs") && reload["debounceMs"].is_number()) {
                settings.debounce_ms = std::max(50, reload["debounceMs"].get<int>());
            }
        }
    }
    return settings;
}

// ============================================================================
// Reload Plan (对齐 config-reload-plan.ts:95-200)
// ============================================================================

struct GatewayReloadPlan {
    bool requires_restart = false;
    bool channels_changed = false;
    bool cron_changed = false;
    bool auth_changed = false;
    bool memory_changed = false;
    bool plugins_changed = false;
    std::vector<std::string> changed_paths;
    std::string reason;
};

static bool path_starts_with(const std::string& path, const std::string& prefix) {
    return path == prefix || (path.size() > prefix.size() && path.substr(0, prefix.size() + 1) == prefix + ".");
}

GatewayReloadPlan build_gateway_reload_plan(
    const json& prev_config,
    const json& next_config
) {
    GatewayReloadPlan plan;
    plan.changed_paths = diff_config_paths(prev_config, next_config, "");
    if (plan.changed_paths.empty()) return plan;

    // Classify changes
    for (const auto& path : plan.changed_paths) {
        if (path_starts_with(path, "channels")) plan.channels_changed = true;
        if (path_starts_with(path, "cron")) plan.cron_changed = true;
        if (path_starts_with(path, "gateway.auth") || path_starts_with(path, "gateway.token") ||
            path_starts_with(path, "gateway.password")) plan.auth_changed = true;
        if (path_starts_with(path, "memory")) plan.memory_changed = true;
        if (path_starts_with(path, "plugins")) plan.plugins_changed = true;
    }

    // Determine if restart needed (对齐 config-reload-plan.ts restart heuristic)
    // Paths that require restart: gateway.port, gateway.tls, agents.defaults.*
    static const std::vector<std::string> restart_prefixes = {
        "gateway.port", "gateway.tls", "gateway.host",
        "agents.defaults.model", "agents.defaults.provider",
        "agents.list",
    };
    for (const auto& path : plan.changed_paths) {
        for (const auto& rp : restart_prefixes) {
            if (path_starts_with(path, rp)) {
                plan.requires_restart = true;
                plan.reason = "config path '" + path + "' requires gateway restart";
                break;
            }
        }
        if (plan.requires_restart) break;
    }

    return plan;
}

}  // namespace gateway
}  // namespace hunterclaw
