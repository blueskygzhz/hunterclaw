#pragma once
// Aligned with openclaw: src/hooks/workspace.ts (380 lines)
// Hook workspace discovery: bundled, plugin, managed, workspace, extra dirs

#include "hunterclaw/hooks/hooks.h"
#include "hunterclaw/hooks/hook_policy.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace hunterclaw {
namespace hooks {

// ============================================================================
// Hook Discovery Options
// ============================================================================

struct WorkspaceHookDiscoveryOptions {
    std::optional<core::Config> config;
    std::optional<std::string> managed_hooks_dir;
    std::optional<std::string> bundled_hooks_dir;
};

// ============================================================================
// Hook Directory Discovery (对齐 workspace.ts functions)
// ============================================================================

/// Load hooks from a single directory containing hook subdirectories
/// 对齐: loadHookEntriesFromDir
std::vector<HookEntry> load_hook_entries_from_dir(
    const std::string& dir,
    HookSource source,
    const std::optional<std::string>& plugin_id = std::nullopt);

/// Discover all workspace hook entries from all sources
/// 对齐: discoverWorkspaceHookEntries
std::vector<HookEntry> discover_workspace_hook_entries(
    const std::string& workspace_dir,
    const WorkspaceHookDiscoveryOptions& opts = {});

/// Load and resolve workspace hook entries (with priority dedup)
/// 对齐: loadWorkspaceHookEntries
std::vector<HookEntry> load_workspace_hook_entries(
    const std::string& workspace_dir,
    const WorkspaceHookDiscoveryOptions& opts = {});

/// Build hook snapshot for caching
/// 对齐: buildWorkspaceHookSnapshot
HookSnapshot build_workspace_hook_snapshot(
    const std::string& workspace_dir,
    const WorkspaceHookDiscoveryOptions& opts = {});

// ============================================================================
// Hook Frontmatter Parsing (对齐 frontmatter.ts)
// ============================================================================

/// Parse HOOK.md frontmatter block into key-value pairs
ParsedHookFrontmatter parse_hook_frontmatter(const std::string& content);

/// Resolve OpenClaw metadata from frontmatter
std::optional<OpenClawHookMetadata> resolve_openclaw_metadata(
    const ParsedHookFrontmatter& frontmatter);

/// Resolve hook invocation policy from frontmatter
HookInvocationPolicy resolve_hook_invocation_policy(
    const ParsedHookFrontmatter& frontmatter);

/// Resolve hook key from name and entry
std::string resolve_hook_key(const std::string& hook_name,
                              const std::optional<HookEntry>& entry = std::nullopt);

}  // namespace hooks
}  // namespace hunterclaw
