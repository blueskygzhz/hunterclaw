// Aligned with openclaw: src/auto-reply/commands-args.ts
#pragma once

#include "hunterclaw/auto_reply/commands_registry.h"
#include <string>
#include <optional>
#include <map>
#include <functional>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Command arg formatters
// Aligned with OpenClaw: COMMAND_ARG_FORMATTERS (commands-args.ts)
// ============================================================================

using CommandArgsFormatter = std::function<std::optional<std::string>(const CommandArgValues&)>;

// ── Internal helpers ───────────────────────────────────────────────────────

inline std::optional<std::string> normalize_arg_value(const std::string& value) {
    std::string trimmed = value;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);
    return trimmed.empty() ? std::nullopt : std::optional<std::string>{trimmed};
}

inline std::string format_set_unset_action(
    const std::string& action,
    const std::optional<std::string>& path,
    const std::optional<std::string>& value)
{
    if (action == "unset") {
        return path ? action + " " + *path : action;
    }
    if (action == "set") {
        if (!path) return action;
        if (!value) return action + " " + *path;
        return action + " " + *path + "=" + *value;
    }
    return action;
}

// ── /config formatter ──────────────────────────────────────────────────────

// Aligned with OpenClaw: formatConfigArgs
inline std::optional<std::string> format_config_args(const CommandArgValues& values) {
    auto action_it = values.find("action");
    if (action_it == values.end()) return std::nullopt;
    std::string action = action_it->second;
    // lowercase
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);
    auto action_norm = normalize_arg_value(action);
    if (!action_norm) return std::nullopt;

    auto path_it = values.find("path");
    auto path = (path_it != values.end()) ? normalize_arg_value(path_it->second) : std::nullopt;
    auto value_it = values.find("value");
    auto val = (value_it != values.end()) ? normalize_arg_value(value_it->second) : std::nullopt;

    if (*action_norm == "show" || *action_norm == "get") {
        return path ? *action_norm + " " + *path : *action_norm;
    }
    return format_set_unset_action(*action_norm, path, val);
}

// ── /debug formatter ───────────────────────────────────────────────────────

// Aligned with OpenClaw: formatDebugArgs
inline std::optional<std::string> format_debug_args(const CommandArgValues& values) {
    auto action_it = values.find("action");
    if (action_it == values.end()) return std::nullopt;
    std::string action = action_it->second;
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);
    auto action_norm = normalize_arg_value(action);
    if (!action_norm) return std::nullopt;

    auto path_it = values.find("path");
    auto path = (path_it != values.end()) ? normalize_arg_value(path_it->second) : std::nullopt;
    auto value_it = values.find("value");
    auto val = (value_it != values.end()) ? normalize_arg_value(value_it->second) : std::nullopt;

    if (*action_norm == "show" || *action_norm == "reset") {
        return *action_norm;
    }
    return format_set_unset_action(*action_norm, path, val);
}

// ── /queue formatter ───────────────────────────────────────────────────────

// Aligned with OpenClaw: formatQueueArgs
inline std::optional<std::string> format_queue_args(const CommandArgValues& values) {
    auto get = [&](const std::string& key) -> std::optional<std::string> {
        auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return normalize_arg_value(it->second);
    };
    auto mode = get("mode");
    auto debounce = get("debounce");
    auto cap = get("cap");
    auto drop = get("drop");

    std::string result;
    if (mode) result += *mode;
    if (debounce) { if (!result.empty()) result += " "; result += "debounce:" + *debounce; }
    if (cap)      { if (!result.empty()) result += " "; result += "cap:" + *cap; }
    if (drop)     { if (!result.empty()) result += " "; result += "drop:" + *drop; }
    return result.empty() ? std::nullopt : std::optional<std::string>{result};
}

// ── /exec formatter ────────────────────────────────────────────────────────

// Aligned with OpenClaw: formatExecArgs
inline std::optional<std::string> format_exec_args(const CommandArgValues& values) {
    auto get = [&](const std::string& key) -> std::optional<std::string> {
        auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return normalize_arg_value(it->second);
    };
    auto host = get("host");
    auto security = get("security");
    auto ask = get("ask");
    auto node = get("node");

    std::string result;
    if (host)     { if (!result.empty()) result += " "; result += "host=" + *host; }
    if (security) { if (!result.empty()) result += " "; result += "security=" + *security; }
    if (ask)      { if (!result.empty()) result += " "; result += "ask=" + *ask; }
    if (node)     { if (!result.empty()) result += " "; result += "node=" + *node; }
    return result.empty() ? std::nullopt : std::optional<std::string>{result};
}

// ── Registry ───────────────────────────────────────────────────────────────

// Aligned with OpenClaw: COMMAND_ARG_FORMATTERS
inline const std::map<std::string, CommandArgsFormatter>& get_command_arg_formatters() {
    static const std::map<std::string, CommandArgsFormatter> formatters = {
        {"config", format_config_args},
        {"debug",  format_debug_args},
        {"queue",  format_queue_args},
        {"exec",   format_exec_args},
    };
    return formatters;
}

} // namespace auto_reply
} // namespace hunterclaw
