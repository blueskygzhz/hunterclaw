#pragma once

// ============================================================================
// Memory Plugin Manager
//
// OpenClaw alignment:
//   In OpenClaw, plugins are loaded via the plugin system (plugins/runtime/).
//   There is NO dedicated MemoryPluginManager class in OpenClaw.
//
//   Memory plugins are discovered and loaded by the general plugin loader,
//   which calls plugin.register(api) / plugin.activate(api).
//   The plugin then registers its MemorySearchManager via the plugin API.
//
//   HunterClaw mapping:
//   - MemoryPluginRegistry (plugin.h) holds registered MemoryPluginDefinitions
//   - MemoryPluginLoader (below) handles .so discovery and loading
//   - search_manager.h resolves the active MemorySearchManager
// ============================================================================

#include "hunterclaw/memory/plugin.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace hunterclaw {
namespace memory {

// ============================================================================
// PluginManifest
// 对齐 OpenClaw plugin manifest (.plugin.json)
//
// OpenClaw plugin manifest fields:
//   id, name, version, kind ("memory"), description, main (.so), config
// ============================================================================
struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string kind;         // "memory" or "context-engine"
    std::string description;
    std::string main;         // .so filename
    std::map<std::string, std::string> config;  // default config values

    // Optional metadata
    std::string author;
    std::string license;
    std::string homepage;
    std::vector<std::string> keywords;
};

// ============================================================================
// MemoryPluginLoader
//
// Handles discovery and loading of memory plugin .so files.
// Loaded plugins register themselves into MemoryPluginRegistry.
//
// This is the C++ equivalent of OpenClaw's plugin loader for memory plugins.
// ============================================================================
class MemoryPluginLoader {
public:
    static MemoryPluginLoader& instance();

    // Disable copy/move
    MemoryPluginLoader(const MemoryPluginLoader&) = delete;
    MemoryPluginLoader& operator=(const MemoryPluginLoader&) = delete;

    // ========================================================================
    // Plugin directory management
    // ========================================================================

    void add_plugins_directory(const std::string& dir);
    void set_plugins_directories(const std::vector<std::string>& dirs);
    std::vector<std::string> get_plugins_directories() const;

    // ========================================================================
    // Plugin discovery and loading
    // ========================================================================

    // Discover all plugin manifests in configured directories
    core::Result<std::vector<PluginManifest>> discover_plugins();

    // Load a plugin from a manifest file path
    core::Result<void> load_plugin(const std::string& manifest_path);

    // Load all discovered plugins
    core::Result<int> load_all_plugins();

    // Unload a plugin by ID (dlclose + remove from registry)
    core::Result<void> unload_plugin(const std::string& plugin_id);

    // ========================================================================
    // Manifest parsing
    // ========================================================================

    static core::Result<PluginManifest> parse_manifest(const std::string& manifest_path);
    static core::Result<void> validate_manifest(const PluginManifest& manifest);

private:
    MemoryPluginLoader() = default;
    ~MemoryPluginLoader();

    std::vector<std::string> plugin_dirs_;
    std::map<std::string, void*> handles_;  // plugin_id -> dlopen handle
    mutable std::mutex mutex_;
};

} // namespace memory
} // namespace hunterclaw
