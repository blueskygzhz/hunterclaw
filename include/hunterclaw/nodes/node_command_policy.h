#pragma once

// 🎯 对齐 OpenClaw: gateway/node-command-policy.ts
// 按平台定义允许的命令集，支持 allowCommands/denyCommands 配置覆盖

#include "hunterclaw/nodes/node.h"
#include <string>
#include <vector>
#include <set>
#include <map>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// Canvas / Camera / Screen / Location / etc. 命令常量
// (对齐 OpenClaw gateway/node-command-policy.ts:10-57)
// ============================================================================

inline const std::vector<std::string> CANVAS_COMMANDS = {
    "canvas.present", "canvas.hide", "canvas.navigate", "canvas.eval",
    "canvas.snapshot", "canvas.a2ui.push", "canvas.a2ui.pushJSONL", "canvas.a2ui.reset",
};

inline const std::vector<std::string> CAMERA_COMMANDS = {"camera.list"};
inline const std::vector<std::string> CAMERA_DANGEROUS_COMMANDS = {"camera.snap", "camera.clip"};
inline const std::vector<std::string> SCREEN_DANGEROUS_COMMANDS = {"screen.record"};
inline const std::vector<std::string> LOCATION_COMMANDS = {"location.get"};
inline const std::vector<std::string> NOTIFICATION_COMMANDS = {"notifications.list"};
inline const std::vector<std::string> ANDROID_NOTIFICATION_COMMANDS = {
    "notifications.list", "notifications.actions"};
inline const std::vector<std::string> DEVICE_COMMANDS = {"device.info", "device.status"};
inline const std::vector<std::string> ANDROID_DEVICE_COMMANDS = {
    "device.info", "device.status", "device.permissions", "device.health"};
inline const std::vector<std::string> CONTACTS_COMMANDS = {"contacts.search"};
inline const std::vector<std::string> CONTACTS_DANGEROUS_COMMANDS = {"contacts.add"};
inline const std::vector<std::string> CALENDAR_COMMANDS = {"calendar.events"};
inline const std::vector<std::string> CALENDAR_DANGEROUS_COMMANDS = {"calendar.add"};
inline const std::vector<std::string> CALL_LOG_COMMANDS = {"callLog.search"};
inline const std::vector<std::string> REMINDERS_COMMANDS = {"reminders.list"};
inline const std::vector<std::string> REMINDERS_DANGEROUS_COMMANDS = {"reminders.add"};
inline const std::vector<std::string> PHOTOS_COMMANDS = {"photos.latest"};
inline const std::vector<std::string> MOTION_COMMANDS = {"motion.activity", "motion.pedometer"};
inline const std::vector<std::string> SMS_DANGEROUS_COMMANDS = {"sms.send", "sms.search"};

// 🎯 对齐 OpenClaw: DEFAULT_DANGEROUS_NODE_COMMANDS (line 67-74)
inline const std::vector<std::string> DEFAULT_DANGEROUS_NODE_COMMANDS = {
    "camera.snap", "camera.clip",
    "screen.record",
    "contacts.add",
    "calendar.add",
    "reminders.add",
    "sms.send", "sms.search",
};

// ============================================================================
// Platform ID (对齐 OpenClaw line 121)
// ============================================================================

enum class PlatformId {
    Ios, Android, MacOS, Windows, Linux, Unknown
};

// ============================================================================
// 平台命令策略函数
// ============================================================================

// 🎯 对齐 OpenClaw: normalizePlatformId(platform?, deviceFamily?)
PlatformId normalize_platform_id(const std::string& platform,
                                  const std::string& device_family = "");

// 🎯 对齐 OpenClaw: resolveNodeCommandAllowlist(cfg, node?)
// 根据平台 + config.gateway.nodes.allowCommands/denyCommands 计算最终 allowlist
std::set<std::string> resolve_node_command_allowlist(
    const std::vector<std::string>& config_allow_commands,
    const std::vector<std::string>& config_deny_commands,
    const std::string& platform = "",
    const std::string& device_family = "");

// 🎯 对齐 OpenClaw: normalizeDeclaredNodeCommands({declaredCommands, allowlist})
std::vector<std::string> normalize_declared_node_commands(
    const std::vector<std::string>& declared_commands,
    const std::set<std::string>& allowlist);

// 🎯 对齐 OpenClaw: isNodeCommandAllowed({command, declaredCommands, allowlist})
struct NodeCommandAllowedResult {
    bool ok;
    std::string reason;  // 空 if ok=true
};

NodeCommandAllowedResult is_node_command_allowed(
    const std::string& command,
    const std::vector<std::string>& declared_commands,
    const std::set<std::string>& allowlist);

} // namespace nodes
} // namespace hunterclaw
