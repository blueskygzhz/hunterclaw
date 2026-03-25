// Aligned with openclaw: src/auto-reply/commands-registry.ts
// Aligned with openclaw: src/auto-reply/commands-registry.types.ts
// Aligned with openclaw: src/auto-reply/commands-registry.data.ts
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <map>
#include <set>
#include <functional>
#include <regex>
#include <memory>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Command types (aligned with OpenClaw: commands-registry.types.ts)
// ============================================================================

enum class CommandScope {
    Text,    // "text"  — text-only (no native slash command)
    Native,  // "native" — native slash command only
    Both     // "both"  — both text and native
};

enum class CommandCategory {
    Session,    // "session"
    Options,    // "options"
    Status,     // "status"
    Management, // "management"
    Media,      // "media"
    Tools,      // "tools"
    Docks       // "docks"
};

enum class CommandArgType {
    String,  // "string"
    Number,  // "number"
    Boolean  // "boolean"
};

enum class CommandArgsParsing {
    None,       // "none"
    Positional  // "positional"
};

struct CommandArgChoice {
    std::string value;
    std::string label;
};

struct CommandArgDefinition {
    std::string name;
    std::string description;
    CommandArgType type = CommandArgType::String;
    bool required = false;
    std::vector<CommandArgChoice> choices;
    bool prefer_autocomplete = false;
    bool capture_remaining = false;
};

struct CommandArgMenuSpec {
    std::string arg;
    std::optional<std::string> title;
};

using CommandArgValue = std::string;  // simplified: all values as string
using CommandArgValues = std::map<std::string, CommandArgValue>;

struct CommandArgs {
    std::optional<std::string> raw;
    std::optional<CommandArgValues> values;
};

struct ChatCommandDefinition {
    std::string key;
    std::optional<std::string> native_name;
    std::string description;
    std::vector<std::string> text_aliases;
    bool accepts_args = false;
    std::vector<CommandArgDefinition> args;
    CommandArgsParsing args_parsing = CommandArgsParsing::None;
    std::optional<CommandArgMenuSpec> args_menu;
    CommandScope scope = CommandScope::Both;
    std::optional<CommandCategory> category;
    // Optional custom formatter for args
    std::function<std::optional<std::string>(const CommandArgValues&)> format_args;
};

struct NativeCommandSpec {
    std::string name;
    std::string description;
    bool accepts_args = false;
    std::vector<CommandArgDefinition> args;
};

struct CommandNormalizeOptions {
    std::optional<std::string> bot_username;
};

struct CommandDetection {
    std::set<std::string> exact;
    std::regex regex;
};

struct ShouldHandleTextCommandsParams {
    bool commands_text_enabled = true;  // cfg.commands?.text !== false
    std::string surface;
    std::string command_source;  // "text" | "native"
};

// ============================================================================
// CommandsRegistry — aligned with OpenClaw commands-registry.ts
// ============================================================================

class CommandsRegistry {
public:
    CommandsRegistry();

    // ── List commands ──────────────────────────────────────────────────────

    // Aligned with OpenClaw: listChatCommands()
    std::vector<ChatCommandDefinition> list_chat_commands() const;

    // Aligned with OpenClaw: listNativeCommandSpecs()
    std::vector<NativeCommandSpec> list_native_command_specs(
        const std::string& provider = "") const;

    // ── Find commands ──────────────────────────────────────────────────────

    // Aligned with OpenClaw: findCommandByNativeName()
    const ChatCommandDefinition* find_command_by_native_name(
        const std::string& name,
        const std::string& provider = "") const;

    // ── Text command resolution ────────────────────────────────────────────

    // Aligned with OpenClaw: normalizeCommandBody()
    std::string normalize_command_body(
        const std::string& raw,
        const CommandNormalizeOptions& options = {}) const;

    // Aligned with OpenClaw: isCommandMessage()
    bool is_command_message(const std::string& raw) const;

    // Aligned with OpenClaw: getCommandDetection()
    const CommandDetection& get_command_detection() const;

    // Aligned with OpenClaw: maybeResolveTextAlias()
    std::optional<std::string> maybe_resolve_text_alias(const std::string& raw) const;

    struct ResolvedTextCommand {
        const ChatCommandDefinition* command = nullptr;
        std::optional<std::string> args;
    };

    // Aligned with OpenClaw: resolveTextCommand()
    std::optional<ResolvedTextCommand> resolve_text_command(const std::string& raw) const;

    // ── Args parsing ───────────────────────────────────────────────────────

    // Aligned with OpenClaw: parseCommandArgs()
    std::optional<CommandArgs> parse_command_args(
        const ChatCommandDefinition& command,
        const std::string& raw = "") const;

    // Aligned with OpenClaw: serializeCommandArgs()
    std::optional<std::string> serialize_command_args(
        const ChatCommandDefinition& command,
        const std::optional<CommandArgs>& args) const;

    // Aligned with OpenClaw: buildCommandText()
    static std::string build_command_text(
        const std::string& command_name,
        const std::string& args = "");

    // Aligned with OpenClaw: buildCommandTextFromArgs()
    std::string build_command_text_from_args(
        const ChatCommandDefinition& command,
        const std::optional<CommandArgs>& args = std::nullopt) const;

    // ── Native command surfaces ────────────────────────────────────────────

    // Aligned with OpenClaw: isNativeCommandSurface()
    bool is_native_command_surface(const std::string& surface) const;

    // Aligned with OpenClaw: shouldHandleTextCommands()
    bool should_handle_text_commands(const ShouldHandleTextCommandsParams& params) const;

private:
    std::vector<ChatCommandDefinition> commands_;
    std::set<std::string> native_command_surfaces_;

    // Cached detection
    mutable std::optional<CommandDetection> cached_detection_;

    void build_commands();
    void rebuild_detection_cache() const;

    // Internal helpers
    std::map<std::string, std::pair<std::string, bool>> build_text_alias_map() const;
    std::string resolve_native_name(
        const ChatCommandDefinition& command,
        const std::string& provider = "") const;
};

// ============================================================================
// Global registry accessor (singleton pattern)
// Aligned with OpenClaw: getChatCommands() / getNativeCommandSurfaces()
// ============================================================================

CommandsRegistry& get_commands_registry();

// Convenience free functions
inline std::vector<ChatCommandDefinition> list_chat_commands() {
    return get_commands_registry().list_chat_commands();
}

inline bool is_command_message(const std::string& raw) {
    return get_commands_registry().is_command_message(raw);
}

inline std::optional<CommandsRegistry::ResolvedTextCommand> resolve_text_command(
    const std::string& raw) {
    return get_commands_registry().resolve_text_command(raw);
}

} // namespace auto_reply
} // namespace hunterclaw
