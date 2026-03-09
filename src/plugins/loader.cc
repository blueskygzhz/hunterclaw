#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/core/utils.h"
#include <dlfcn.h>
#include <stdexcept>

namespace hunterclaw {
namespace plugins {

// Plugin loader for native (.so/.dll) plugins
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
        
        // Parse manifest
        const char* manifest_json = get_manifest();
        // TODO: Parse JSON manifest
        
        // Initialize plugin
        HunterClawPlugin* plugin_ptr = init_fn("{}");
        if (!plugin_ptr) {
            dlclose(handle);
            return core::Result<std::shared_ptr<Plugin>>::Err(
                "Plugin initialization failed");
        }
        
        // Create wrapper
        auto plugin = std::make_shared<NativePlugin>(
            handle, plugin_ptr, shutdown_fn);
        
        return core::Result<std::shared_ptr<Plugin>>::Ok(plugin);
    }

private:
    class NativePlugin : public Plugin {
    public:
        NativePlugin(void* handle, HunterClawPlugin* plugin, void (*shutdown)(HunterClawPlugin*))
            : handle_(handle), plugin_(plugin), shutdown_fn_(shutdown) {}
        
        ~NativePlugin() override {
            if (plugin_ && shutdown_fn_) {
                shutdown_fn_(plugin_);
            }
            if (handle_) {
                dlclose(handle_);
            }
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
        
        core::PluginManifest get_manifest() const override {
            // TODO: Parse from plugin
            return core::PluginManifest{};
        }
        
    private:
        void* handle_;
        HunterClawPlugin* plugin_;
        void (*shutdown_fn_)(HunterClawPlugin*);
    };
};

core::Result<std::shared_ptr<Plugin>> load_plugin(const std::string& path) {
    NativePluginLoader loader;
    return loader.load(path);
}

} // namespace plugins
} // namespace hunterclaw
