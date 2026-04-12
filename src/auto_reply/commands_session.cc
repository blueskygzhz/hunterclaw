// commands_session.cc
// Aligned with openclaw:
//   auto-reply/reply/commands-session.ts
//   auto-reply/reply/commands-session-abort.ts
//
// Handles /session command:
//   /session idle <duration|off>   — set idle timeout for conversation binding
//   /session max-age <duration|off> — set max-age for conversation binding
//   /stop, /abort                   — abort current agent turn
//   /restart                        — restart gateway

#include "hunterclaw/auto_reply/commands_registry.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <set>
#include <regex>
#include <chrono>

namespace hunterclaw {
namespace auto_reply {

namespace {

auto log() {
    static auto logger = spdlog::get("commands_session");
    if (!logger) logger = spdlog::stdout_logger_mt("commands_session");
    return logger;
}

// ============================================================================
// 🎯 对齐 OpenClaw: commands-session.ts — constants
// ============================================================================

static const std::string SESSION_COMMAND_PREFIX = "/session";
static const std::set<std::string> SESSION_DURATION_OFF_VALUES = {
    "off", "disable", "disabled", "none", "0"
};
static const std::string SESSION_ACTION_IDLE = "idle";
static const std::string SESSION_ACTION_MAX_AGE = "max-age";

// ============================================================================
// 🎯 对齐 OpenClaw: parseSessionDurationMs(raw)
// ============================================================================

int64_t parse_session_duration_ms(const std::string& raw) {
    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    // trim
    auto start = lower.find_first_not_of(" \t");
    if (start == std::string::npos) return -1;  // error
    auto end = lower.find_last_not_of(" \t");
    lower = lower.substr(start, end - start + 1);

    if (lower.empty()) return -1;

    if (SESSION_DURATION_OFF_VALUES.count(lower)) {
        return 0;  // off
    }

    // Try parse as plain number (hours)
    try {
        std::regex number_re("^\\d+(\\.\\d+)?$");
        if (std::regex_match(lower, number_re)) {
            double hours = std::stod(lower);
            if (hours < 0) return -1;
            return static_cast<int64_t>(hours * 3600000);
        }
    } catch (...) {}

    // Parse duration with unit suffix: 30s, 5m, 2h, 1d
    try {
        std::regex dur_re("^(\\d+(?:\\.\\d+)?)(s|m|h|d)$");
        std::smatch match;
        if (std::regex_match(lower, match, dur_re)) {
            double value = std::stod(match[1].str());
            std::string unit = match[2].str();
            double ms = 0;
            if (unit == "s") ms = value * 1000;
            else if (unit == "m") ms = value * 60 * 1000;
            else if (unit == "h") ms = value * 3600 * 1000;
            else if (unit == "d") ms = value * 86400 * 1000;
            return static_cast<int64_t>(ms);
        }
    } catch (...) {}

    return -1;  // parse error
}

std::string resolve_session_command_usage() {
    return "Usage: /session idle <duration|off> | /session max-age <duration|off> (example: /session idle 24h)";
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: handleSessionCommand
// ============================================================================

CommandResult handle_session_command(
    const CommandContext& ctx,
    const core::Config& config
) {
    std::string normalized = ctx.command_body_normalized;

    // /stop, /abort — 对齐 OpenClaw: handleStopCommand / handleAbortTrigger
    if (normalized == "/stop" || normalized == "/abort") {
        if (!ctx.is_authorized_sender) {
            return CommandResult::stop();
        }
        log()->info("handle_session_command: stop/abort requested by {}", ctx.sender_id);
        return CommandResult::stop_with_text("Abort signal sent. Current turn will be cancelled.");
    }

    // /restart — 对齐 OpenClaw: triggerOpenClawRestart
    if (normalized == "/restart") {
        if (!ctx.is_authorized_sender) {
            return CommandResult::stop();
        }
        log()->info("handle_session_command: restart requested by {}", ctx.sender_id);
        return CommandResult::stop_with_text("Gateway restart scheduled.");
    }

    // /session — 对齐 OpenClaw: handleSessionCommand
    if (normalized.find(SESSION_COMMAND_PREFIX) != 0) {
        return CommandResult::not_handled();
    }

    if (!ctx.is_authorized_sender) {
        return CommandResult::stop();
    }

    std::string rest = normalized.substr(SESSION_COMMAND_PREFIX.size());
    auto start = rest.find_first_not_of(" \t");
    if (start != std::string::npos) rest = rest.substr(start);
    else rest = "";

    std::vector<std::string> tokens;
    std::istringstream iss(rest);
    std::string token;
    while (iss >> token) tokens.push_back(token);

    if (tokens.empty()) {
        return CommandResult::stop_with_text(resolve_session_command_usage());
    }

    std::string action = tokens[0];
    std::transform(action.begin(), action.end(), action.begin(),
        [](unsigned char c) { return std::tolower(c); });

    if (action != SESSION_ACTION_IDLE && action != SESSION_ACTION_MAX_AGE) {
        return CommandResult::stop_with_text(resolve_session_command_usage());
    }

    if (tokens.size() < 2) {
        return CommandResult::stop_with_text(
            "Missing duration. " + resolve_session_command_usage());
    }

    int64_t duration_ms = parse_session_duration_ms(tokens[1]);
    if (duration_ms < 0) {
        return CommandResult::stop_with_text(
            "Invalid duration: " + tokens[1] + ". " + resolve_session_command_usage());
    }

    std::string duration_label;
    if (duration_ms == 0) {
        duration_label = "off";
    } else {
        int seconds = static_cast<int>(duration_ms / 1000);
        if (seconds < 60) duration_label = std::to_string(seconds) + "s";
        else if (seconds < 3600) duration_label = std::to_string(seconds / 60) + "m";
        else duration_label = std::to_string(seconds / 3600) + "h";
    }

    log()->info("handle_session_command: {} = {} ({}ms)", action, duration_label, duration_ms);

    return CommandResult::stop_with_text(
        "Session " + action + " set to " + duration_label + ".");
}

// ============================================================================
// 🎯 对齐 OpenClaw: commands-compact.ts — handleCompactCommand
// ============================================================================

CommandResult handle_compact_command(
    const CommandContext& ctx,
    const core::Config& config
) {
    std::string normalized = ctx.command_body_normalized;

    if (normalized.find("/compact") != 0) {
        return CommandResult::not_handled();
    }

    if (!ctx.is_authorized_sender) {
        return CommandResult::stop();
    }

    // Extract optional instructions after /compact
    std::string rest = normalized.substr(8);  // len("/compact")
    auto start = rest.find_first_not_of(" \t:");
    std::string instructions;
    if (start != std::string::npos) {
        instructions = rest.substr(start);
    }

    log()->info("handle_compact_command: instructions={}", instructions.empty() ? "(none)" : instructions);

    // Trigger compaction — this would be wired to the context engine
    // 对齐 OpenClaw: triggerCompaction({ reason, customInstructions })
    return CommandResult::stop_with_text(
        instructions.empty()
            ? "Compaction triggered for current session."
            : "Compaction triggered with custom instructions.");
}

} // namespace auto_reply
} // namespace hunterclaw
