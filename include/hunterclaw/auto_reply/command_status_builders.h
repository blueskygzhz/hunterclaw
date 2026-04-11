#pragma once

// 严格对齐 OpenClaw auto-reply/command-status-builders.ts (234行)
// 命令帮助/状态格式化

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

/// Build /help message.
/// 对齐 buildHelpMessage
std::string build_help_message(const std::string& config_json = "{}");

/// Build /commands list.
/// 对齐 buildCommandsMessage
std::string build_commands_message(
    const std::string& config_json = "{}",
    const std::optional<int>& page = std::nullopt,
    const std::optional<std::string>& surface = std::nullopt
);

/// Paginated commands result.
struct CommandsMessageResult {
  std::string text;
  int total_pages;
  int current_page;
  bool has_next;
  bool has_prev;
};

/// Build paginated /commands list.
/// 对齐 buildCommandsMessagePaginated
CommandsMessageResult build_commands_message_paginated(
    const std::string& config_json = "{}",
    const std::optional<int>& page = std::nullopt,
    const std::optional<std::string>& surface = std::nullopt
);

}  // namespace auto_reply
}  // namespace hunterclaw
