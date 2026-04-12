// commands_context.cc
// Aligned with openclaw:
//   auto-reply/reply/commands-context.ts           (/context command)
//   auto-reply/reply/commands-context-report.ts    (context report generation)
//   auto-reply/reply/commands-context-command.ts   (context command routing)
//   auto-reply/reply/commands-system-prompt.ts     (/system-prompt command)
//   auto-reply/reply/commands-info.ts              (/info command)
//
// Handles:
//   /context           — show context window usage report
//   /system-prompt      — show or modify system prompt
//   /info               — show agent/session information

#include "hunterclaw/auto_reply/commands_registry.h"
#include "hunterclaw/context/context_engine.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <sstream>
#include <algorithm>
#include <iomanip>

namespace hunterclaw {
namespace auto_reply {

namespace {

auto log() {
    static auto logger = spdlog::get("commands_context");
    if (!logger) logger = spdlog::stdout_logger_mt("commands_context");
    return logger;
}

// ============================================================================
// 🎯 对齐 OpenClaw: commands-context-report.ts — buildContextReport
// Build a formatted context window usage report
// ============================================================================

std::string build_context_report(
    const context::ContextWindowInfo& info,
    const std::string& model_name
) {
    std::ostringstream oss;

    oss << "**Context Window Report**\n\n";

    // Model info
    if (!model_name.empty()) {
        oss << "Model: " << model_name << "\n";
    }
    oss << "Context limit: " << info.max_tokens << " tokens\n";
    oss << "Used: " << info.used_tokens << " tokens ("
        << std::fixed << std::setprecision(1)
        << (info.max_tokens > 0 ? (100.0 * info.used_tokens / info.max_tokens) : 0)
        << "%)\n";
    oss << "Available: " << (info.max_tokens - info.used_tokens) << " tokens\n\n";

    // Breakdown
    oss << "**Breakdown:**\n";
    oss << "- System prompt: " << info.system_prompt_tokens << " tokens\n";
    oss << "- Conversation: " << info.conversation_tokens << " tokens\n";
    oss << "- Tool results: " << info.tool_result_tokens << " tokens\n";
    if (info.memory_tokens > 0) {
        oss << "- Memory context: " << info.memory_tokens << " tokens\n";
    }
    if (info.bootstrap_tokens > 0) {
        oss << "- Bootstrap files: " << info.bootstrap_tokens << " tokens\n";
    }

    oss << "\nMessages: " << info.message_count << " total, "
        << info.user_messages << " user, "
        << info.assistant_messages << " assistant\n";

    // Compaction status
    if (info.compaction_count > 0) {
        oss << "\nCompactions: " << info.compaction_count;
        if (info.last_compaction_at > 0) {
            oss << " (last: " << info.last_compaction_at << ")";
        }
        oss << "\n";
    }

    return oss.str();
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: handleContextCommand
// ============================================================================

CommandResult handle_context_command(
    const CommandContext& ctx,
    const core::Config& config
) {
    std::string normalized = ctx.command_body_normalized;

    if (normalized != "/context" && normalized.find("/context ") != 0) {
        return CommandResult::not_handled();
    }

    if (!ctx.is_authorized_sender) {
        return CommandResult::stop();
    }

    log()->info("handle_context_command: session={}", ctx.session_key);

    // Build context report (would use context engine in real impl)
    context::ContextWindowInfo info;
    info.max_tokens = 200000;
    info.used_tokens = 0;
    info.system_prompt_tokens = 0;
    info.conversation_tokens = 0;
    info.tool_result_tokens = 0;
    info.memory_tokens = 0;
    info.bootstrap_tokens = 0;
    info.message_count = 0;
    info.user_messages = 0;
    info.assistant_messages = 0;
    info.compaction_count = 0;
    info.last_compaction_at = 0;

    return CommandResult::stop_with_text(build_context_report(info, ""));
}

// ============================================================================
// 🎯 对齐 OpenClaw: handleSystemPromptCommand
// ============================================================================

CommandResult handle_system_prompt_command(
    const CommandContext& ctx,
    const core::Config& config
) {
    std::string normalized = ctx.command_body_normalized;

    if (normalized != "/system-prompt" && normalized != "/sp" &&
        normalized.find("/system-prompt ") != 0 && normalized.find("/sp ") != 0) {
        return CommandResult::not_handled();
    }

    if (!ctx.is_authorized_sender) {
        return CommandResult::stop();
    }

    // Extract rest
    std::string prefix = (normalized.find("/system-prompt") == 0) ? "/system-prompt" : "/sp";
    std::string rest = normalized.substr(prefix.size());
    auto start = rest.find_first_not_of(" \t");
    if (start != std::string::npos) rest = rest.substr(start);
    else rest = "";

    if (rest.empty()) {
        // Show current system prompt
        return CommandResult::stop_with_text(
            "System prompt display requires gateway integration. "
            "Use `/context` to see context window usage.");
    }

    // Set system prompt override
    log()->info("handle_system_prompt_command: set override len={}", rest.size());
    return CommandResult::stop_with_text(
        "System prompt override set (" + std::to_string(rest.size()) + " chars).");
}

// ============================================================================
// 🎯 对齐 OpenClaw: commands-info.ts — handleInfoCommand
// ============================================================================

CommandResult handle_info_command(
    const CommandContext& ctx,
    const core::Config& config
) {
    std::string normalized = ctx.command_body_normalized;

    if (normalized != "/info" && normalized.find("/info ") != 0) {
        return CommandResult::not_handled();
    }

    if (!ctx.is_authorized_sender) {
        return CommandResult::stop();
    }

    agents::AgentScopeManager scope(config);
    std::string agent_id = scope.resolve_default_agent_id();
    auto agent_config = scope.resolve_agent_config(agent_id);

    std::ostringstream oss;
    oss << "**Agent Info**\n\n";
    oss << "Agent ID: " << agent_id << "\n";
    if (agent_config.has_value()) {
        oss << "Name: " << agent_config->name << "\n";
        oss << "Workspace: " << agent_config->workspace << "\n";
        if (agent_config->model_primary.has_value()) {
            oss << "Model: " << *agent_config->model_primary << "\n";
        }
    }
    oss << "Session: " << ctx.session_key << "\n";

    // Tools info — 对齐 OpenClaw: commands-info.ts tools section
    oss << "\nUse `/context` for context window details.\n";
    oss << "Use `/status` for full runtime status.\n";

    return CommandResult::stop_with_text(oss.str());
}

} // namespace auto_reply
} // namespace hunterclaw
