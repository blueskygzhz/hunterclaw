// Aligned with openclaw:
//   src/auto-reply/skill-commands.ts (135行)
//   src/auto-reply/skill-commands-base.ts (100行)
// Skill 命令发现、解析、调用解析
#pragma once

#include "hunterclaw/auto_reply/commands_registry.h"
#include "hunterclaw/skills/skill.h"
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Reserved command names (对齐 OC: listReservedChatSlashCommandNames)
// ============================================================================

/// Build the set of reserved slash command names from builtin commands.
/// Prevents skill commands from shadowing built-in /help, /status, etc.
/// 对齐 OC: listReservedChatSlashCommandNames (skill-commands-base.ts:8-29)
std::set<std::string> list_reserved_chat_slash_command_names(
    const std::vector<std::string>& extra_names = {});

// ============================================================================
// Skill command invocation resolution (对齐 OC: resolveSkillCommandInvocation)
// ============================================================================

struct SkillCommandInvocation {
    const skills::SkillCommandSpec* command = nullptr;
    std::optional<std::string> args;
};

/// Resolve a skill command invocation from normalized command text.
/// Handles both `/skill <name> [input]` and direct `/<skill-name> [input]`.
/// 对齐 OC: resolveSkillCommandInvocation (skill-commands-base.ts:59-99)
std::optional<SkillCommandInvocation> resolve_skill_command_invocation(
    const std::string& command_body_normalized,
    const std::vector<skills::SkillCommandSpec>& skill_commands);

// ============================================================================
// Workspace skill command listing (对齐 OC: listSkillCommandsForWorkspace)
// ============================================================================

struct ListSkillCommandsForWorkspaceParams {
    std::string workspace_dir;
    std::string config_json;
    std::string agent_id;
    std::vector<std::string> skill_filter;  // empty = unrestricted
};

/// List skill commands for a single workspace.
/// 对齐 OC: listSkillCommandsForWorkspace (skill-commands.ts:22-42)
std::vector<skills::SkillCommandSpec> list_skill_commands_for_workspace(
    const ListSkillCommandsForWorkspaceParams& params);

// ============================================================================
// Agent-wide skill command listing (对齐 OC: listSkillCommandsForAgents)
// ============================================================================

struct ListSkillCommandsForAgentsParams {
    std::string config_json;
    std::vector<std::string> agent_ids;  // empty = use listAgentIds(cfg)
};

/// List skill commands across all agents, deduplicating by skill name.
/// Groups by canonical workspace to avoid duplicate registration when
/// multiple agents share the same directory.
/// 对齐 OC: listSkillCommandsForAgents (skill-commands.ts:60-130)
std::vector<skills::SkillCommandSpec> list_skill_commands_for_agents(
    const ListSkillCommandsForAgentsParams& params);

// ============================================================================
// Internal helpers (对齐 OC: dedupeBySkillName, normalizeSkillCommandLookup)
// ============================================================================

/// Deduplicate skill commands by skill_name (case-insensitive).
/// 对齐 OC: dedupeBySkillName (skill-commands.ts:44-58)
std::vector<skills::SkillCommandSpec> dedupe_by_skill_name(
    const std::vector<skills::SkillCommandSpec>& commands);

/// Normalize a skill command lookup string: lowercase + replace spaces/underscores with hyphens.
/// 对齐 OC: normalizeSkillCommandLookup (skill-commands-base.ts:31-33)
std::string normalize_skill_command_lookup(const std::string& value);

/// Find a skill command by raw name (exact match by name or skill_name, then normalized match).
/// 对齐 OC: findSkillCommand (skill-commands-base.ts:35-57)
const skills::SkillCommandSpec* find_skill_command(
    const std::vector<skills::SkillCommandSpec>& skill_commands,
    const std::string& raw_name);

/// Merge two skill filter lists.
/// undefined (empty vector) = unrestricted; non-empty = explicit allowlist.
/// 对齐 OC: mergeSkillFilters (skill-commands.ts:64-78)
std::optional<std::vector<std::string>> merge_skill_filters(
    const std::optional<std::vector<std::string>>& existing,
    const std::optional<std::vector<std::string>>& incoming);

}  // namespace auto_reply
}  // namespace hunterclaw
