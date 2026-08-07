// ============================================================================
// dangerous_tools.cc - Shared tool-risk constants
// 🎯 严格对齐 OpenClaw: src/security/dangerous-tools.ts
// ============================================================================

#include "hunterclaw/security/dangerous_tools.h"

#include <algorithm>
#include <cctype>

namespace hunterclaw {
namespace security {

namespace {

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool contains_ci(const std::vector<std::string>& list, const std::string& name) {
    const std::string lower = to_lower(name);
    for (const auto& item : list) {
        if (to_lower(item) == lower) return true;
    }
    return false;
}

}  // namespace

// 🎯 对齐 OpenClaw DEFAULT_GATEWAY_HTTP_TOOL_DENY (dangerous-tools.ts:10-46)
const std::vector<std::string>& default_gateway_http_tool_deny() {
    static const std::vector<std::string> kList = {
        "exec",                // Direct command execution — immediate RCE surface
        "spawn",               // Arbitrary child process creation — immediate RCE surface
        "shell",               // Shell command execution — immediate RCE surface
        "fs_write",            // Arbitrary file mutation on the host
        "fs_delete",           // Arbitrary file deletion on the host
        "fs_move",             // Arbitrary file move/rename on the host
        "apply_patch",         // Patch application can rewrite arbitrary files
        "terminal",            // Agent-owned host terminal — interactive RCE surface
        "sessions_spawn",      // Session orchestration — spawning agents remotely is RCE
        "sessions_send",       // Cross-session injection
        "conversations_list",  // External conversation discovery (server credentials)
        "conversations_send",
        "conversations_turn",
        "automations",         // Persistent automation control plane
        "cron",                // Legacy alias of automations (openclaw LEGACY_AUTOMATIONS_TOOL_NAMES)
        "gateway",             // Gateway config can expose secrets and host topology
        "nodes",               // Node command relay can reach system.run on paired hosts
        "computer",            // Desktop control on a paired Mac
        "mobile_ui",           // Android AccessibilityService reads and UI control
        "openclaw",
    };
    return kList;
}

// 🎯 对齐 OpenClaw GATEWAY_CONTROL_PLANE_TOOLS (dangerous-tools.ts:52)
const std::vector<std::string>& gateway_control_plane_tools() {
    static const std::vector<std::string> kList = {
        "automations",
        "cron",  // legacy alias
        "gateway",
    };
    return kList;
}

// 🎯 对齐 OpenClaw GATEWAY_OWNER_ONLY_CORE_TOOLS (dangerous-tools.ts:59-71)
const std::vector<std::string>& gateway_owner_only_core_tools() {
    static const std::vector<std::string> kList = {
        // ...GATEWAY_CONTROL_PLANE_TOOLS
        "automations",
        "cron",  // legacy alias
        "gateway",
        // core owner-only tools
        "sessions",
        "screen",
        "terminal",
        "conversations_list",
        "conversations_send",
        "conversations_turn",
        "nodes",
        "computer",
        "mobile_ui",
        "openclaw",
    };
    return kList;
}

bool is_gateway_owner_only_core_tool(const std::string& tool_name) {
    return contains_ci(gateway_owner_only_core_tools(), tool_name);
}

bool is_default_gateway_http_tool_denied(const std::string& tool_name) {
    return contains_ci(default_gateway_http_tool_deny(), tool_name);
}

}  // namespace security
}  // namespace hunterclaw
