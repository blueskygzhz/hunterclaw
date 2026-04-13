// Aligned with openclaw:
//   src/auto-reply/skill-commands.ts (135行)
//   src/auto-reply/skill-commands-base.ts (100行)
// Skill 命令发现、解析、调用解析

#include "hunterclaw/auto_reply/skill_commands.h"
#include "hunterclaw/auto_reply/commands_registry.h"
#include "hunterclaw/agents/agent_scope.h"
#include <algorithm>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Helpers
// ============================================================================

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// ============================================================================
// list_reserved_chat_slash_command_names
// 对齐 OC: listReservedChatSlashCommandNames (skill-commands-base.ts:8-29)
// ============================================================================

std::set<std::string> list_reserved_chat_slash_command_names(
    const std::vector<std::string>& extra_names) {
    std::set<std::string> reserved;
    const auto& registry = get_commands_registry();
    const auto commands = registry.list_chat_commands();

    for (const auto& command : commands) {
        if (command.native_name) {
            std::string lower = to_lower(trim(*command.native_name));
            if (!lower.empty()) {
                reserved.insert(lower);
            }
        }
        for (const auto& alias : command.text_aliases) {
            std::string trimmed = trim(alias);
            if (trimmed.empty() || trimmed[0] != '/') continue;
            std::string name = to_lower(trimmed.substr(1));
            if (!name.empty()) {
                reserved.insert(name);
            }
        }
    }

    for (const auto& name : extra_names) {
        std::string lower = to_lower(trim(name));
        if (!lower.empty()) {
            reserved.insert(lower);
        }
    }

    return reserved;
}

// ============================================================================
// normalize_skill_command_lookup
// 对齐 OC: normalizeSkillCommandLookup (skill-commands-base.ts:31-33)
// (lowercaseOptionalString(value) ?? "").replace(/[\s_]+/g, "-")
// ============================================================================

std::string normalize_skill_command_lookup(const std::string& value) {
    std::string lower = to_lower(trim(value));
    // Replace whitespace and underscores with hyphens
    std::string result;
    result.reserve(lower.size());
    for (char c : lower) {
        if (c == ' ' || c == '\t' || c == '_') {
            result += '-';
        } else {
            result += c;
        }
    }
    // Collapse multiple hyphens
    std::string collapsed;
    bool last_was_hyphen = false;
    for (char c : result) {
        if (c == '-') {
            if (!last_was_hyphen) {
                collapsed += c;
            }
            last_was_hyphen = true;
        } else {
            collapsed += c;
            last_was_hyphen = false;
        }
    }
    return collapsed;
}

// ============================================================================
// find_skill_command
// 对齐 OC: findSkillCommand (skill-commands-base.ts:35-57)
// ============================================================================

const skills::SkillCommandSpec* find_skill_command(
    const std::vector<skills::SkillCommandSpec>& skill_commands,
    const std::string& raw_name) {
    std::string trimmed = trim(raw_name);
    if (trimmed.empty()) return nullptr;

    std::string lowered = to_lower(trimmed);
    std::string normalized = normalize_skill_command_lookup(trimmed);

    for (const auto& entry : skill_commands) {
        // Exact match by name
        if (to_lower(trim(entry.name)) == lowered) return &entry;
        // Exact match by skill_name
        if (to_lower(trim(entry.skill_name)) == lowered) return &entry;
        // Normalized match
        if (normalize_skill_command_lookup(entry.name) == normalized) return &entry;
        if (normalize_skill_command_lookup(entry.skill_name) == normalized) return &entry;
    }
    return nullptr;
}

// ============================================================================
// resolve_skill_command_invocation
// 对齐 OC: resolveSkillCommandInvocation (skill-commands-base.ts:59-99)
// ============================================================================

std::optional<SkillCommandInvocation> resolve_skill_command_invocation(
    const std::string& command_body_normalized,
    const std::vector<skills::SkillCommandSpec>& skill_commands) {
    std::string trimmed = trim(command_body_normalized);
    if (trimmed.empty() || trimmed[0] != '/') return std::nullopt;

    // Parse: /command [args]
    std::regex cmd_re(R"(^/([^\s]+)(?:\s+([\s\S]+))?$)");
    std::smatch match;
    if (!std::regex_match(trimmed, match, cmd_re)) return std::nullopt;

    std::string command_name = to_lower(trim(match[1].str()));
    if (command_name.empty()) return std::nullopt;

    // Handle `/skill <name> [input]`
    if (command_name == "skill") {
        std::string remainder = match[2].matched ? trim(match[2].str()) : "";
        if (remainder.empty()) return std::nullopt;

        std::regex skill_re(R"(^([^\s]+)(?:\s+([\s\S]+))?$)");
        std::smatch skill_match;
        if (!std::regex_match(remainder, skill_match, skill_re)) return std::nullopt;

        const auto* cmd = find_skill_command(skill_commands, skill_match[1].str());
        if (!cmd) return std::nullopt;

        SkillCommandInvocation result;
        result.command = cmd;
        if (skill_match[2].matched) {
            std::string args = trim(skill_match[2].str());
            if (!args.empty()) result.args = args;
        }
        return result;
    }

    // Handle `/<skill-command-name> [input]`
    const skills::SkillCommandSpec* found = nullptr;
    for (const auto& entry : skill_commands) {
        if (to_lower(trim(entry.name)) == command_name) {
            found = &entry;
            break;
        }
    }
    if (!found) return std::nullopt;

    SkillCommandInvocation result;
    result.command = found;
    if (match[2].matched) {
        std::string args = trim(match[2].str());
        if (!args.empty()) result.args = args;
    }
    return result;
}

// ============================================================================
// dedupe_by_skill_name
// 对齐 OC: dedupeBySkillName (skill-commands.ts:44-58)
// ============================================================================

std::vector<skills::SkillCommandSpec> dedupe_by_skill_name(
    const std::vector<skills::SkillCommandSpec>& commands) {
    std::set<std::string> seen;
    std::vector<skills::SkillCommandSpec> out;
    out.reserve(commands.size());

    for (const auto& cmd : commands) {
        std::string key = to_lower(trim(cmd.skill_name));
        if (!key.empty() && seen.count(key)) continue;
        if (!key.empty()) seen.insert(key);
        out.push_back(cmd);
    }
    return out;
}

// ============================================================================
// merge_skill_filters
// 对齐 OC: mergeSkillFilters (skill-commands.ts:64-78)
// ============================================================================

std::optional<std::vector<std::string>> merge_skill_filters(
    const std::optional<std::vector<std::string>>& existing,
    const std::optional<std::vector<std::string>>& incoming) {
    // undefined = no allowlist (unrestricted)
    // If any is unrestricted, keep unrestricted
    if (!existing.has_value() || !incoming.has_value()) {
        return std::nullopt;
    }
    // Empty allowlist contributes no skills but doesn't widen to unrestricted
    if (existing->empty()) {
        std::set<std::string> s(incoming->begin(), incoming->end());
        return std::vector<std::string>(s.begin(), s.end());
    }
    if (incoming->empty()) {
        std::set<std::string> s(existing->begin(), existing->end());
        return std::vector<std::string>(s.begin(), s.end());
    }
    // Merge both
    std::set<std::string> merged(existing->begin(), existing->end());
    merged.insert(incoming->begin(), incoming->end());
    return std::vector<std::string>(merged.begin(), merged.end());
}

// ============================================================================
// list_skill_commands_for_workspace
// 对齐 OC: listSkillCommandsForWorkspace (skill-commands.ts:22-42)
// ============================================================================

std::vector<skills::SkillCommandSpec> list_skill_commands_for_workspace(
    const ListSkillCommandsForWorkspaceParams& params) {
    // Delegate to SkillManager::build_workspace_skill_command_specs
    // which is already aligned with OC buildWorkspaceSkillCommandSpecs
    auto reserved = list_reserved_chat_slash_command_names();

    // Resolve skill manager from workspace
    auto skill_mgr = agents::resolve_workspace_skill_manager(params.workspace_dir);
    if (!skill_mgr) return {};

    skills::SkillEligibilityContext ctx;
    ctx.advertise_exec_node = true;  // default

    return skill_mgr->build_workspace_skill_command_specs(
        ctx, params.skill_filter, reserved);
}

// ============================================================================
// list_skill_commands_for_agents
// 对齐 OC: listSkillCommandsForAgents (skill-commands.ts:60-130)
// ============================================================================

std::vector<skills::SkillCommandSpec> list_skill_commands_for_agents(
    const ListSkillCommandsForAgentsParams& params) {
    auto used = list_reserved_chat_slash_command_names();
    std::vector<skills::SkillCommandSpec> entries;

    // Group by canonical workspace to avoid duplicate registration
    // when multiple agents share the same directory
    struct WorkspaceEntry {
        std::string workspace_dir;
        std::optional<std::vector<std::string>> skill_filter;
    };
    std::map<std::string, WorkspaceEntry> workspace_filters;

    std::vector<std::string> agent_ids = params.agent_ids;
    if (agent_ids.empty()) {
        agent_ids = agents::list_agent_ids(params.config_json);
    }

    for (const auto& agent_id : agent_ids) {
        std::string workspace_dir =
            agents::resolve_agent_workspace_dir(params.config_json, agent_id);
        if (!fs::exists(workspace_dir)) continue;

        std::string canonical_dir;
        try {
            canonical_dir = fs::canonical(workspace_dir).string();
        } catch (...) {
            continue;
        }

        auto skill_filter = agents::resolve_agent_skills_filter(
            params.config_json, agent_id);

        auto it = workspace_filters.find(canonical_dir);
        if (it != workspace_filters.end()) {
            it->second.skill_filter = merge_skill_filters(
                it->second.skill_filter, skill_filter);
            continue;
        }
        workspace_filters[canonical_dir] = {workspace_dir, skill_filter};
    }

    for (const auto& [_, ws] : workspace_filters) {
        auto skill_mgr = agents::resolve_workspace_skill_manager(ws.workspace_dir);
        if (!skill_mgr) continue;

        skills::SkillEligibilityContext ctx;
        ctx.advertise_exec_node = true;

        std::vector<std::string> filter =
            ws.skill_filter.value_or(std::vector<std::string>{});
        auto commands = skill_mgr->build_workspace_skill_command_specs(
            ctx, filter, used);

        for (const auto& command : commands) {
            used.insert(to_lower(trim(command.name)));
            entries.push_back(command);
        }
    }

    return dedupe_by_skill_name(entries);
}

}  // namespace auto_reply
}  // namespace hunterclaw
