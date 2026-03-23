#pragma once

#include "hunterclaw/core/types.h"
#include <dlfcn.h>
#include <memory>
#include <functional>

namespace hunterclaw {
namespace plugins {

// ============================================================================
// Plugin Interface (ABI-stable C API)
// ============================================================================

extern "C" {

// Plugin lifecycle
struct HunterClawPlugin;

typedef HunterClawPlugin* (*PluginInitFunc)(const char* config_json);
typedef void (*PluginShutdownFunc)(HunterClawPlugin* plugin);
typedef const char* (*PluginGetManifestFunc)();

// Plugin version (for ABI compatibility check)
constexpr uint32_t HUNTERCLAW_PLUGIN_API_VERSION = 1;

struct PluginAPI {
    uint32_t version;
    PluginInitFunc init;
    PluginShutdownFunc shutdown;
    PluginGetManifestFunc get_manifest;
};

} // extern "C"

// ============================================================================
// Plugin Context (passed to plugins)
// ============================================================================

struct PluginContext {
    std::shared_ptr<core::Config> config;
    std::string workspace_dir;
    std::string plugin_dir;
    
    // Callbacks for plugin to interact with core
    std::function<void(const std::string& log)> log_info;
    std::function<void(const std::string& log)> log_error;
    std::function<void(const std::string& event, const core::JsonValue& data)> emit_event;
};

// ============================================================================
// Plugin Base Class (C++ interface)
// ============================================================================

class Plugin {
public:
    virtual ~Plugin() = default;
    
    // Lifecycle
    virtual core::Result<bool> init(const PluginContext& ctx) = 0;
    virtual core::Result<bool> shutdown() = 0;
    virtual core::Result<bool> on_load() = 0;
    virtual core::Result<bool> on_unload() = 0;
    
    // Metadata
    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    
    // Capabilities
    virtual bool supports_hot_reload() const { return false; }
    virtual core::Result<bool> reload() { 
        return core::Result<bool>::Err("Hot reload not supported"); 
    }
};

// ============================================================================
// Plugin Manager
// ============================================================================

class PluginManager {
public:
    // Factory method
    static std::shared_ptr<PluginManager> create();
    
    explicit PluginManager(std::shared_ptr<core::Config> config);
    ~PluginManager();
    
    // Plugin lifecycle
    core::Result<bool> load(const std::string& plugin_id, const std::string& path);
    core::Result<bool> unload(const std::string& plugin_id);
    core::Result<bool> reload(const std::string& plugin_id);
    
    // Plugin discovery
    std::vector<std::string> list() const;
    std::shared_ptr<Plugin> get(const std::string& plugin_id) const;
    
    // Hot reload
    void start_watch();
    void stop_watch();
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Plugin Remote Registry (for discovering and installing plugins from remote)
// ============================================================================

struct PluginRegistryEntry {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string download_url;
    std::vector<std::string> tags;
    size_t download_count;
    double rating;
};

class PluginRemoteRegistry {
public:
    explicit PluginRemoteRegistry(const std::string& registry_url);
    
    // Search and discovery
    core::Result<std::vector<PluginRegistryEntry>> search(const std::string& query);
    core::Result<PluginRegistryEntry> get_plugin_info(const std::string& plugin_id);
    
    // Download and install
    core::Result<std::string> download_plugin(const std::string& plugin_id, const std::string& target_dir);
    
    // Publish (for plugin developers)
    core::Result<bool> publish_plugin(const std::string& plugin_path, const std::string& api_key);
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Helper Macros for Plugin Development
// ============================================================================

// Declare plugin exports
#define HUNTERCLAW_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))

// Plugin registration macro
#define HUNTERCLAW_REGISTER_PLUGIN(PluginClass) \
    static PluginClass* g_plugin_instance = nullptr; \
    \
    HUNTERCLAW_PLUGIN_EXPORT \
    HunterClawPlugin* hunterclaw_plugin_init(const char* config_json) { \
        g_plugin_instance = new PluginClass(); \
        hunterclaw::plugins::PluginContext ctx; \
        /* Parse config_json and populate ctx */ \
        auto result = g_plugin_instance->init(ctx); \
        if (!result.is_ok()) { \
            delete g_plugin_instance; \
            g_plugin_instance = nullptr; \
            return nullptr; \
        } \
        return reinterpret_cast<HunterClawPlugin*>(g_plugin_instance); \
    } \
    \
    HUNTERCLAW_PLUGIN_EXPORT \
    void hunterclaw_plugin_shutdown(HunterClawPlugin* plugin) { \
        if (g_plugin_instance) { \
            g_plugin_instance->shutdown(); \
            delete g_plugin_instance; \
            g_plugin_instance = nullptr; \
        } \
    } \
    \
    HUNTERCLAW_PLUGIN_EXPORT \
    const char* hunterclaw_plugin_get_manifest() { \
        static std::string manifest_json; \
        if (manifest_json.empty() && g_plugin_instance) { \
            /* \u2705 \u5bf9\u9f50 OpenClaw: \u5e8f\u5217\u5316 manifest \u5230 JSON */ \
            manifest_json = std::string("{") + \
                "\"id\":\"" + g_plugin_instance->id() + "\"," + \
                "\"name\":\"" + g_plugin_instance->name() + "\"," + \
                "\"version\":\"" + g_plugin_instance->version() + "\"" + \
                "}"; \
        } \
        return manifest_json.c_str(); \
    }

} // namespace plugins
} // namespace hunterclaw
