#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/core/utils.h"
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <dlfcn.h>

namespace hunterclaw {
namespace plugins {

// Forward declaration
core::Result<std::shared_ptr<Plugin>> load_plugin_impl(const std::string& path);

// ============================================================================
// PluginManager::Impl
// ============================================================================

struct PluginManager::Impl {
    std::map<std::string, std::shared_ptr<Plugin>> plugins_;
    std::map<std::string, std::string> plugin_paths_;
    std::mutex mutex_;
    // ✅ 对齐 OpenClaw: 文件监控热重载
    std::atomic<bool> watching_{false};
    std::thread watch_thread_;
    std::map<std::string, std::filesystem::file_time_type> file_mtimes_;
    
    core::Result<bool> load(const std::string& plugin_id, const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Check if already loaded
        if (plugins_.find(plugin_id) != plugins_.end()) {
            return core::Result<bool>::Err("Plugin already loaded: " + plugin_id);
        }
        
        // Load the plugin
        auto plugin_result = load_plugin_impl(path);
        if (plugin_result.is_err()) {
            return core::Result<bool>::Err(plugin_result.error);
        }
        
        auto plugin = plugin_result.get();
        
        // Call on_load
        auto load_result = plugin->on_load();
        if (load_result.is_err()) {
            return core::Result<bool>::Err("Plugin on_load failed: " + load_result.error);
        }
        
        // Store plugin
        plugins_[plugin_id] = plugin;
        plugin_paths_[plugin_id] = path;
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> unload(const std::string& plugin_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = plugins_.find(plugin_id);
        if (it == plugins_.end()) {
            return core::Result<bool>::Err("Plugin not found: " + plugin_id);
        }
        
        // Call on_unload
        auto result = it->second->on_unload();
        if (result.is_err()) {
            return core::Result<bool>::Err("Plugin on_unload failed: " + result.error);
        }
        
        // Remove from maps
        plugins_.erase(it);
        plugin_paths_.erase(plugin_id);
        
        return core::Result<bool>::Ok(true);
    }
    
    std::vector<std::string> list() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        
        std::vector<std::string> result;
        for (const auto& [id, _] : plugins_) {
            result.push_back(id);
        }
        return result;
    }
    
    std::shared_ptr<Plugin> get(const std::string& plugin_id) const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        
        auto it = plugins_.find(plugin_id);
        if (it != plugins_.end()) {
            return it->second;
        }
        return nullptr;
    }
};

// ============================================================================
// PluginManager Implementation
// ============================================================================

PluginManager::PluginManager(std::shared_ptr<core::Config> config)
    : impl_(std::make_unique<Impl>()) {
    (void)config;
}

PluginManager::~PluginManager() = default;

std::shared_ptr<PluginManager> PluginManager::create() {
    return std::make_shared<PluginManager>(nullptr);
}

core::Result<bool> PluginManager::load(const std::string& plugin_id, const std::string& path) {
    return impl_->load(plugin_id, path);
}

core::Result<bool> PluginManager::unload(const std::string& plugin_id) {
    return impl_->unload(plugin_id);
}

core::Result<bool> PluginManager::reload(const std::string& plugin_id) {
    auto unload_result = impl_->unload(plugin_id);
    if (unload_result.is_err()) {
        return unload_result;
    }
    
    auto path_it = impl_->plugin_paths_.find(plugin_id);
    if (path_it == impl_->plugin_paths_.end()) {
        return core::Result<bool>::Err("Plugin path not found");
    }
    
    return impl_->load(plugin_id, path_it->second);
}

std::vector<std::string> PluginManager::list() const {
    return impl_->list();
}

std::shared_ptr<Plugin> PluginManager::get(const std::string& plugin_id) const {
    return impl_->get(plugin_id);
}

void PluginManager::start_watch() {
    // ✅ 对齐 OpenClaw: 文件监控热重载 (chokidar.watch)
    if (impl_->watching_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        for (const auto& [id, path] : impl_->plugin_paths_) {
            try { impl_->file_mtimes_[path] = std::filesystem::last_write_time(path); } catch (...) {}
        }
    }
    impl_->watch_thread_ = std::thread([this]() {
        while (impl_->watching_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!impl_->watching_.load()) break;
            std::vector<std::pair<std::string, std::string>> changed;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex_);
                for (const auto& [id, path] : impl_->plugin_paths_) {
                    try {
                        auto mtime = std::filesystem::last_write_time(path);
                        auto it = impl_->file_mtimes_.find(path);
                        if (it == impl_->file_mtimes_.end() || it->second != mtime) {
                            impl_->file_mtimes_[path] = mtime;
                            changed.push_back({id, path});
                        }
                    } catch (...) {}
                }
            }
            for (const auto& [id, path] : changed) {
                impl_->unload(id);
                impl_->load(id, path);
            }
        }
    });
}

void PluginManager::stop_watch() {
    // ✅ 对齐 OpenClaw: 停止文件监控
    impl_->watching_.store(false);
    if (impl_->watch_thread_.joinable()) {
        impl_->watch_thread_.join();
    }
}

// ============================================================================
// Plugin Loading Implementation
// ============================================================================

core::Result<std::shared_ptr<Plugin>> load_plugin_impl(const std::string& path) {
    // Open shared library
    void* handle = dlopen(path.c_str(), RTLD_LAZY);
    if (!handle) {
        return core::Result<std::shared_ptr<Plugin>>::Err(
            std::string("Failed to load plugin: ") + dlerror()
        );
    }
    
    // Get create_plugin function
    using CreatePluginFunc = Plugin* (*)();
    auto create_plugin = reinterpret_cast<CreatePluginFunc>(
        dlsym(handle, "create_plugin")
    );
    
    if (!create_plugin) {
        dlclose(handle);
        return core::Result<std::shared_ptr<Plugin>>::Err(
            "Plugin does not export create_plugin function"
        );
    }
    
    // Create plugin instance
    Plugin* plugin = create_plugin();
    if (!plugin) {
        dlclose(handle);
        return core::Result<std::shared_ptr<Plugin>>::Err(
            "create_plugin returned null"
        );
    }
    
    return core::Result<std::shared_ptr<Plugin>>::Ok(
        std::shared_ptr<Plugin>(plugin, [handle](Plugin* p) {
            delete p;
            dlclose(handle);
        })
    );
}

} // namespace plugins
} // namespace hunterclaw
