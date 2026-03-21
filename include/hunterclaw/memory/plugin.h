#pragma once

// ============================================================================
// Memory Plugin Interface
//
// Alignment with OpenClaw src/plugins/types.ts:
//
//   export type PluginKind = "memory" | "context-engine";
//
//   export type OpenClawPluginDefinition = {
//     id?: string;
//     name?: string;
//     description?: string;
//     version?: string;
//     kind?: PluginKind;
//     configSchema?: OpenClawPluginConfigSchema;
//     register?: (api: OpenClawPluginApi) => void | Promise<void>;
//     activate?: (api: OpenClawPluginApi) => void | Promise<void>;
//   };
//
// In OpenClaw, a "memory" plugin is simply an OpenClawPluginDefinition with
// kind="memory". It registers a MemorySearchManager via the plugin API.
// There is NO separate MemoryPlugin base class in OpenClaw.
//
// The MemorySearchManager interface (types.ts) is the actual contract:
//   interface MemorySearchManager {
//     search(query, opts?): Promise<MemorySearchResult[]>
//     readFile(params): Promise<{text, path}>
//     status(): MemoryProviderStatus
//     sync?(params?): Promise<void>
//     probeEmbeddingAvailability(): Promise<MemoryEmbeddingProbeResult>
//     probeVectorAvailability(): Promise<boolean>
//     close?(): Promise<void>
//   }
//
// HunterClaw C++ mapping:
//   - MemorySearchManager (types.h) = MemorySearchManager interface
//   - MemoryPluginDefinition (below) = OpenClawPluginDefinition with kind="memory"
//   - Memory plugins register their MemorySearchManager via the plugin API
// ============================================================================

#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"
#include <memory>
#include <string>
#include <functional>
#include <map>

namespace hunterclaw {

// Forward declaration
namespace plugins {
struct PluginContext;
}

namespace memory {

// ============================================================================
// PluginKind (对齐 plugins/types.ts: PluginKind = "memory" | "context-engine")
// ============================================================================
enum class PluginKind {
    Memory,         // "memory"
    ContextEngine,  // "context-engine"
};

// ============================================================================
// MemoryPluginConfigSchema
// 对齐 OpenClawPluginConfigSchema (plugins/types.ts:47-57)
// ============================================================================
struct MemoryPluginConfigSchema {
    // Optional: validate config values
    std::function<bool(const std::map<std::string, std::string>&, std::vector<std::string>&)>
        validate;
    // Optional: JSON schema for UI hints
    std::map<std::string, std::string> json_schema;
};

// ============================================================================
// MemoryPluginDefinition
// 对齐 OpenClawPluginDefinition (plugins/types.ts:248-257) with kind="memory"
//
// OpenClaw:
//   export type OpenClawPluginDefinition = {
//     id?: string;
//     name?: string;
//     description?: string;
//     version?: string;
//     kind?: PluginKind;
//     configSchema?: OpenClawPluginConfigSchema;
//     register?: (api: OpenClawPluginApi) => void | Promise<void>;
//     activate?: (api: OpenClawPluginApi) => void | Promise<void>;
//   };
//
// A memory plugin provides a MemorySearchManager implementation.
// It does NOT inherit from any base class — it's a plain struct/definition.
// ============================================================================
struct MemoryPluginDefinition {
    // Plugin identity
    std::string id;
    std::string name;
    std::string description;
    std::string version;

    // kind is always "memory" for memory plugins
    PluginKind kind = PluginKind::Memory;

    // Optional config schema
    MemoryPluginConfigSchema config_schema;

    // Factory: create the MemorySearchManager for a given agent/workspace
    // Called during plugin activation (对齐 activate callback in OpenClaw)
    std::function<std::shared_ptr<MemorySearchManager>(
        const std::string& agent_id,
        const std::string& workspace_dir,
        const std::map<std::string, std::string>& plugin_config
    )> create_manager;
};

// ============================================================================
// Memory Plugin Registry
// 对齐 OpenClaw: plugins register their memory manager via the plugin API.
// In HunterClaw, memory plugins register a MemoryPluginDefinition.
// The search-manager.h (getMemorySearchManager) resolves the active manager.
// ============================================================================
class MemoryPluginRegistry {
public:
    static MemoryPluginRegistry& instance();

    // Register a memory plugin definition
    void register_plugin(const MemoryPluginDefinition& def);

    // Get a registered plugin by ID
    const MemoryPluginDefinition* get_plugin(const std::string& id) const;

    // List all registered plugin IDs
    std::vector<std::string> list_plugin_ids() const;

    // Check if a plugin is registered
    bool has_plugin(const std::string& id) const;

private:
    MemoryPluginRegistry() = default;
    std::map<std::string, MemoryPluginDefinition> plugins_;
};

} // namespace memory
} // namespace hunterclaw
