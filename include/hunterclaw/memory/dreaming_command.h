#pragma once

// ============================================================================
// Dreaming Command — 严格对齐 OpenClaw dreaming-command.ts (128 行)
//
// 注册 /dreaming 斜杠命令，允许用户在运行时切换 dreaming 开关。
// 子命令: /dreaming on | /dreaming off | /dreaming status
// ============================================================================

#include <string>
#include <functional>
#include <optional>

namespace hunterclaw {
namespace memory {

/**
 * Result from dreaming command handler.
 */
struct DreamingCommandResult {
    std::string text;
};

/**
 * Context for dreaming command execution.
 */
struct DreamingCommandContext {
    std::string args;  // e.g. "on", "off", "status", "help"
    std::optional<std::string> gateway_client_scopes;  // e.g. "operator.admin"
};

/**
 * Config reader/writer interface for dreaming state.
 */
struct DreamingConfigAccess {
    std::function<bool()> is_enabled;
    std::function<std::string()> get_cron;
    std::function<double()> get_min_score;
    std::function<int()> get_min_recall_count;
    std::function<int()> get_min_unique_queries;
    std::function<std::optional<std::string>()> get_timezone;
    std::function<void(bool enabled)> set_enabled;
};

/**
 * Handle /dreaming command.
 * (对齐 dreaming-command.ts:82-127 registerDreamingCommand handler)
 */
DreamingCommandResult handle_dreaming_command(
    const DreamingCommandContext& ctx,
    const DreamingConfigAccess& config
);

}  // namespace memory
}  // namespace hunterclaw
