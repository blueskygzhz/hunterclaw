#include "hunterclaw/hooks/hook_loader.h"
#include "hunterclaw/hooks/hook_workspace.h"
#include "hunterclaw/hooks/hook_policy.h"
#include <spdlog/spdlog.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace hooks {

// ============================================================================
// HookLoader Implementation (对齐 loader.ts:62-240)
// ============================================================================

int HookLoader::load_internal_hooks(
    const core::Config& cfg,
    const std::string& workspace_dir,
    const HookLoaderOptions& opts
) {
    // Check if hooks are globally disabled
    auto cfg_json = cfg.data;
    if (cfg_json.contains("hooks") && cfg_json["hooks"].contains("internal")) {
        auto& internal = cfg_json["hooks"]["internal"];
        if (internal.contains("enabled") && internal["enabled"] == false) {
            return 0;
        }
    }
    
    int loaded_count = 0;
    
    // 1. Register bundled hooks (always available)
    loaded_count += register_bundled_hooks();
    
    // 2. Load hooks from workspace directories
    try {
        WorkspaceHookDiscoveryOptions disco_opts;
        disco_opts.config = cfg;
        if (!opts.managed_hooks_dir.empty()) {
            disco_opts.managed_hooks_dir = opts.managed_hooks_dir;
        }
        if (!opts.bundled_hooks_dir.empty()) {
            disco_opts.bundled_hooks_dir = opts.bundled_hooks_dir;
        }
        
        auto entries = load_workspace_hook_entries(workspace_dir, disco_opts);
        
        for (const auto& entry : entries) {
            // Check eligibility
            if (!should_include_hook(entry, cfg)) {
                continue;
            }
            
            // Register for all events in metadata
            if (!entry.metadata || entry.metadata->events.empty()) {
                //spdlog::warn("[hooks:loader] Hook '{}' has no events defined", entry.hook.name);
                continue;
            }
            
            // In C++, hooks are compiled-in, not dynamically loaded.
            // The handler_path points to the hook source for reference.
            // Actual handler registration happens via bundled hooks or plugin system.
            // spdlog::debug discovered hook: entry.hook.name -> events[0]
            
            if (std::find(loaded_hook_names_.begin(), loaded_hook_names_.end(), 
                          entry.hook.name) != loaded_hook_names_.end()) {
                continue; // dedup
            }
            loaded_hook_names_.push_back(entry.hook.name);
            loaded_count++;
        }
    } catch (const std::exception& e) {
        //spdlog::error("[hooks:loader] Failed to load directory-based hooks: {}", e.what());
    }
    
    return loaded_count;
}

int HookLoader::register_bundled_hooks() {
    int count = 0;
    auto defs = get_bundled_hook_definitions();
    for (const auto& def : defs) {
        for (const auto& event_key : def.events) {
            HookRegistry::instance().register_hook(event_key, def.handler);
        }
        loaded_hook_names_.push_back(def.name);
        count++;
    }
    //spdlog::debug("[hooks:loader] Registered {} bundled hooks", count);
    return count;
}

std::vector<DiscoveredHook> HookLoader::discover_hooks_in_dir(
    const std::string& dir,
    HookSource source
) {
    auto entries = load_hook_entries_from_dir(dir, source);
    std::vector<DiscoveredHook> result;
    result.reserve(entries.size());
    for (auto& entry : entries) {
        DiscoveredHook dh;
        dh.entry = entry;
        dh.handler_path = entry.hook.handler_path;
        dh.base_dir = entry.hook.base_dir;
        result.push_back(dh);
    }
    return result;
}

bool HookLoader::should_include_hook(const HookEntry& entry, const core::Config& cfg) {
    auto enable_state = resolve_hook_enable_state(entry, cfg);
    return enable_state.enabled;
}

std::optional<OpenClawHookMetadata> HookLoader::parse_hook_md(const std::string& hook_md_path) {
    try {
        std::ifstream ifs(hook_md_path);
        if (!ifs.is_open()) return std::nullopt;
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        auto frontmatter = parse_hook_frontmatter(content);
        return resolve_openclaw_metadata(frontmatter);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> HookLoader::resolve_handler_path(const std::string& hook_dir) {
    static const std::vector<std::string> candidates = {
        "handler.cc", "handler.cpp", "handler.ts", "handler.js",
        "index.cc", "index.cpp", "index.ts", "index.js"
    };
    for (const auto& candidate : candidates) {
        auto path = fs::path(hook_dir) / candidate;
        if (fs::exists(path)) return path.string();
    }
    return std::nullopt;
}

bool HookLoader::verify_handler_boundary(const std::string& handler_path, const std::string& base_dir) {
    try {
        auto canonical_handler = fs::canonical(handler_path);
        auto canonical_base = fs::canonical(base_dir);
        // Verify handler is within base directory
        auto rel = fs::relative(canonical_handler, canonical_base);
        auto rel_str = rel.string();
        return !rel_str.empty() && rel_str.substr(0, 2) != "..";
    } catch (...) {
        return false;
    }
}

}  // namespace hooks
}  // namespace hunterclaw
