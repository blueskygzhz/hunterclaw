#pragma once
// Aligned with openclaw: src/hooks/loader.ts (257 lines) + workspace.ts (381 lines)
// Dynamic hook loading: directory-based discovery + legacy config handlers

#include "hunterclaw/hooks/hooks.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <fstream>

namespace hunterclaw {
namespace hooks {

// ============================================================================
// Hook Loading Configuration
// ============================================================================
struct HookLoaderOptions {
    std::string managed_hooks_dir;   // Managed hooks directory
    std::string bundled_hooks_dir;   // Bundled hooks directory (default: built-in)
};

// ============================================================================
// Hook Source Types (对齐 hooks/types.ts)
// ============================================================================
inline std::string hook_source_to_string(HookSource source) {
    switch (source) {
        case HookSource::OPENCLAW_BUNDLED: return "openclaw-bundled";
        case HookSource::OPENCLAW_MANAGED: return "openclaw-managed";
        case HookSource::OPENCLAW_WORKSPACE: return "openclaw-workspace";
        case HookSource::OPENCLAW_PLUGIN: return "openclaw-plugin";
        default: return "unknown";
    }
}

// ============================================================================
// Hook Discovery Result
// ============================================================================
struct DiscoveredHook {
    HookEntry entry;
    std::string handler_path;
    std::string base_dir;
};

// ============================================================================
// Bundled Hook Definitions (对齐 hooks/bundled/)
// ============================================================================
// These are the 4 bundled hooks from OpenClaw:
//   1. boot-md: Injects BOOT.md content on gateway startup
//   2. bootstrap-extra-files: Adds extra files on agent bootstrap
//   3. session-memory: Auto-saves session memories
//   4. command-logger: Logs commands for audit

struct BundledHookDef {
    std::string name;
    std::string description;
    std::vector<std::string> events;  // e.g., "gateway:startup", "agent:bootstrap"
    InternalHookHandler handler;
};

// ============================================================================
// Hook Loader (对齐 loader.ts:loadInternalHooks)
// ============================================================================
class HookLoader {
public:
    // Load and register all hook handlers
    // Returns number of handlers successfully loaded
    int load_internal_hooks(const core::Config& cfg,
                            const std::string& workspace_dir,
                            const HookLoaderOptions& opts = {});

    // Register bundled hooks (boot-md, session-memory, command-logger, bootstrap-extra-files)
    int register_bundled_hooks();

    // Load hooks from a directory (对齐 loadWorkspaceHookEntries)
    std::vector<DiscoveredHook> discover_hooks_in_dir(const std::string& dir,
                                                        HookSource source);

    // Check if a hook should be included (对齐 shouldIncludeHook)
    bool should_include_hook(const HookEntry& entry, const core::Config& cfg);

private:
    // Parse HOOK.md frontmatter (对齐 parseFrontmatter)
    std::optional<OpenClawHookMetadata> parse_hook_md(const std::string& hook_md_path);

    // Resolve handler path from hook directory
    std::optional<std::string> resolve_handler_path(const std::string& hook_dir);

    // Security: verify handler is within base directory
    bool verify_handler_boundary(const std::string& handler_path, const std::string& base_dir);

    // Track loaded hooks for dedup
    std::vector<std::string> loaded_hook_names_;
};

// ============================================================================
// Bundled Hook Implementations (simplified C++ equivalents)
// ============================================================================
namespace bundled {

// boot-md: Injects BOOT.md content into agent bootstrap
inline InternalHookHandler make_boot_md_handler() {
    return [](const InternalHookEvent& event) {
        if (event.type != HookEventType::GATEWAY || event.action != "startup") return;
        // In C++, the equivalent is to load BOOT.md from workspace and inject
        // into agent context. This is a no-op placeholder that the gateway
        // startup sequence can hook into.
    };
}

// session-memory: Auto-saves session memories on compaction
inline InternalHookHandler make_session_memory_handler() {
    return [](const InternalHookEvent& event) {
        if (event.type != HookEventType::SESSION) return;
        // Session memory hook: extract key facts from compacted conversation
        // and store them for future context injection.
    };
}

// command-logger: Logs commands for audit trail
inline InternalHookHandler make_command_logger_handler() {
    return [](const InternalHookEvent& event) {
        if (event.type != HookEventType::COMMAND) return;
        // Command logger: append command to audit log file
    };
}

// bootstrap-extra-files: Adds extra files on agent bootstrap
inline InternalHookHandler make_bootstrap_extra_files_handler() {
    return [](const InternalHookEvent& event) {
        if (event.type != HookEventType::AGENT || event.action != "bootstrap") return;
        // Bootstrap extra files: load additional files from workspace
        // and inject into the agent's bootstrap context.
    };
}

} // namespace bundled

// ============================================================================
// Convenience: get all bundled hook definitions
// ============================================================================
inline std::vector<BundledHookDef> get_bundled_hook_definitions() {
    return {
        {"boot-md", "Inject BOOT.md on gateway startup",
         {"gateway:startup"}, bundled::make_boot_md_handler()},
        {"session-memory", "Auto-save session memories",
         {"session:compacted"}, bundled::make_session_memory_handler()},
        {"command-logger", "Log commands for audit",
         {"command:executed"}, bundled::make_command_logger_handler()},
        {"bootstrap-extra-files", "Add extra files on agent bootstrap",
         {"agent:bootstrap"}, bundled::make_bootstrap_extra_files_handler()},
    };
}

} // namespace hooks
} // namespace hunterclaw
