// commands_subagents.cc
// Aligned with openclaw:
//   auto-reply/reply/commands-subagents.ts
//   auto-reply/reply/commands-subagents-dispatch.ts
//   auto-reply/reply/commands-subagents-text.ts
//   auto-reply/reply/commands-status-subagents.ts
//
// Unified subagent command handling for /subagents (or /sub) slash command.
// Dispatches to: list, spawn, kill, steer, info, send, focus, unfocus, agents, log, help

#include "hunterclaw/auto_reply/commands_registry.h"
#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace auto_reply {

namespace {

auto log() {
    static auto logger = spdlog::get("commands_subagents");
    if (!logger) logger = spdlog::stdout_logger_mt("commands_subagents");
    return logger;
}

// ============================================================================
// 🎯 对齐 OpenClaw: commands-subagents-dispatch.ts — constants
// ============================================================================

static const std::string COMMAND = "/subagents";
static const std::string COMMAND_SHORT = "/sub";
static const std::string COMMAND_AGENTS = "/agents";
static const std::string COMMAND_FOCUS = "/focus";
static const std::string COMMAND_UNFOCUS = "/unfocus";
static const std::string COMMAND_KILL = "/kill";
static const std::string COMMAND_STEER = "/steer";
static const std::string COMMAND_TELL = "/tell";

// 🎯 对齐 OpenClaw: ACTIONS list
static const std::vector<std::string> ACTIONS = {
    "list", "spawn", "kill", "steer", "info", "send",
    "focus", "unfocus", "agents", "log", "help"
};

// ============================================================================
// 🎯 对齐 OpenClaw: resolveHandledPrefix(normalized)
// ============================================================================

std::string resolve_handled_prefix(const std::string& normalized) {
    if (normalized.find(COMMAND + " ") == 0 || normalized == COMMAND) {
        return COMMAND;
    }
    if (normalized.find(COMMAND_SHORT + " ") == 0 || normalized == COMMAND_SHORT) {
        return COMMAND_SHORT;
    }
    if (normalized.find(COMMAND_AGENTS + " ") == 0 || normalized == COMMAND_AGENTS) {
        return COMMAND_AGENTS;
    }
    if (normalized.find(COMMAND_FOCUS + " ") == 0 || normalized == COMMAND_FOCUS) {
        return COMMAND_FOCUS;
    }
    if (normalized == COMMAND_UNFOCUS) {
        return COMMAND_UNFOCUS;
    }
    if (normalized.find(COMMAND_KILL + " ") == 0 || normalized == COMMAND_KILL) {
        return COMMAND_KILL;
    }
    if (normalized.find(COMMAND_STEER + " ") == 0 || normalized == COMMAND_STEER) {
        return COMMAND_STEER;
    }
    if (normalized.find(COMMAND_TELL + " ") == 0 || normalized == COMMAND_TELL) {
        return COMMAND_TELL;
    }
    return "";
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSubagentsAction(tokens)
// ============================================================================

std::string resolve_subagents_action(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return "list";

    std::string first = tokens[0];
    std::transform(first.begin(), first.end(), first.begin(),
        [](unsigned char c) { return std::tolower(c); });

    for (const auto& action : ACTIONS) {
        if (first == action) return action;
    }

    // Default: treat as list
    return "list";
}

// ============================================================================
// 🎯 对齐 OpenClaw: commands-subagents-text.ts — extractMessageText
// ============================================================================

struct ExtractedMessage {
    std::string role;
    std::string text;
};

std::optional<ExtractedMessage> extract_message_text(const nlohmann::json& message) {
    std::string role;
    std::string text;

    if (message.contains("role") && message["role"].is_string()) {
        role = message["role"].get<std::string>();
    }
    if (message.contains("content") && message["content"].is_string()) {
        text = message["content"].get<std::string>();
    } else if (message.contains("content") && message["content"].is_array()) {
        for (const auto& block : message["content"]) {
            if (block.contains("type") && block["type"] == "text" &&
                block.contains("text") && block["text"].is_string()) {
                if (!text.empty()) text += "\n";
                text += block["text"].get<std::string>();
            }
        }
    }

    if (text.empty()) return std::nullopt;
    return ExtractedMessage{role, text};
}

// ============================================================================
// 🎯 对齐 OpenClaw: commands-status-subagents.ts — buildSubagentsStatusLine
// ============================================================================

std::string build_subagents_status_line(
    const std::vector<agents::SubagentEntry>& runs,
    bool verbose_enabled
) {
    if (runs.empty()) return "";

    int active = 0;
    int done = 0;
    std::vector<std::string> active_labels;

    for (const auto& entry : runs) {
        bool is_ended = (entry.completed_at > 0 ||
                         entry.state == agents::SubagentState::Completed ||
                         entry.state == agents::SubagentState::Failed ||
                         entry.state == agents::SubagentState::Killed);
        if (!is_ended) {
            active++;
            if (active_labels.size() < 3) {
                std::string label = !entry.label.empty() ? entry.label :
                    (!entry.subagent_name.empty() ? entry.subagent_name : "subagent");
                active_labels.push_back(label);
            }
        } else {
            done++;
        }
    }

    if (verbose_enabled) {
        std::string label_text;
        if (!active_labels.empty()) {
            label_text = " (";
            for (size_t i = 0; i < active_labels.size(); i++) {
                if (i > 0) label_text += ", ";
                label_text += active_labels[i];
            }
            label_text += ")";
        }
        return "🤖 Subagents: " + std::to_string(active) + " active" + label_text +
               " · " + std::to_string(done) + " done";
    }

    if (active > 0) {
        return "🤖 Subagents: " + std::to_string(active) + " active";
    }

    return "";
}

// ============================================================================
// 🎯 对齐 OpenClaw: Help text
// ============================================================================

std::string get_subagents_help_text() {
    return
        "**Subagent Commands**\n"
        "\n"
        "`/subagents` or `/sub` — list active and recent subagents\n"
        "`/subagents list` — show active/recent subagent sessions\n"
        "`/subagents spawn <task>` — spawn a new subagent for a task\n"
        "`/subagents kill <id>` — kill a running subagent\n"
        "`/subagents steer <id> <message>` — redirect a running subagent\n"
        "`/subagents info <id>` — show subagent details and last reply\n"
        "`/subagents send <id> <message>` — send a message to a subagent\n"
        "`/focus <id>` — focus on a subagent (route messages to it)\n"
        "`/unfocus` — stop focusing on a subagent\n"
        "`/agents` — list configured agents\n"
        "`/subagents log <id>` — show subagent conversation log\n"
        "`/subagents help` — show this help\n";
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: handleSubagentsCommand (main entry)
// ============================================================================

CommandResult handle_subagents_command(
    const CommandContext& ctx,
    agents::SubagentRegistry& registry,
    const core::Config& config
) {
    std::string normalized = ctx.command_body_normalized;
    std::string handled_prefix = resolve_handled_prefix(normalized);
    if (handled_prefix.empty()) {
        return CommandResult::not_handled();
    }

    // Check authorization
    if (!ctx.is_authorized_sender) {
        log()->debug("Ignoring {} from unauthorized sender: {}", handled_prefix, ctx.sender_id);
        return CommandResult::stop();
    }

    // Extract rest tokens after prefix
    std::string rest = normalized.substr(handled_prefix.size());
    // trim
    auto start = rest.find_first_not_of(" \t");
    if (start != std::string::npos) rest = rest.substr(start);
    else rest = "";

    std::vector<std::string> tokens;
    std::istringstream iss(rest);
    std::string token;
    while (iss >> token) tokens.push_back(token);

    // Resolve action
    std::string action;
    if (handled_prefix == COMMAND_FOCUS) action = "focus";
    else if (handled_prefix == COMMAND_UNFOCUS) action = "unfocus";
    else if (handled_prefix == COMMAND_KILL) action = "kill";
    else if (handled_prefix == COMMAND_STEER || handled_prefix == COMMAND_TELL) action = "steer";
    else if (handled_prefix == COMMAND_AGENTS) action = "agents";
    else action = resolve_subagents_action(tokens);

    // Remove action token from tokens
    if (!tokens.empty() && tokens[0] == action) {
        tokens.erase(tokens.begin());
    }

    log()->info("handle_subagents_command: action={} tokens={}", action, tokens.size());

    // ============================================================================
    // Dispatch actions — 对齐 OpenClaw action handlers
    // ============================================================================

    if (action == "help") {
        return CommandResult::stop_with_text(get_subagents_help_text());
    }

    if (action == "list") {
        // 🎯 对齐 OpenClaw: action-list.ts
        agents::AgentScopeManager scope(config);
        std::string default_agent = scope.resolve_default_agent_id();

        auto all_active = registry.list_active();
        std::vector<agents::SubagentEntry> entries;
        for (const auto& entry : all_active) {
            entries.push_back(*entry);
        }

        auto list = agents::build_subagent_list(entries, 30, 72);
        if (list.total == 0) {
            return CommandResult::stop_with_text("No active or recent subagents.");
        }
        return CommandResult::stop_with_text(list.text);
    }

    if (action == "kill") {
        // 🎯 对齐 OpenClaw: action-kill.ts
        if (tokens.empty()) {
            return CommandResult::stop_with_text("Usage: /subagents kill <id>");
        }
        std::string run_id = tokens[0];

        auto result = agents::kill_subagent_run(registry, run_id);
        if (!result.error.empty()) {
            return CommandResult::stop_with_text("Error: " + result.error);
        }
        if (result.killed) {
            return CommandResult::stop_with_text("Killed subagent: " + run_id);
        }
        return CommandResult::stop_with_text("Subagent already ended: " + run_id);
    }

    if (action == "info") {
        // 🎯 对齐 OpenClaw: action-info.ts
        if (tokens.empty()) {
            return CommandResult::stop_with_text("Usage: /subagents info <id>");
        }
        std::string run_id = tokens[0];
        auto entry = registry.get_entry(run_id);
        if (!entry) {
            return CommandResult::stop_with_text("Subagent not found: " + run_id);
        }

        std::ostringstream oss;
        oss << "**Subagent: " << (entry->label.empty() ? entry->subagent_name : entry->label) << "**\n"
            << "ID: " << entry->subagent_run_id << "\n"
            << "Task: " << entry->task << "\n"
            << "Status: " << static_cast<int>(entry->state) << "\n"
            << "Age: " << entry->age_seconds() << "s\n";

        if (!entry->result_text.empty()) {
            std::string result_preview = entry->result_text;
            if (result_preview.size() > 500) {
                result_preview = result_preview.substr(0, 500) + "...";
            }
            oss << "\nLast reply:\n" << result_preview;
        }

        return CommandResult::stop_with_text(oss.str());
    }

    if (action == "agents") {
        // 🎯 对齐 OpenClaw: action-agents.ts
        agents::AgentScopeManager scope(config);
        auto ids = scope.list_agent_ids();

        std::ostringstream oss;
        oss << "**Configured Agents** (" << ids.size() << ")\n\n";
        for (const auto& id : ids) {
            auto entry = scope.resolve_agent_entry(id);
            std::string name = entry.has_value() ? entry->name : id;
            oss << "- **" << id << "**"
                << (name != id ? " (" + name + ")" : "")
                << "\n";
        }
        return CommandResult::stop_with_text(oss.str());
    }

    if (action == "spawn" || action == "steer" || action == "send" ||
        action == "focus" || action == "unfocus" || action == "log") {
        // These require more complex integration with gateway/session system
        // For now, return a stub message
        return CommandResult::stop_with_text(
            "Action /" + action + " requires gateway integration (not yet wired).");
    }

    return CommandResult::stop_with_text(get_subagents_help_text());
}

// ============================================================================
// 🎯 对齐 OpenClaw: dispatch-acp-command-bypass.ts
// shouldBypassAcpDispatchForCommand
// ============================================================================

bool should_bypass_acp_dispatch_for_command(
    const std::string& command_body,
    const core::Config& config,
    bool command_authorized
) {
    std::string trimmed = command_body;
    auto start = trimmed.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return false;
    auto end = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(start, end - start + 1);

    if (trimmed.empty()) return false;

    // /new and /reset always bypass ACP dispatch
    if (trimmed.find("/new") == 0 || trimmed.find("/reset") == 0) {
        return true;
    }

    // ! commands (bash) bypass if authorized and bash enabled
    if (trimmed[0] == '!') {
        return command_authorized;
    }

    return false;
}

// ============================================================================
// 🎯 对齐 OpenClaw: commands-acp.ts — handleAcpCommand
// ============================================================================

static const std::string ACP_COMMAND = "/acp";

static const std::vector<std::string> ACP_ACTIONS = {
    "spawn", "cancel", "steer", "close", "status",
    "set-mode", "set", "cwd", "permissions", "timeout",
    "model", "reset-options", "doctor", "install", "sessions", "help"
};

std::string resolve_acp_action(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return "help";
    std::string first = tokens[0];
    std::transform(first.begin(), first.end(), first.begin(),
        [](unsigned char c) { return std::tolower(c); });
    for (const auto& action : ACP_ACTIONS) {
        if (first == action) return action;
    }
    return "help";
}

std::string get_acp_help_text() {
    return
        "**ACP Commands**\n"
        "\n"
        "`/acp spawn <task>` — spawn an ACP session\n"
        "`/acp cancel <id>` — cancel an ACP session\n"
        "`/acp steer <id> <message>` — redirect an ACP session\n"
        "`/acp close <id>` — close an ACP session\n"
        "`/acp status [id]` — show ACP session status\n"
        "`/acp set-mode <mode>` — set ACP session mode (run/session)\n"
        "`/acp set <key> <value>` — set ACP runtime option\n"
        "`/acp cwd [path]` — get/set ACP working directory\n"
        "`/acp permissions` — show ACP permissions\n"
        "`/acp timeout [seconds]` — get/set ACP timeout\n"
        "`/acp model [model]` — get/set ACP model\n"
        "`/acp reset-options` — reset ACP runtime options\n"
        "`/acp doctor` — diagnose ACP issues\n"
        "`/acp install <plugin>` — install ACP plugin\n"
        "`/acp sessions` — list ACP sessions\n"
        "`/acp help` — show this help\n";
}

CommandResult handle_acp_command(
    const CommandContext& ctx,
    const core::Config& config
) {
    std::string normalized = ctx.command_body_normalized;
    if (normalized.find(ACP_COMMAND) != 0) {
        return CommandResult::not_handled();
    }

    if (!ctx.is_authorized_sender) {
        log()->debug("Ignoring /acp from unauthorized sender: {}", ctx.sender_id);
        return CommandResult::stop();
    }

    std::string rest = normalized.substr(ACP_COMMAND.size());
    auto start = rest.find_first_not_of(" \t");
    if (start != std::string::npos) rest = rest.substr(start);
    else rest = "";

    std::vector<std::string> tokens;
    std::istringstream iss(rest);
    std::string token;
    while (iss >> token) tokens.push_back(token);

    std::string action = resolve_acp_action(tokens);

    if (action == "help") {
        return CommandResult::stop_with_text(get_acp_help_text());
    }

    // ACP actions require gateway integration — stub for now
    return CommandResult::stop_with_text(
        "ACP action /" + action + " requires gateway ACP integration.");
}

} // namespace auto_reply
} // namespace hunterclaw
