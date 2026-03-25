// Aligned with openclaw: src/auto-reply/commands-registry.ts
// Aligned with openclaw: src/auto-reply/commands-registry.data.ts
#include "hunterclaw/auto_reply/commands_registry.h"
#include "hunterclaw/auto_reply/thinking.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Native name overrides (aligned with OpenClaw: NATIVE_NAME_OVERRIDES)
// ============================================================================
static const std::map<std::string, std::map<std::string, std::string>> NATIVE_NAME_OVERRIDES = {
    {"discord", {{"tts", "voice"}}},
    {"slack",   {{"status", "agentstatus"}}},
};

// ============================================================================
// Helper: escape regex special chars
// ============================================================================
static std::string escape_regex(const std::string& s) {
    static const std::string special = R"(\.^$|?*+()[]{}/)";
    std::string result;
    result.reserve(s.size() * 2);
    for (char c : s) {
        if (special.find(c) != std::string::npos) {
            result += '\\';
        }
        result += c;
    }
    return result;
}

// ============================================================================
// Helper: trim + lowercase
// ============================================================================
static std::string trim_lower(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    std::string result = s.substr(start, end - start + 1);
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

// ============================================================================
// CommandsRegistry implementation
// ============================================================================

CommandsRegistry::CommandsRegistry() {
    build_commands();
}

// ── Build command list (aligned with OpenClaw: buildChatCommands) ──────────

void CommandsRegistry::build_commands() {
    commands_.clear();

    // Helper lambda to add a command
    auto add = [&](ChatCommandDefinition cmd) {
        commands_.push_back(std::move(cmd));
    };

    // /help
    {
        ChatCommandDefinition cmd;
        cmd.key = "help"; cmd.native_name = "help";
        cmd.description = "Show available commands.";
        cmd.text_aliases = {"/help"};
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Status;
        add(cmd);
    }
    // /commands
    {
        ChatCommandDefinition cmd;
        cmd.key = "commands"; cmd.native_name = "commands";
        cmd.description = "List all slash commands.";
        cmd.text_aliases = {"/commands"};
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Status;
        add(cmd);
    }
    // /skill
    {
        ChatCommandDefinition cmd;
        cmd.key = "skill"; cmd.native_name = "skill";
        cmd.description = "Run a skill by name.";
        cmd.text_aliases = {"/skill"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Tools;
        cmd.args = {
            {"name", "Skill name", CommandArgType::String, true, {}, false, false},
            {"input", "Skill input", CommandArgType::String, false, {}, false, true},
        };
        add(cmd);
    }
    // /status
    {
        ChatCommandDefinition cmd;
        cmd.key = "status"; cmd.native_name = "status";
        cmd.description = "Show current status.";
        cmd.text_aliases = {"/status"};
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Status;
        add(cmd);
    }
    // /allowlist (text-only)
    {
        ChatCommandDefinition cmd;
        cmd.key = "allowlist";
        cmd.description = "List/add/remove allowlist entries.";
        cmd.text_aliases = {"/allowlist"};
        cmd.accepts_args = true;
        cmd.scope = CommandScope::Text;
        cmd.category = CommandCategory::Management;
        add(cmd);
    }
    // /approve
    {
        ChatCommandDefinition cmd;
        cmd.key = "approve"; cmd.native_name = "approve";
        cmd.description = "Approve or deny exec requests.";
        cmd.text_aliases = {"/approve"};
        cmd.accepts_args = true;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        add(cmd);
    }
    // /context
    {
        ChatCommandDefinition cmd;
        cmd.key = "context"; cmd.native_name = "context";
        cmd.description = "Explain how context is built and used.";
        cmd.text_aliases = {"/context"};
        cmd.accepts_args = true;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Status;
        add(cmd);
    }
    // /export-session
    {
        ChatCommandDefinition cmd;
        cmd.key = "export-session"; cmd.native_name = "export-session";
        cmd.description = "Export current session to HTML file with full system prompt.";
        cmd.text_aliases = {"/export-session", "/export"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Status;
        cmd.args = {
            {"path", "Output path (default: workspace)", CommandArgType::String, false, {}, false, false},
        };
        add(cmd);
    }
    // /tts
    {
        ChatCommandDefinition cmd;
        cmd.key = "tts"; cmd.native_name = "tts";
        cmd.description = "Control text-to-speech (TTS).";
        cmd.text_aliases = {"/tts"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Media;
        cmd.args = {
            {"action", "TTS action", CommandArgType::String, false,
             {{"on","On"},{"off","Off"},{"status","Status"},{"provider","Provider"},
              {"limit","Limit"},{"summary","Summary"},{"audio","Audio"},{"help","Help"}},
             false, false},
            {"value", "Provider, limit, or text", CommandArgType::String, false, {}, false, true},
        };
        cmd.args_menu = CommandArgMenuSpec{"action", "TTS Actions"};
        add(cmd);
    }
    // /whoami
    {
        ChatCommandDefinition cmd;
        cmd.key = "whoami"; cmd.native_name = "whoami";
        cmd.description = "Show your sender id.";
        cmd.text_aliases = {"/whoami", "/id"};
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Status;
        add(cmd);
    }
    // /session
    {
        ChatCommandDefinition cmd;
        cmd.key = "session"; cmd.native_name = "session";
        cmd.description = "Manage session-level settings (for example /session idle).";
        cmd.text_aliases = {"/session"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Session;
        cmd.args = {
            {"action", "idle | max-age", CommandArgType::String, false,
             {{"idle","idle"},{"max-age","max-age"}}, false, false},
            {"value", "Duration (24h, 90m) or off", CommandArgType::String, false, {}, false, true},
        };
        cmd.args_menu = CommandArgMenuSpec{"action", std::nullopt};
        add(cmd);
    }
    // /subagents
    {
        ChatCommandDefinition cmd;
        cmd.key = "subagents"; cmd.native_name = "subagents";
        cmd.description = "List, kill, log, spawn, or steer subagent runs for this session.";
        cmd.text_aliases = {"/subagents"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        cmd.args = {
            {"action", "list | kill | log | info | send | steer | spawn", CommandArgType::String, false,
             {{"list","list"},{"kill","kill"},{"log","log"},{"info","info"},
              {"send","send"},{"steer","steer"},{"spawn","spawn"}}, false, false},
            {"target", "Run id, index, or session key", CommandArgType::String, false, {}, false, false},
            {"value", "Additional input (limit/message)", CommandArgType::String, false, {}, false, true},
        };
        cmd.args_menu = CommandArgMenuSpec{"action", std::nullopt};
        add(cmd);
    }
    // /acp
    {
        ChatCommandDefinition cmd;
        cmd.key = "acp"; cmd.native_name = "acp";
        cmd.description = "Manage ACP sessions and runtime options.";
        cmd.text_aliases = {"/acp"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        cmd.args = {
            {"action", "Action to run", CommandArgType::String, false,
             {{"spawn","spawn"},{"cancel","cancel"},{"steer","steer"},{"close","close"},
              {"sessions","sessions"},{"status","status"},{"set-mode","set-mode"},
              {"set","set"},{"cwd","cwd"},{"permissions","permissions"},
              {"timeout","timeout"},{"model","model"},{"reset-options","reset-options"},
              {"doctor","doctor"},{"install","install"},{"help","help"}},
             true, false},
            {"value", "Action arguments", CommandArgType::String, false, {}, false, true},
        };
        cmd.args_menu = CommandArgMenuSpec{"action", std::nullopt};
        add(cmd);
    }
    // /focus
    {
        ChatCommandDefinition cmd;
        cmd.key = "focus"; cmd.native_name = "focus";
        cmd.description = "Bind this thread or topic/conversation to a session target.";
        cmd.text_aliases = {"/focus"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        cmd.args = {
            {"target", "Subagent label/index or session key/id/label", CommandArgType::String, false, {}, false, true},
        };
        add(cmd);
    }
    // /unfocus
    {
        ChatCommandDefinition cmd;
        cmd.key = "unfocus"; cmd.native_name = "unfocus";
        cmd.description = "Remove the current thread or topic/conversation binding.";
        cmd.text_aliases = {"/unfocus"};
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        add(cmd);
    }
    // /agents
    {
        ChatCommandDefinition cmd;
        cmd.key = "agents"; cmd.native_name = "agents";
        cmd.description = "List thread-bound agents for this session.";
        cmd.text_aliases = {"/agents"};
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        add(cmd);
    }
    // /kill
    {
        ChatCommandDefinition cmd;
        cmd.key = "kill"; cmd.native_name = "kill";
        cmd.description = "Kill a running subagent (or all).";
        cmd.text_aliases = {"/kill"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        cmd.args = {
            {"target", "Label, run id, index, or all", CommandArgType::String, false, {}, false, false},
        };
        cmd.args_menu = CommandArgMenuSpec{"target", std::nullopt};
        add(cmd);
    }
    // /steer
    {
        ChatCommandDefinition cmd;
        cmd.key = "steer"; cmd.native_name = "steer";
        cmd.description = "Send guidance to a running subagent.";
        cmd.text_aliases = {"/steer", "/tell"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        cmd.args = {
            {"target", "Label, run id, or index", CommandArgType::String, false, {}, false, false},
            {"message", "Steering message", CommandArgType::String, false, {}, false, true},
        };
        add(cmd);
    }
    // /config
    {
        ChatCommandDefinition cmd;
        cmd.key = "config"; cmd.native_name = "config";
        cmd.description = "Show or set config values.";
        cmd.text_aliases = {"/config"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::None;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        cmd.args = {
            {"action", "show | get | set | unset", CommandArgType::String, false,
             {{"show","show"},{"get","get"},{"set","set"},{"unset","unset"}}, false, false},
            {"path", "Config path", CommandArgType::String, false, {}, false, false},
            {"value", "Value for set", CommandArgType::String, false, {}, false, true},
        };
        add(cmd);
    }
    // /debug
    {
        ChatCommandDefinition cmd;
        cmd.key = "debug"; cmd.native_name = "debug";
        cmd.description = "Set runtime debug overrides.";
        cmd.text_aliases = {"/debug"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::None;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        cmd.args = {
            {"action", "show | reset | set | unset", CommandArgType::String, false,
             {{"show","show"},{"reset","reset"},{"set","set"},{"unset","unset"}}, false, false},
            {"path", "Debug path", CommandArgType::String, false, {}, false, false},
            {"value", "Value for set", CommandArgType::String, false, {}, false, true},
        };
        add(cmd);
    }
    // /usage
    {
        ChatCommandDefinition cmd;
        cmd.key = "usage"; cmd.native_name = "usage";
        cmd.description = "Usage footer or cost summary.";
        cmd.text_aliases = {"/usage"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        cmd.args = {
            {"mode", "off, tokens, full, or cost", CommandArgType::String, false,
             {{"off","off"},{"tokens","tokens"},{"full","full"},{"cost","cost"}}, false, false},
        };
        cmd.args_menu = CommandArgMenuSpec{"mode", std::nullopt};
        add(cmd);
    }
    // /stop
    {
        ChatCommandDefinition cmd;
        cmd.key = "stop"; cmd.native_name = "stop";
        cmd.description = "Stop the current run.";
        cmd.text_aliases = {"/stop"};
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Session;
        add(cmd);
    }
    // /restart
    {
        ChatCommandDefinition cmd;
        cmd.key = "restart"; cmd.native_name = "restart";
        cmd.description = "Restart HunterClaw.";
        cmd.text_aliases = {"/restart"};
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Tools;
        add(cmd);
    }
    // /activation
    {
        ChatCommandDefinition cmd;
        cmd.key = "activation"; cmd.native_name = "activation";
        cmd.description = "Set group activation mode.";
        cmd.text_aliases = {"/activation"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        cmd.args = {
            {"mode", "mention or always", CommandArgType::String, false,
             {{"mention","mention"},{"always","always"}}, false, false},
        };
        cmd.args_menu = CommandArgMenuSpec{"mode", std::nullopt};
        add(cmd);
    }
    // /send
    {
        ChatCommandDefinition cmd;
        cmd.key = "send"; cmd.native_name = "send";
        cmd.description = "Set send policy.";
        cmd.text_aliases = {"/send"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Management;
        cmd.args = {
            {"mode", "on, off, or inherit", CommandArgType::String, false,
             {{"on","on"},{"off","off"},{"inherit","inherit"}}, false, false},
        };
        cmd.args_menu = CommandArgMenuSpec{"mode", std::nullopt};
        add(cmd);
    }
    // /reset
    {
        ChatCommandDefinition cmd;
        cmd.key = "reset"; cmd.native_name = "reset";
        cmd.description = "Reset the current session.";
        cmd.text_aliases = {"/reset"};
        cmd.accepts_args = true;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Session;
        add(cmd);
    }
    // /new
    {
        ChatCommandDefinition cmd;
        cmd.key = "new"; cmd.native_name = "new";
        cmd.description = "Start a new session.";
        cmd.text_aliases = {"/new"};
        cmd.accepts_args = true;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Session;
        add(cmd);
    }
    // /compact
    {
        ChatCommandDefinition cmd;
        cmd.key = "compact"; cmd.native_name = "compact";
        cmd.description = "Compact the session context.";
        cmd.text_aliases = {"/compact"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Session;
        cmd.args = {
            {"instructions", "Extra compaction instructions", CommandArgType::String, false, {}, false, true},
        };
        add(cmd);
    }
    // /think
    {
        ChatCommandDefinition cmd;
        cmd.key = "think"; cmd.native_name = "think";
        cmd.description = "Set thinking level.";
        cmd.text_aliases = {"/think", "/thinking", "/t"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        cmd.args = {
            {"level", "off, minimal, low, medium, high, xhigh", CommandArgType::String, false,
             {{"off","off"},{"minimal","minimal"},{"low","low"},
              {"medium","medium"},{"high","high"},{"xhigh","xhigh"},{"adaptive","adaptive"}},
             false, false},
        };
        cmd.args_menu = CommandArgMenuSpec{"level", std::nullopt};
        add(cmd);
    }
    // /verbose
    {
        ChatCommandDefinition cmd;
        cmd.key = "verbose"; cmd.native_name = "verbose";
        cmd.description = "Toggle verbose mode.";
        cmd.text_aliases = {"/verbose", "/v"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        cmd.args = {
            {"mode", "on or off", CommandArgType::String, false,
             {{"on","on"},{"off","off"}}, false, false},
        };
        cmd.args_menu = CommandArgMenuSpec{"mode", std::nullopt};
        add(cmd);
    }
    // /fast
    {
        ChatCommandDefinition cmd;
        cmd.key = "fast"; cmd.native_name = "fast";
        cmd.description = "Toggle fast mode.";
        cmd.text_aliases = {"/fast"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        cmd.args = {
            {"mode", "status, on, or off", CommandArgType::String, false,
             {{"status","status"},{"on","on"},{"off","off"}}, false, false},
        };
        cmd.args_menu = CommandArgMenuSpec{"mode", std::nullopt};
        add(cmd);
    }
    // /reasoning
    {
        ChatCommandDefinition cmd;
        cmd.key = "reasoning"; cmd.native_name = "reasoning";
        cmd.description = "Toggle reasoning visibility.";
        cmd.text_aliases = {"/reasoning", "/reason"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        cmd.args = {
            {"mode", "on, off, or stream", CommandArgType::String, false,
             {{"on","on"},{"off","off"},{"stream","stream"}}, false, false},
        };
        cmd.args_menu = CommandArgMenuSpec{"mode", std::nullopt};
        add(cmd);
    }
    // /elevated
    {
        ChatCommandDefinition cmd;
        cmd.key = "elevated"; cmd.native_name = "elevated";
        cmd.description = "Toggle elevated mode.";
        cmd.text_aliases = {"/elevated", "/elev"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        cmd.args = {
            {"mode", "on, off, ask, or full", CommandArgType::String, false,
             {{"on","on"},{"off","off"},{"ask","ask"},{"full","full"}}, false, false},
        };
        cmd.args_menu = CommandArgMenuSpec{"mode", std::nullopt};
        add(cmd);
    }
    // /exec
    {
        ChatCommandDefinition cmd;
        cmd.key = "exec"; cmd.native_name = "exec";
        cmd.description = "Set exec defaults for this session.";
        cmd.text_aliases = {"/exec"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::None;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        cmd.args = {
            {"host", "sandbox, gateway, or node", CommandArgType::String, false,
             {{"sandbox","sandbox"},{"gateway","gateway"},{"node","node"}}, false, false},
            {"security", "deny, allowlist, or full", CommandArgType::String, false,
             {{"deny","deny"},{"allowlist","allowlist"},{"full","full"}}, false, false},
            {"ask", "off, on-miss, or always", CommandArgType::String, false,
             {{"off","off"},{"on-miss","on-miss"},{"always","always"}}, false, false},
            {"node", "Node id or name", CommandArgType::String, false, {}, false, false},
        };
        add(cmd);
    }
    // /model
    {
        ChatCommandDefinition cmd;
        cmd.key = "model"; cmd.native_name = "model";
        cmd.description = "Show or set the model.";
        cmd.text_aliases = {"/model"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        cmd.args = {
            {"model", "Model id (provider/model or id)", CommandArgType::String, false, {}, false, false},
        };
        add(cmd);
    }
    // /models
    {
        ChatCommandDefinition cmd;
        cmd.key = "models"; cmd.native_name = "models";
        cmd.description = "List model providers or provider models.";
        cmd.text_aliases = {"/models"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::None;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        add(cmd);
    }
    // /queue
    {
        ChatCommandDefinition cmd;
        cmd.key = "queue"; cmd.native_name = "queue";
        cmd.description = "Adjust queue settings.";
        cmd.text_aliases = {"/queue"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::None;
        cmd.scope = CommandScope::Both;
        cmd.category = CommandCategory::Options;
        cmd.args = {
            {"mode", "queue mode", CommandArgType::String, false,
             {{"steer","steer"},{"interrupt","interrupt"},{"followup","followup"},
              {"collect","collect"},{"steer-backlog","steer-backlog"}}, false, false},
            {"debounce", "debounce duration (e.g. 500ms, 2s)", CommandArgType::String, false, {}, false, false},
            {"cap", "queue cap", CommandArgType::Number, false, {}, false, false},
            {"drop", "drop policy", CommandArgType::String, false,
             {{"old","old"},{"new","new"},{"summarize","summarize"}}, false, false},
        };
        add(cmd);
    }
    // /bash (text-only)
    {
        ChatCommandDefinition cmd;
        cmd.key = "bash";
        cmd.description = "Run host shell commands (host-only).";
        cmd.text_aliases = {"/bash"};
        cmd.accepts_args = true;
        cmd.args_parsing = CommandArgsParsing::Positional;
        cmd.scope = CommandScope::Text;
        cmd.category = CommandCategory::Tools;
        cmd.args = {
            {"command", "Shell command", CommandArgType::String, false, {}, false, true},
        };
        add(cmd);
    }

    // Build native command surfaces (channels with native commands)
    native_command_surfaces_ = {"discord", "telegram", "slack"};

    // Invalidate detection cache
    cached_detection_.reset();
}

// ── Resolve native name ────────────────────────────────────────────────────

std::string CommandsRegistry::resolve_native_name(
    const ChatCommandDefinition& command,
    const std::string& provider) const
{
    if (!command.native_name) return "";
    if (!provider.empty()) {
        auto pit = NATIVE_NAME_OVERRIDES.find(provider);
        if (pit != NATIVE_NAME_OVERRIDES.end()) {
            auto oit = pit->second.find(command.key);
            if (oit != pit->second.end()) return oit->second;
        }
    }
    return *command.native_name;
}

// ── Build text alias map ───────────────────────────────────────────────────

std::map<std::string, std::pair<std::string, bool>>
CommandsRegistry::build_text_alias_map() const {
    std::map<std::string, std::pair<std::string, bool>> map;
    for (const auto& cmd : commands_) {
        const std::string canonical = cmd.text_aliases.empty()
            ? "/" + cmd.key
            : cmd.text_aliases[0];
        const bool accepts_args = cmd.accepts_args;
        for (const auto& alias : cmd.text_aliases) {
            std::string normalized = trim_lower(alias);
            if (normalized.empty()) continue;
            if (map.find(normalized) == map.end()) {
                map[normalized] = {canonical, accepts_args};
            }
        }
    }
    return map;
}

// ── Rebuild detection cache ────────────────────────────────────────────────

void CommandsRegistry::rebuild_detection_cache() const {
    CommandDetection det;
    std::vector<std::string> patterns;

    for (const auto& cmd : commands_) {
        for (const auto& alias : cmd.text_aliases) {
            std::string normalized = trim_lower(alias);
            if (normalized.empty()) continue;
            det.exact.insert(normalized);
            std::string escaped = escape_regex(normalized);
            if (escaped.empty()) continue;
            if (cmd.accepts_args) {
                patterns.push_back(escaped + R"((?:\s+.+|\s*:\s*.*)?)");
            } else {
                patterns.push_back(escaped + R"((?:\s*:\s*)?)");
            }
        }
    }

    if (!patterns.empty()) {
        std::string pattern = "^(?:";
        for (size_t i = 0; i < patterns.size(); ++i) {
            if (i > 0) pattern += "|";
            pattern += patterns[i];
        }
        pattern += ")$";
        det.regex = std::regex(pattern, std::regex::icase);
    } else {
        det.regex = std::regex("$^");  // never matches
    }

    cached_detection_ = std::move(det);
}

// ── Public API ─────────────────────────────────────────────────────────────

std::vector<ChatCommandDefinition> CommandsRegistry::list_chat_commands() const {
    return commands_;
}

std::vector<NativeCommandSpec> CommandsRegistry::list_native_command_specs(
    const std::string& provider) const
{
    std::vector<NativeCommandSpec> specs;
    for (const auto& cmd : commands_) {
        if (cmd.scope == CommandScope::Text) continue;
        if (!cmd.native_name) continue;
        NativeCommandSpec spec;
        spec.name = resolve_native_name(cmd, provider);
        spec.description = cmd.description;
        spec.accepts_args = cmd.accepts_args;
        spec.args = cmd.args;
        specs.push_back(std::move(spec));
    }
    return specs;
}

const ChatCommandDefinition* CommandsRegistry::find_command_by_native_name(
    const std::string& name,
    const std::string& provider) const
{
    std::string normalized = trim_lower(name);
    for (const auto& cmd : commands_) {
        if (cmd.scope == CommandScope::Text) continue;
        std::string native = trim_lower(resolve_native_name(cmd, provider));
        if (native == normalized) return &cmd;
    }
    return nullptr;
}

std::string CommandsRegistry::normalize_command_body(
    const std::string& raw,
    const CommandNormalizeOptions& options) const
{
    // Trim
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return raw;
    size_t e = raw.find_last_not_of(" \t\r\n");
    std::string trimmed = raw.substr(s, e - s + 1);

    if (trimmed.empty() || trimmed[0] != '/') return trimmed;

    // Take only first line
    size_t nl = trimmed.find('\n');
    std::string single_line = (nl == std::string::npos) ? trimmed : trimmed.substr(0, nl);
    // Trim single_line
    size_t se = single_line.find_last_not_of(" \t\r\n");
    if (se != std::string::npos) single_line = single_line.substr(0, se + 1);

    // Normalize colon syntax: /cmd:args → /cmd args
    std::string normalized = single_line;
    {
        static const std::regex colon_re(R"(^/([^\s:]+)\s*:(.*))");
        std::smatch m;
        if (std::regex_match(single_line, m, colon_re)) {
            std::string cmd_part = m[1].str();
            std::string rest = m[2].str();
            // Trim rest from left
            size_t rs = rest.find_first_not_of(" \t");
            if (rs != std::string::npos) rest = rest.substr(rs);
            normalized = rest.empty() ? "/" + cmd_part : "/" + cmd_part + " " + rest;
        }
    }

    // Strip @bot_username mention: /cmd@botname → /cmd
    if (options.bot_username) {
        std::string bot_lower = trim_lower(*options.bot_username);
        if (!bot_lower.empty()) {
            static const std::regex mention_re(R"(^/([^\s@]+)@([^\s]+)(.*))");
            std::smatch m;
            if (std::regex_match(normalized, m, mention_re)) {
                std::string mentioned = trim_lower(m[2].str());
                if (mentioned == bot_lower) {
                    normalized = "/" + m[1].str() + m[3].str();
                }
            }
        }
    }

    // Resolve text alias
    auto alias_map = build_text_alias_map();
    std::string lowered = trim_lower(normalized);
    auto it = alias_map.find(lowered);
    if (it != alias_map.end()) {
        return it->second.first;
    }

    // Try token-based alias resolution
    static const std::regex token_re(R"(^/([^\s]+)(?:\s+([\s\S]+))?$)");
    std::smatch tm;
    if (std::regex_match(normalized, tm, token_re)) {
        std::string token_key = "/" + trim_lower(tm[1].str());
        auto tit = alias_map.find(token_key);
        if (tit != alias_map.end()) {
            std::string rest = tm[2].matched ? tm[2].str() : "";
            // Trim rest from left
            size_t rs = rest.find_first_not_of(" \t");
            if (rs != std::string::npos) rest = rest.substr(rs);
            if (!rest.empty() && !tit->second.second) {
                // Command doesn't accept args — don't normalize
                return normalized;
            }
            return rest.empty() ? tit->second.first : tit->second.first + " " + rest;
        }
    }

    return normalized;
}

bool CommandsRegistry::is_command_message(const std::string& raw) const {
    std::string trimmed = normalize_command_body(raw);
    return !trimmed.empty() && trimmed[0] == '/';
}

const CommandDetection& CommandsRegistry::get_command_detection() const {
    if (!cached_detection_) {
        rebuild_detection_cache();
    }
    return *cached_detection_;
}

std::optional<std::string> CommandsRegistry::maybe_resolve_text_alias(
    const std::string& raw) const
{
    std::string trimmed = trim_lower(normalize_command_body(raw));
    if (trimmed.empty() || trimmed[0] != '/') return std::nullopt;

    const auto& det = get_command_detection();
    if (det.exact.count(trimmed)) return trimmed;

    if (!std::regex_match(trimmed, det.regex)) return std::nullopt;

    // Extract token
    static const std::regex token_re(R"(^/([^\s:]+)(?:\s|$))");
    std::smatch m;
    if (!std::regex_search(trimmed, m, token_re)) return std::nullopt;
    std::string token_key = "/" + m[1].str();

    auto alias_map = build_text_alias_map();
    return alias_map.count(token_key) ? std::optional<std::string>{token_key} : std::nullopt;
}

std::optional<CommandsRegistry::ResolvedTextCommand>
CommandsRegistry::resolve_text_command(const std::string& raw) const {
    std::string trimmed = normalize_command_body(raw);
    auto alias = maybe_resolve_text_alias(trimmed);
    if (!alias) return std::nullopt;

    auto alias_map = build_text_alias_map();
    auto it = alias_map.find(*alias);
    if (it == alias_map.end()) return std::nullopt;

    // Find command by canonical alias
    const ChatCommandDefinition* found = nullptr;
    for (const auto& cmd : commands_) {
        for (const auto& ta : cmd.text_aliases) {
            if (trim_lower(ta) == *alias) {
                found = &cmd;
                break;
            }
        }
        if (found) break;
    }
    if (!found) return std::nullopt;

    if (!found->accepts_args) {
        return ResolvedTextCommand{found, std::nullopt};
    }

    // Extract args after alias
    std::string args_str;
    if (trimmed.size() > alias->size()) {
        args_str = trimmed.substr(alias->size());
        size_t as = args_str.find_first_not_of(" \t");
        if (as != std::string::npos) args_str = args_str.substr(as);
        else args_str.clear();
    }

    return ResolvedTextCommand{found, args_str.empty() ? std::nullopt : std::optional<std::string>{args_str}};
}

std::optional<CommandArgs> CommandsRegistry::parse_command_args(
    const ChatCommandDefinition& command,
    const std::string& raw) const
{
    std::string trimmed = raw;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return std::nullopt;

    if (command.args.empty() || command.args_parsing == CommandArgsParsing::None) {
        return CommandArgs{trimmed, std::nullopt};
    }

    // Positional parsing
    CommandArgValues values;
    std::vector<std::string> tokens;
    std::istringstream iss(trimmed);
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);

    size_t idx = 0;
    for (const auto& def : command.args) {
        if (idx >= tokens.size()) break;
        if (def.capture_remaining) {
            std::string rest;
            for (size_t i = idx; i < tokens.size(); ++i) {
                if (i > idx) rest += " ";
                rest += tokens[i];
            }
            values[def.name] = rest;
            idx = tokens.size();
            break;
        }
        values[def.name] = tokens[idx++];
    }

    return CommandArgs{trimmed, values};
}

std::optional<std::string> CommandsRegistry::serialize_command_args(
    const ChatCommandDefinition& command,
    const std::optional<CommandArgs>& args) const
{
    if (!args) return std::nullopt;
    if (args->raw) {
        std::string r = *args->raw;
        size_t s = r.find_first_not_of(" \t\r\n");
        if (s != std::string::npos) {
            size_t e = r.find_last_not_of(" \t\r\n");
            r = r.substr(s, e - s + 1);
        }
        if (!r.empty()) return r;
    }
    if (!args->values || command.args.empty()) return std::nullopt;

    // Format positional args
    std::string result;
    for (const auto& def : command.args) {
        auto it = args->values->find(def.name);
        if (it == args->values->end()) continue;
        std::string val = it->second;
        size_t vs = val.find_first_not_of(" \t\r\n");
        if (vs == std::string::npos) continue;
        size_t ve = val.find_last_not_of(" \t\r\n");
        val = val.substr(vs, ve - vs + 1);
        if (val.empty()) continue;
        if (!result.empty()) result += " ";
        result += val;
        if (def.capture_remaining) break;
    }
    return result.empty() ? std::nullopt : std::optional<std::string>{result};
}

std::string CommandsRegistry::build_command_text(
    const std::string& command_name,
    const std::string& args)
{
    std::string trimmed_args = args;
    size_t s = trimmed_args.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) trimmed_args.clear();
    else {
        size_t e = trimmed_args.find_last_not_of(" \t\r\n");
        trimmed_args = trimmed_args.substr(s, e - s + 1);
    }
    return trimmed_args.empty() ? "/" + command_name : "/" + command_name + " " + trimmed_args;
}

std::string CommandsRegistry::build_command_text_from_args(
    const ChatCommandDefinition& command,
    const std::optional<CommandArgs>& args) const
{
    std::string name = command.native_name.value_or(command.key);
    auto serialized = serialize_command_args(command, args);
    return build_command_text(name, serialized.value_or(""));
}

bool CommandsRegistry::is_native_command_surface(const std::string& surface) const {
    if (surface.empty()) return false;
    return native_command_surfaces_.count(trim_lower(surface)) > 0;
}

bool CommandsRegistry::should_handle_text_commands(
    const ShouldHandleTextCommandsParams& params) const
{
    if (params.command_source == "native") return true;
    if (params.commands_text_enabled) return true;
    return !is_native_command_surface(params.surface);
}

// ── Singleton ──────────────────────────────────────────────────────────────

CommandsRegistry& get_commands_registry() {
    static CommandsRegistry instance;
    return instance;
}

} // namespace auto_reply
} // namespace hunterclaw
