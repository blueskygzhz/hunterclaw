// ============================================================================
// acp_commands.cc
// 🎯 严格对齐 OpenClaw: src/acp/commands.ts
// ============================================================================

#include "hunterclaw/acp/acp_commands.h"

namespace hunterclaw {
namespace acp {

// ── get_available_commands ────────────────────────────────────────────────────
// 🎯 对齐 commands.ts: getAvailableCommands()
// 返回 ACP spec 标准命令集。对齐 OpenClaw 的 11 个核心命令。
// 注意: 命令顺序反映优先级（影响客户端展示顺序）。
std::vector<AcpCommand> get_available_commands() {
    return {
        {"help",     "Show help and common commands."},
        {"commands", "List available commands."},
        {"status",   "Show current status."},
        {"context",  "Explain context usage."},
        {"subagents","List or manage sub-agents."},
        {"stop",     "Stop the current run."},
        {"reset",    "Reset the session."},
        {"compact",  "Compact the session history."},
        {"model",    "Select a model."},
        {"think",    "Set thinking level."},
        {"verbose",  "Set verbose mode."},
    };
}

// 🔧 便捷函数: 返回 std::vector<nlohmann::json>，兼容旧版签名
std::vector<nlohmann::json> get_available_commands_vector() {
    auto cmds = get_available_commands();
    std::vector<nlohmann::json> result;
    result.reserve(cmds.size());
    for (const auto& cmd : cmds) {
        result.push_back({{"name", cmd.name}, {"description", cmd.description}});
    }
    return result;
}

// ── serialize_commands_to_json ────────────────────────────────────────────────
// 🎯 对齐 commands.ts: serializeCommandsToJSON(commands)
nlohmann::json serialize_commands_to_json(const std::vector<AcpCommand>& commands) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& cmd : commands) {
        arr.push_back({
            {"name", cmd.name},
            {"description", cmd.description}
        });
    }
    return arr;
}

// ── filter_commands_for_elevated ─────────────────────────────────────────────
// 🎯 对齐 commands.ts: filterCommandsForElevated(commands, elevated)
std::vector<AcpCommand> filter_commands_for_elevated(
    const std::vector<AcpCommand>& commands, bool elevated) {

    if (!elevated) return commands;

    // Elevated 模式只暴露控制级命令（对齐 OC: 裁剪为安全子集）
    std::vector<AcpCommand> filtered;
    for (const auto& cmd : commands) {
        if (cmd.name == "help" || cmd.name == "commands" ||
            cmd.name == "status" || cmd.name == "context" ||
            cmd.name == "model" || cmd.name == "think" ||
            cmd.name == "verbose") {
            filtered.push_back(cmd);
        }
    }
    return filtered;
}

} // namespace acp
} // namespace hunterclaw
