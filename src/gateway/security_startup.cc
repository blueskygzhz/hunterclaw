// 🎯 对齐 OpenClaw: src/gateway/server-startup-log.ts
// collectEnabledInsecureOrDangerousFlags 调用点
// 在 gateway 启动时检查危险配置标志并输出警告

#include "hunterclaw/security/audit.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;

// ============================================================================
// collectEnabledInsecureOrDangerousFlags
// 严格对齐 OpenClaw: src/security/dangerous-config-flags.ts
// ============================================================================

std::vector<std::string> collect_enabled_insecure_or_dangerous_flags(const json& cfg) {
    std::vector<std::string> enabled_flags;

    // 🎯 对齐 OpenClaw: cfg.gateway?.controlUi?.allowInsecureAuth === true
    if (cfg.contains("gateway") && cfg["gateway"].contains("controlUi")) {
        const auto& ui = cfg["gateway"]["controlUi"];
        if (ui.value("allowInsecureAuth", false)) {
            enabled_flags.push_back("gateway.controlUi.allowInsecureAuth=true");
        }
        if (ui.value("dangerouslyAllowHostHeaderOriginFallback", false)) {
            enabled_flags.push_back("gateway.controlUi.dangerouslyAllowHostHeaderOriginFallback=true");
        }
        if (ui.value("dangerouslyDisableDeviceAuth", false)) {
            enabled_flags.push_back("gateway.controlUi.dangerouslyDisableDeviceAuth=true");
        }
    }

    // 🎯 对齐 OpenClaw: cfg.hooks?.gmail?.allowUnsafeExternalContent === true
    if (cfg.contains("hooks")) {
        const auto& hooks = cfg["hooks"];
        if (hooks.contains("gmail") && hooks["gmail"].value("allowUnsafeExternalContent", false)) {
            enabled_flags.push_back("hooks.gmail.allowUnsafeExternalContent=true");
        }
        // 🎯 对齐 OpenClaw: hooks.mappings[i].allowUnsafeExternalContent
        if (hooks.contains("mappings") && hooks["mappings"].is_array()) {
            int index = 0;
            for (const auto& mapping : hooks["mappings"]) {
                if (mapping.value("allowUnsafeExternalContent", false)) {
                    enabled_flags.push_back("hooks.mappings[" + std::to_string(index) +
                                           "].allowUnsafeExternalContent=true");
                }
                index++;
            }
        }
    }

    // 🎯 对齐 OpenClaw: cfg.tools?.exec?.applyPatch?.workspaceOnly === false
    if (cfg.contains("tools") && cfg["tools"].contains("exec") &&
        cfg["tools"]["exec"].contains("applyPatch")) {
        if (cfg["tools"]["exec"]["applyPatch"].value("workspaceOnly", true) == false) {
            enabled_flags.push_back("tools.exec.applyPatch.workspaceOnly=false");
        }
    }

    return enabled_flags;
}

// ============================================================================
// log_gateway_startup_security_warnings
// 严格对齐 OpenClaw: logGatewayStartup → collectEnabledInsecureOrDangerousFlags
// ============================================================================

void log_gateway_startup_security_warnings(const json& cfg) {
    auto enabled_flags = collect_enabled_insecure_or_dangerous_flags(cfg);
    if (!enabled_flags.empty()) {
        std::string flags_str;
        for (size_t i = 0; i < enabled_flags.size(); ++i) {
            if (i > 0) flags_str += ", ";
            flags_str += enabled_flags[i];
        }
        auto logger = spdlog::get("default");
        if (logger) {
            logger->warn("security warning: dangerous config flags enabled: {}. "
                         "Run `hunterclaw security audit`.", flags_str);
        }
    }
}

}  // namespace gateway
}  // namespace hunterclaw
