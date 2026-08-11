#pragma once

// ============================================================================
// acp_commands.h
// 🎯 严格对齐 OpenClaw: src/acp/commands.ts
//
// 职责:
//   - ACP 可用命令定义（对齐 ACP spec）
//   - 命令过滤（按权限/上下文裁剪）
//   - 命令序列化（供 initialize / available_commands_update 使用）
// ============================================================================

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace acp {

// ── ACP Command ───────────────────────────────────────────────────────────────

// 🎯 对齐 commands.ts: AcpCommand
struct AcpCommand {
    std::string name;
    std::string description;
};

// ── Public API ────────────────────────────────────────────────────────────────

// 🎯 对齐 commands.ts: getAvailableCommands()
// 返回所有标准 ACP 命令定义。
// 对齐 OpenClaw ACP spec + OC commands.ts COMMANDS 数组。
std::vector<AcpCommand> get_available_commands();

// 🔧 便捷函数: 返回 std::vector<nlohmann::json>（每个元素为 {name, description} 对象）
// 兼容旧版 acp_translator.h 中 get_available_commands() 的签名，
// 降低调用方重构成本。
std::vector<nlohmann::json> get_available_commands_vector();

// 🎯 对齐 commands.ts: serializeCommandsToJSON(commands)
// 将命令列表序列化为 { name, description } JSON 数组，
// 用于 initialize response 的 commands 字段或 available_commands_update 通知。
nlohmann::json serialize_commands_to_json(const std::vector<AcpCommand>& commands);

// 🎯 对齐 commands.ts: filterCommandsForElevated(commands, elevated)
// 当 ACP 连接为 elevated 模式时只返回控制级命令，
// 普通 interactive 模式返回全部。
std::vector<AcpCommand> filter_commands_for_elevated(
    const std::vector<AcpCommand>& commands, bool elevated);

} // namespace acp
} // namespace hunterclaw
