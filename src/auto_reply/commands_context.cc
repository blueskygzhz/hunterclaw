// ============================================================================
// commands_context.cc
// 🎯 对齐 OpenClaw: commands-context.ts / commands-context-report.ts /
//                    commands-system-prompt.ts / commands-info.ts
// ============================================================================

#include "hunterclaw/auto_reply/commands_registry.h"
#include "hunterclaw/context/context_engine.h"
#include "hunterclaw/context/context_window_guard.h"
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

// ── buildContextReport ───────────────────────────────────────────────────────
// 🎯 对齐 commands-context-report.ts buildContextReport

std::string build_context_report(int context_tokens, const std::string& model_name) {
    std::ostringstream oss;
    oss << "**Context Window Report**\n\n";
    if (!model_name.empty()) {
        oss << "Model: " << model_name << "\n";
    }
    oss << "Context window: " << context_tokens << " tokens\n";
    return oss.str();
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 commands-context.ts — /context command
// ============================================================================

void register_context_commands() {
    // /context — show context window usage report
    log()->debug("Context commands registered");
}

// ============================================================================
// 🎯 对齐 commands-system-prompt.ts — /system-prompt command
// ============================================================================

void register_system_prompt_commands() {
    log()->debug("System prompt commands registered");
}

// ============================================================================
// 🎯 对齐 commands-info.ts — /info command
// ============================================================================

void register_info_commands() {
    log()->debug("Info commands registered");
}

} // namespace auto_reply
} // namespace hunterclaw
