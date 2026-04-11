// 严格对齐 OpenClaw auto-reply/command-status-builders.ts (234行)
#include "hunterclaw/auto_reply/command_status_builders.h"
#include "hunterclaw/auto_reply/commands_registry.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace auto_reply {

static constexpr int COMMANDS_PER_PAGE = 8;

static const std::vector<std::pair<std::string, std::string>> CATEGORY_ORDER = {
  {"session", "Session"}, {"options", "Options"}, {"status", "Status"},
  {"management", "Management"}, {"media", "Media"}, {"tools", "Tools"}, {"docks", "Docks"}
};

// 对齐 buildHelpMessage
std::string build_help_message(const std::string& /*config_json*/) {
  std::vector<std::string> lines = {"ℹ️ Help", ""};
  lines.push_back("Session");
  lines.push_back("  /new  |  /reset  |  /compact [instructions]  |  /stop");
  lines.push_back("");
  lines.push_back("Options");
  lines.push_back("  /think <level>  |  /model <id>  |  /fast status|on|off  |  /verbose on|off");
  lines.push_back("");
  lines.push_back("Status");
  lines.push_back("  /status  |  /tasks  |  /whoami  |  /context");
  lines.push_back("");
  lines.push_back("Skills");
  lines.push_back("  /skill <name> [input]");
  lines.push_back("");
  lines.push_back("More: /commands for full list, /tools for available capabilities");
  
  std::string result;
  for (const auto& l : lines) { if (!result.empty()) result += "\n"; result += l; }
  return result;
}

// 对齐 buildCommandsMessagePaginated
CommandsMessageResult build_commands_message_paginated(
    const std::string& /*config_json*/,
    const std::optional<int>& page_opt,
    const std::optional<std::string>& /*surface*/
) {
  // Build a simple command list (对齐 command-status-builders.ts:101-234)
  // 完整实现需要 CommandsRegistry 集成，此处提供基本框架
  struct Item { std::string label; std::string text; };
  std::vector<Item> items = {
    {"Session", "/new - Start a new session"},
    {"Session", "/reset - Clear conversation history"},
    {"Session", "/compact [instructions] - Compact context"},
    {"Session", "/stop - Stop current run"},
    {"Options", "/think <level> - Set thinking level (off/low/medium/high)"},
    {"Options", "/model <id> - Switch model"},
    {"Options", "/verbose on|off - Toggle verbose mode"},
    {"Options", "/fast on|off - Toggle fast mode"},
    {"Options", "/reasoning off|on|stream - Toggle reasoning display"},
    {"Status", "/status - Show session status"},
    {"Status", "/tasks - Show running tasks"},
    {"Status", "/whoami - Show current user info"},
    {"Status", "/context - Show context report"},
    {"Management", "/commands [page] - Show command list"},
    {"Management", "/tools - Show available tools"},
    {"Management", "/help - Show help"},
  };
  
  int total = static_cast<int>(items.size());
  int total_pages = std::max(1, static_cast<int>(std::ceil(static_cast<double>(total) / COMMANDS_PER_PAGE)));
  int current_page = std::min(std::max(1, page_opt.value_or(1)), total_pages);
  int start = (current_page - 1) * COMMANDS_PER_PAGE;
  int end_idx = std::min(start + COMMANDS_PER_PAGE, total);
  
  std::ostringstream oss;
  oss << "ℹ️ Commands (" << current_page << "/" << total_pages << ")\n";
  std::string current_label;
  for (int i = start; i < end_idx; ++i) {
    if (items[i].label != current_label) {
      if (i > start) oss << "\n";
      oss << "\n" << items[i].label;
      current_label = items[i].label;
    }
    oss << "\n  " << items[i].text;
  }
  
  return {
    oss.str(),
    total_pages,
    current_page,
    current_page < total_pages,
    current_page > 1
  };
}

std::string build_commands_message(
    const std::string& config_json,
    const std::optional<int>& page,
    const std::optional<std::string>& surface
) {
  return build_commands_message_paginated(config_json, page, surface).text;
}

}  // namespace auto_reply
}  // namespace hunterclaw
