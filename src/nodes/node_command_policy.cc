// 🎯 对齐 OpenClaw: gateway/node-command-policy.ts (完整实现)
// 平台命令策略、allowlist/denylist、危险命令集

#include "hunterclaw/nodes/node_command_policy.h"
#include <algorithm>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// 平台默认命令集 (对齐 OpenClaw line 76-119)
// ============================================================================

static std::vector<std::string> get_platform_defaults(PlatformId id) {
    std::vector<std::string> result;
    auto append = [&](const std::vector<std::string>& src) {
        result.insert(result.end(), src.begin(), src.end());
    };

    switch (id) {
        case PlatformId::Ios:
            // 🎯 对齐 OpenClaw: ios platform defaults (line 77-88)
            append(CANVAS_COMMANDS);
            append(CAMERA_COMMANDS);
            append(LOCATION_COMMANDS);
            append(DEVICE_COMMANDS);
            append(CONTACTS_COMMANDS);
            append(CALENDAR_COMMANDS);
            append(REMINDERS_COMMANDS);
            append(PHOTOS_COMMANDS);
            append(MOTION_COMMANDS);
            result.push_back(NODE_SYSTEM_NOTIFY_COMMAND);  // iOS: system.notify only
            break;

        case PlatformId::Android:
            // 🎯 对齐 OpenClaw: android platform defaults (line 89-102)
            append(CANVAS_COMMANDS);
            append(CAMERA_COMMANDS);
            append(LOCATION_COMMANDS);
            append(ANDROID_NOTIFICATION_COMMANDS);
            result.push_back(NODE_SYSTEM_NOTIFY_COMMAND);
            append(ANDROID_DEVICE_COMMANDS);
            append(CONTACTS_COMMANDS);
            append(CALENDAR_COMMANDS);
            append(CALL_LOG_COMMANDS);
            append(REMINDERS_COMMANDS);
            append(PHOTOS_COMMANDS);
            append(MOTION_COMMANDS);
            break;

        case PlatformId::MacOS:
            // 🎯 对齐 OpenClaw: macos platform defaults (line 103-114)
            append(CANVAS_COMMANDS);
            append(CAMERA_COMMANDS);
            append(LOCATION_COMMANDS);
            append(DEVICE_COMMANDS);
            append(CONTACTS_COMMANDS);
            append(CALENDAR_COMMANDS);
            append(REMINDERS_COMMANDS);
            append(PHOTOS_COMMANDS);
            append(MOTION_COMMANDS);
            // SYSTEM_COMMANDS
            for (const auto& cmd : NODE_SYSTEM_RUN_COMMANDS) result.push_back(cmd);
            result.push_back(NODE_SYSTEM_NOTIFY_COMMAND);
            result.push_back(NODE_BROWSER_PROXY_COMMAND);
            break;

        case PlatformId::Linux:
        case PlatformId::Windows:
            // 🎯 对齐 OpenClaw: linux/windows = SYSTEM_COMMANDS only (line 115-116)
            for (const auto& cmd : NODE_SYSTEM_RUN_COMMANDS) result.push_back(cmd);
            result.push_back(NODE_SYSTEM_NOTIFY_COMMAND);
            result.push_back(NODE_BROWSER_PROXY_COMMAND);
            break;

        case PlatformId::Unknown:
        default:
            // 🎯 对齐 OpenClaw: unknown = UNKNOWN_PLATFORM_COMMANDS (line 118)
            append(CANVAS_COMMANDS);
            append(CAMERA_COMMANDS);
            append(LOCATION_COMMANDS);
            result.push_back(NODE_SYSTEM_NOTIFY_COMMAND);
            break;
    }
    return result;
}

// ============================================================================
// normalizePlatformId (对齐 OpenClaw line 123-174)
// ============================================================================

// 🎯 对齐 OpenClaw: PLATFORM_PREFIX_RULES (line 123-132)
static PlatformId resolve_platform_by_prefix(const std::string& value) {
    if (value.empty()) return PlatformId::Unknown;
    if (value.substr(0, 3) == "ios") return PlatformId::Ios;
    if (value.substr(0, 7) == "android") return PlatformId::Android;
    if (value.substr(0, 3) == "mac" || value.substr(0, 6) == "darwin") return PlatformId::MacOS;
    if (value.substr(0, 3) == "win") return PlatformId::Windows;
    if (value.substr(0, 5) == "linux") return PlatformId::Linux;
    return PlatformId::Unknown;
}

// 🎯 对齐 OpenClaw: DEVICE_FAMILY_TOKEN_RULES (line 134-143)
static PlatformId resolve_platform_by_device_family(const std::string& family) {
    if (family.empty()) return PlatformId::Unknown;
    if (family.find("iphone") != std::string::npos ||
        family.find("ipad") != std::string::npos ||
        family.find("ios") != std::string::npos) return PlatformId::Ios;
    if (family.find("android") != std::string::npos) return PlatformId::Android;
    if (family.find("mac") != std::string::npos) return PlatformId::MacOS;
    if (family.find("windows") != std::string::npos) return PlatformId::Windows;
    if (family.find("linux") != std::string::npos) return PlatformId::Linux;
    return PlatformId::Unknown;
}

static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

PlatformId normalize_platform_id(const std::string& platform, const std::string& device_family) {
    // 🎯 对齐 OpenClaw line 165-174
    std::string raw = to_lower(platform);
    PlatformId by_platform = resolve_platform_by_prefix(raw);
    if (by_platform != PlatformId::Unknown) {
        return by_platform;
    }
    std::string family = to_lower(device_family);
    PlatformId by_family = resolve_platform_by_device_family(family);
    return (by_family != PlatformId::Unknown) ? by_family : PlatformId::Unknown;
}

// ============================================================================
// resolveNodeCommandAllowlist (对齐 OpenClaw line 176-192)
// ============================================================================

std::set<std::string> resolve_node_command_allowlist(
    const std::vector<std::string>& config_allow_commands,
    const std::vector<std::string>& config_deny_commands,
    const std::string& platform,
    const std::string& device_family) {

    PlatformId pid = normalize_platform_id(platform, device_family);
    auto base = get_platform_defaults(pid);

    // 🎯 对齐 OpenClaw: const allow = new Set([...base, ...extra].map(...))
    std::set<std::string> allow;
    for (const auto& cmd : base) {
        std::string trimmed = cmd;
        // trim
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
        if (!trimmed.empty()) allow.insert(trimmed);
    }
    for (const auto& cmd : config_allow_commands) {
        std::string trimmed = cmd;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
        if (!trimmed.empty()) allow.insert(trimmed);
    }

    // 🎯 对齐 OpenClaw: for (const blocked of deny) { allow.delete(trimmed) }
    for (const auto& blocked : config_deny_commands) {
        std::string trimmed = blocked;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
        if (!trimmed.empty()) allow.erase(trimmed);
    }

    return allow;
}

// ============================================================================
// normalizeDeclaredNodeCommands (对齐 OpenClaw line 211-218)
// ============================================================================

std::vector<std::string> normalize_declared_node_commands(
    const std::vector<std::string>& declared_commands,
    const std::set<std::string>& allowlist) {

    // 🎯 对齐 OpenClaw: normalizeDeclaredCommands + filter by allowlist
    std::set<std::string> seen;
    std::vector<std::string> normalized;
    for (const auto& cmd : declared_commands) {
        std::string trimmed = cmd;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
        if (trimmed.empty() || seen.count(trimmed)) continue;
        seen.insert(trimmed);
        if (allowlist.count(trimmed)) {
            normalized.push_back(trimmed);
        }
    }
    return normalized;
}

// ============================================================================
// isNodeCommandAllowed (对齐 OpenClaw line 220-240)
// ============================================================================

NodeCommandAllowedResult is_node_command_allowed(
    const std::string& command,
    const std::vector<std::string>& declared_commands,
    const std::set<std::string>& allowlist) {

    // 🎯 对齐 OpenClaw line 225-238
    std::string cmd = command;
    while (!cmd.empty() && (cmd.front() == ' ' || cmd.front() == '\t')) cmd.erase(cmd.begin());
    while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t')) cmd.pop_back();

    if (cmd.empty()) {
        return {false, "command required"};
    }
    if (!allowlist.count(cmd)) {
        return {false, "command not allowlisted"};
    }
    if (!declared_commands.empty()) {
        bool found = std::find(declared_commands.begin(), declared_commands.end(), cmd) != declared_commands.end();
        if (!found) {
            return {false, "command not declared by node"};
        }
    } else {
        return {false, "node did not declare commands"};
    }
    return {true, ""};
}

} // namespace nodes
} // namespace hunterclaw
