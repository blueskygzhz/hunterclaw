#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/core/utils.h"
#include <dlfcn.h>
#include <stdexcept>

namespace hunterclaw {
namespace plugins {

// ============================================================================
// HunterClawPlugin Definition (C plugin handle)
// ============================================================================

// Define the plugin structure
struct HunterClawPlugin {
    void* user_data;
    int (*on_load)(HunterClawPlugin* plugin);
    int (*on_unload)(HunterClawPlugin* plugin);
};

// ============================================================================
// Native Plugin Loader
// ============================================================================

class NativePluginLoader {
public:
    core::Result<std::shared_ptr<Plugin>> load(const std::string& path) {
        // Open dynamic library
        void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            return core::Result<std::shared_ptr<Plugin>>::Err(
                std::string("Failed to load plugin: ") + dlerror());
        }
        
        // Get manifest function
        using GetManifestFn = const char* (*)();
        auto get_manifest = reinterpret_cast<GetManifestFn>(
            dlsym(handle, "hunterclaw_plugin_get_manifest"));
        
        if (!get_manifest) {
            dlclose(handle);
            return core::Result<std::shared_ptr<Plugin>>::Err(
                "Plugin missing hunterclaw_plugin_get_manifest symbol");
        }
        
        // Get init function
        using InitFn = HunterClawPlugin* (*)(const char*);
        auto init_fn = reinterpret_cast<InitFn>(
            dlsym(handle, "hunterclaw_plugin_init"));
        
        if (!init_fn) {
            dlclose(handle);
            return core::Result<std::shared_ptr<Plugin>>::Err(
                "Plugin missing hunterclaw_plugin_init symbol");
        }
        
        // Get shutdown function
        using ShutdownFn = void (*)(HunterClawPlugin*);
        auto shutdown_fn = reinterpret_cast<ShutdownFn>(
            dlsym(handle, "hunterclaw_plugin_shutdown"));
        
        if (!shutdown_fn) {
            dlclose(handle);
            return core::Result<std::shared_ptr<Plugin>>::Err(
                "Plugin missing hunterclaw_plugin_shutdown symbol");
        }
        
        // Initialize plugin
        HunterClawPlugin* plugin_ptr = init_fn("{}");
        if (!plugin_ptr) {
            dlclose(handle);
            return core::Result<std::shared_ptr<Plugin>>::Err(
                "Plugin initialization failed");
        }
        
        // Create wrapper
        auto plugin = std::make_shared<NativePlugin>(
            handle, plugin_ptr, shutdown_fn, "native-plugin");
        
        return core::Result<std::shared_ptr<Plugin>>::Ok(plugin);
    }

private:
    class NativePlugin : public Plugin {
    public:
        NativePlugin(void* handle, HunterClawPlugin* plugin, void (*shutdown)(HunterClawPlugin*), const std::string& id)
            : handle_(handle), plugin_(plugin), shutdown_fn_(shutdown), id_(id) {}
        
        ~NativePlugin() override {
            if (plugin_ && shutdown_fn_) {
                shutdown_fn_(plugin_);
            }
            if (handle_) {
                dlclose(handle_);
            }
        }
        
        // Required lifecycle methods
        core::Result<bool> init(const PluginContext& ctx) override {
            // Native plugins are initialized via C API
            return core::Result<bool>::Ok(true);
        }
        
        core::Result<bool> shutdown() override {
            if (plugin_ && shutdown_fn_) {
                shutdown_fn_(plugin_);
                plugin_ = nullptr;
            }
            return core::Result<bool>::Ok(true);
        }
        
        core::Result<bool> on_load() override {
            if (plugin_ && plugin_->on_load) {
                int result = plugin_->on_load(plugin_);
                if (result == 0) {
                    return core::Result<bool>::Ok(true);
                }
                return core::Result<bool>::Err("Plugin on_load returned error");
            }
            return core::Result<bool>::Ok(true);
        }
        
        core::Result<bool> on_unload() override {
            if (plugin_ && plugin_->on_unload) {
                int result = plugin_->on_unload(plugin_);
                if (result == 0) {
                    return core::Result<bool>::Ok(true);
                }
                return core::Result<bool>::Err("Plugin on_unload returned error");
            }
            return core::Result<bool>::Ok(true);
        }
        
        // Metadata methods
        std::string id() const override { return id_; }
        std::string name() const override { return id_; }
        std::string version() const override { return "1.0.0"; }
        
    private:
        void* handle_;
        HunterClawPlugin* plugin_;
        void (*shutdown_fn_)(HunterClawPlugin*);
        std::string id_;
    };
};

core::Result<std::shared_ptr<Plugin>> load_plugin(const std::string& path) {
    NativePluginLoader loader;
    return loader.load(path);
}

} // namespace plugins
} // namespace hunterclaw
