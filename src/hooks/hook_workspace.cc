#include "hunterclaw/hooks/hook_workspace.h"
#include "hunterclaw/hooks/hook_loader.h"  // hook_source_to_string
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace hooks {

// ============================================================================
// Frontmatter Parsing (对齐 frontmatter.ts)
// ============================================================================

ParsedHookFrontmatter parse_hook_frontmatter(const std::string& content) {
    ParsedHookFrontmatter result;
    // Simple YAML-like frontmatter between --- delimiters
    if (content.substr(0, 3) != "---") return result;
    auto end_pos = content.find("\n---", 3);
    if (end_pos == std::string::npos) return result;
    
    std::string block = content.substr(3, end_pos - 3);
    std::istringstream stream(block);
    std::string line;
    while (std::getline(stream, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // Trim whitespace
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(key);
        trim(value);
        if (!key.empty()) {
            result[key] = value;
        }
    }
    return result;
}

std::optional<OpenClawHookMetadata> resolve_openclaw_metadata(
    const ParsedHookFrontmatter& frontmatter
) {
    // Check for openclaw block marker
    auto it = frontmatter.find("openclaw");
    if (it == frontmatter.end() && frontmatter.find("events") == frontmatter.end()) {
        return std::nullopt;
    }
    
    OpenClawHookMetadata meta;
    
    // Parse events (comma-separated)
    auto events_it = frontmatter.find("events");
    if (events_it != frontmatter.end()) {
        std::istringstream ss(events_it->second);
        std::string event;
        while (std::getline(ss, event, ',')) {
            auto trim = [](std::string& s) {
                s.erase(0, s.find_first_not_of(" \t\r\n[]\"'"));
                s.erase(s.find_last_not_of(" \t\r\n[]\"'") + 1);
            };
            trim(event);
            if (!event.empty()) meta.events.push_back(event);
        }
    }
    
    // Parse always
    auto always_it = frontmatter.find("always");
    if (always_it != frontmatter.end()) {
        meta.always = (always_it->second == "true");
    }
    
    // Parse hookKey
    auto key_it = frontmatter.find("hookKey");
    if (key_it != frontmatter.end() && !key_it->second.empty()) {
        meta.hook_key = key_it->second;
    }
    
    // Parse emoji
    auto emoji_it = frontmatter.find("emoji");
    if (emoji_it != frontmatter.end() && !emoji_it->second.empty()) {
        meta.emoji = emoji_it->second;
    }
    
    // Parse export
    auto export_it = frontmatter.find("export");
    if (export_it != frontmatter.end() && !export_it->second.empty()) {
        meta.export_name = export_it->second;
    }
    
    return meta;
}

HookInvocationPolicy resolve_hook_invocation_policy(
    const ParsedHookFrontmatter& frontmatter
) {
    HookInvocationPolicy policy;
    auto it = frontmatter.find("enabled");
    if (it != frontmatter.end()) {
        policy.enabled = (it->second != "false");
    }
    return policy;
}

std::string resolve_hook_key(const std::string& hook_name,
                              const std::optional<HookEntry>& entry) {
    if (entry && entry->metadata && entry->metadata->hook_key) {
        return *entry->metadata->hook_key;
    }
    return hook_name;
}

// ============================================================================
// Single Hook Directory Loading (对齐 workspace.ts:83-148)
// ============================================================================

static std::optional<std::string> find_handler_file(const std::string& hook_dir) {
    static const std::vector<std::string> candidates = {
        "handler.cc", "handler.cpp", "handler.h", "handler.ts", "handler.js",
        "index.cc", "index.cpp", "index.ts", "index.js"
    };
    for (const auto& candidate : candidates) {
        auto path = fs::path(hook_dir) / candidate;
        if (fs::exists(path)) return path.string();
    }
    return std::nullopt;
}

struct LoadedHook {
    Hook hook;
    ParsedHookFrontmatter frontmatter;
};

static std::optional<LoadedHook> load_hook_from_dir(
    const std::string& hook_dir,
    HookSource source,
    const std::optional<std::string>& plugin_id = std::nullopt,
    const std::optional<std::string>& name_hint = std::nullopt
) {
    auto hook_md_path = fs::path(hook_dir) / "HOOK.md";
    if (!fs::exists(hook_md_path)) return std::nullopt;
    
    try {
        std::ifstream ifs(hook_md_path.string());
        if (!ifs.is_open()) return std::nullopt;
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        
        auto frontmatter = parse_hook_frontmatter(content);
        
        std::string name;
        auto name_it = frontmatter.find("name");
        if (name_it != frontmatter.end() && !name_it->second.empty()) {
            name = name_it->second;
        } else if (name_hint) {
            name = *name_hint;
        } else {
            name = fs::path(hook_dir).filename().string();
        }
        
        std::string description;
        auto desc_it = frontmatter.find("description");
        if (desc_it != frontmatter.end()) description = desc_it->second;
        
        auto handler_path = find_handler_file(hook_dir);
        if (!handler_path) {
            //spdlog::warn("[hooks] Hook '{}' has HOOK.md but no handler in {}", name, hook_dir);
            return std::nullopt;
        }
        
        Hook hook;
        hook.name = name;
        hook.description = description;
        hook.source = source;
        hook.plugin_id = plugin_id;
        hook.file_path = hook_md_path.string();
        hook.base_dir = fs::canonical(hook_dir).string();
        hook.handler_path = *handler_path;
        
        return LoadedHook{hook, frontmatter};
    } catch (const std::exception& e) {
        //spdlog::warn("[hooks] Failed to load hook from {}: {}", hook_dir, e.what());
        return std::nullopt;
    }
}

// ============================================================================
// Directory Scanning (对齐 workspace.ts:153-215)
// ============================================================================

static std::vector<LoadedHook> load_hooks_from_dir(
    const std::string& dir,
    HookSource source,
    const std::optional<std::string>& plugin_id = std::nullopt
) {
    std::vector<LoadedHook> hooks;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return hooks;
    
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) continue;
        auto hook = load_hook_from_dir(
            entry.path().string(), source, plugin_id,
            entry.path().filename().string());
        if (hook) hooks.push_back(*hook);
    }
    return hooks;
}

// ============================================================================
// Public API
// ============================================================================

std::vector<HookEntry> load_hook_entries_from_dir(
    const std::string& dir,
    HookSource source,
    const std::optional<std::string>& plugin_id
) {
    auto hooks = load_hooks_from_dir(dir, source, plugin_id);
    std::vector<HookEntry> entries;
    entries.reserve(hooks.size());
    for (auto& h : hooks) {
        HookEntry entry;
        entry.hook = h.hook;
        entry.hook.source = source;
        entry.hook.plugin_id = plugin_id;
        entry.frontmatter = h.frontmatter;
        entry.metadata = resolve_openclaw_metadata(h.frontmatter);
        entry.invocation = resolve_hook_invocation_policy(h.frontmatter);
        entries.push_back(entry);
    }
    return entries;
}

std::vector<HookEntry> discover_workspace_hook_entries(
    const std::string& workspace_dir,
    const WorkspaceHookDiscoveryOptions& opts
) {
    std::vector<HookEntry> all;
    
    // 1. Bundled hooks
    std::string bundled_dir;
    if (opts.bundled_hooks_dir) {
        bundled_dir = *opts.bundled_hooks_dir;
    }
    if (!bundled_dir.empty()) {
        auto bundled = load_hook_entries_from_dir(bundled_dir, HookSource::OPENCLAW_BUNDLED);
        all.insert(all.end(), bundled.begin(), bundled.end());
    }
    
    // 2. Plugin hooks (resolved via plugin manifest)
    // In C++ we don't dynamically scan plugin manifests like OC.
    // Plugin hooks are registered via plugin_system.cc
    
    // 3. Managed hooks (~/.config/openclaw/hooks)
    std::string managed_dir;
    if (opts.managed_hooks_dir) {
        managed_dir = *opts.managed_hooks_dir;
    }
    if (!managed_dir.empty() && fs::exists(managed_dir)) {
        auto managed = load_hook_entries_from_dir(managed_dir, HookSource::OPENCLAW_MANAGED);
        all.insert(all.end(), managed.begin(), managed.end());
    }
    
    // 4. Workspace hooks (<workspace>/hooks/)
    auto workspace_hooks_dir = fs::path(workspace_dir) / "hooks";
    if (fs::exists(workspace_hooks_dir)) {
        auto workspace = load_hook_entries_from_dir(
            workspace_hooks_dir.string(), HookSource::OPENCLAW_WORKSPACE);
        all.insert(all.end(), workspace.begin(), workspace.end());
    }
    
    return all;
}

std::vector<HookEntry> load_workspace_hook_entries(
    const std::string& workspace_dir,
    const WorkspaceHookDiscoveryOptions& opts
) {
    auto entries = discover_workspace_hook_entries(workspace_dir, opts);
    return resolve_hook_entries(entries, [](const HookResolutionCollision& c) {
        // Log collision: ignoring c.ignored.hook.source hook c.name — cannot override c.kept.hook.source
        (void)c;
    });
}

HookSnapshot build_workspace_hook_snapshot(
    const std::string& workspace_dir,
    const WorkspaceHookDiscoveryOptions& opts
) {
    auto entries = load_workspace_hook_entries(workspace_dir, opts);
    HookSnapshot snapshot;
    snapshot.version = 1;
    for (const auto& entry : entries) {
        std::vector<std::string> events;
        if (entry.metadata) {
            events = entry.metadata->events;
        }
        snapshot.hooks.push_back({entry.hook.name, events});
        snapshot.resolved_hooks.push_back(entry.hook);
    }
    return snapshot;
}

}  // namespace hooks
}  // namespace hunterclaw
