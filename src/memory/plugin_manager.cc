#include "hunterclaw/memory/plugin_manager.h"
#include "hunterclaw/memory/provider_registry.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <dlfcn.h>
#include <json/json.h>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

namespace {
    auto log() {
        auto logger = spdlog::get("memory");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("memory");
        }
        return logger;
    }
}

// ========================================
// 单例模式
// ========================================

MemoryPluginManager& MemoryPluginManager::instance() {
    static MemoryPluginManager instance;
    return instance;
}

// ========================================
// 设置插件目录
// ========================================

void MemoryPluginManager::set_plugins_directory(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    plugin_directories_.clear();
    plugin_directories_.push_back(dir);
    log()->info("MemoryPluginManager: set plugins directory to {}", dir);
}

void MemoryPluginManager::add_plugins_directory(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    plugin_directories_.push_back(dir);
    log()->info("MemoryPluginManager: added plugins directory {}", dir);
}

// ========================================
// 发现插件
// ========================================

core::Result<std::vector<std::string>> MemoryPluginManager::discover_plugins() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    discovered_manifests_.clear();
    std::vector<std::string> plugin_ids;
    
    // 默认插件目录 (如果未设置)
    if (plugin_directories_.empty()) {
        // HunterClaw 默认路径
        const char* home = std::getenv("HOME");
        if (home) {
            plugin_directories_.push_back(std::string(home) + "/.hunterclaw/plugins");
        }
        plugin_directories_.push_back("/usr/lib/hunterclaw/plugins");
        plugin_directories_.push_back("/usr/local/lib/hunterclaw/plugins");
    }
    
    // 扫描所有插件目录
    for (const auto& dir : plugin_directories_) {
        if (!fs::exists(dir)) {
            log()->debug("MemoryPluginManager: directory {} does not exist, skipping", dir);
            continue;
        }
        
        log()->info("MemoryPluginManager: scanning directory {} for plugins", dir);
        
        // 查找所有 .plugin.json 文件
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            
            std::string filename = entry.path().filename().string();
            std::string suffix = ".plugin.json";
            if (filename.size() < suffix.size() || 
                filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            
            // 解析 manifest
            auto manifest_result = parse_manifest(entry.path().string());
            if (manifest_result.is_err()) {
                log()->warn("MemoryPluginManager: failed to parse manifest {}: {}",
                    entry.path().string(), manifest_result.error);
                continue;
            }
            
            auto manifest = manifest_result.get();
            
            // 验证 manifest
            auto validate_result = validate_manifest(manifest);
            if (validate_result.is_err()) {
                log()->warn("MemoryPluginManager: invalid manifest {}: {}",
                    entry.path().string(), validate_result.error);
                continue;
            }
            
            // 检查 type 是否为 "memory"
            if (manifest.type != "memory") {
                log()->debug("MemoryPluginManager: skipping non-memory plugin {} (type: {})",
                    manifest.id, manifest.type);
                continue;
            }
            
            // 保存 manifest
            discovered_manifests_[manifest.id] = manifest;
            plugin_ids.push_back(manifest.id);
            
            log()->info("MemoryPluginManager: discovered plugin {} ({}) at {}",
                manifest.id, manifest.name, entry.path().string());
        }
    }
    
    log()->info("MemoryPluginManager: discovered {} memory plugin(s)", plugin_ids.size());
    return core::Result<std::vector<std::string>>::Ok(plugin_ids);
}

// ========================================
// 加载插件 (from manifest)
// ========================================

core::Result<std::shared_ptr<MemoryPlugin>> MemoryPluginManager::load_plugin(
    const std::string& manifest_path
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 1. 解析 manifest
    auto manifest_result = parse_manifest(manifest_path);
    if (manifest_result.is_err()) {
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err(manifest_result.error);
    }
    
    auto manifest = manifest_result.get();
    
    // 2. 验证 manifest
    auto validate_result = validate_manifest(manifest);
    if (validate_result.is_err()) {
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err(validate_result.error);
    }
    
    // 3. 检查是否已加载
    if (plugins_.find(manifest.id) != plugins_.end()) {
        log()->warn("MemoryPluginManager: plugin {} is already loaded", manifest.id);
        return core::Result<std::shared_ptr<MemoryPlugin>>::Ok(plugins_[manifest.id]);
    }
    
    // 4. 找到 .so 文件
    fs::path manifest_dir = fs::path(manifest_path).parent_path();
    fs::path so_path = manifest_dir / manifest.main;
    
    if (!fs::exists(so_path)) {
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Plugin library not found: " + so_path.string());
    }
    
    // 5. dlopen 加载 .so
    void* handle = dlopen(so_path.c_str(), RTLD_LAZY);
    if (!handle) {
        std::string error = dlerror();
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Failed to load plugin library: " + error);
    }
    
    // 6. 查找 create_plugin 函数
    dlerror();  // 清空错误
    CreatePluginFn create_fn = (CreatePluginFn)dlsym(handle, "create_plugin");
    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        dlclose(handle);
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Failed to find create_plugin: " + std::string(dlsym_error));
    }
    
    // 7. 创建插件实例
    MemoryPlugin* raw_plugin = create_fn();
    if (!raw_plugin) {
        dlclose(handle);
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err("create_plugin returned null");
    }
    
    auto plugin = std::shared_ptr<MemoryPlugin>(raw_plugin);
    
    // 8. 初始化插件
    // TODO: 目前 MemoryPlugin 的 init 函数需要适配标准的 plugins::PluginContext
    // 暂时使用简化方案，直接创建一个包含 config map 的 context
    plugins::PluginContext ctx;
    ctx.plugin_dir = manifest_dir.string();
    // config 暂时留空，实际的配置通过 manifest.config 传递给 provider
    
    auto init_result = plugin->init(ctx);
    if (init_result.is_err()) {
        dlclose(handle);
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Failed to initialize plugin: " + init_result.error);
    }
    
    // 9. 获取 Provider 并注册
    auto provider = plugin->get_provider();
    if (!provider) {
        dlclose(handle);
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Plugin returned null provider");
    }
    
    auto& registry = MemoryProviderRegistry::instance();
    auto register_result = registry.register_provider(manifest.id, provider);
    if (register_result.is_err()) {
        dlclose(handle);
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Failed to register provider: " + register_result.error);
    }
    
    // 10. 保存插件和句柄
    plugins_[manifest.id] = plugin;
    plugin_handles_[manifest.id] = handle;
    
    log()->info("MemoryPluginManager: loaded plugin {} ({}) from {}",
        manifest.id, manifest.name, so_path.string());
    
    return core::Result<std::shared_ptr<MemoryPlugin>>::Ok(plugin);
}

// ========================================
// 加载插件 (by ID)
// ========================================

core::Result<std::shared_ptr<MemoryPlugin>> MemoryPluginManager::load_plugin_by_id(
    const std::string& plugin_id
) {
    // 检查是否已发现
    if (discovered_manifests_.find(plugin_id) == discovered_manifests_.end()) {
        return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Plugin not found in discovered manifests: " + plugin_id);
    }
    
    // 找到 manifest 文件路径
    // (需要从 discovered_manifests_ 中恢复路径)
    // 简化实现: 重新扫描目录查找 manifest
    for (const auto& dir : plugin_directories_) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            std::string filename = entry.path().filename().string();
            if (filename.find(".plugin.json") == std::string::npos) continue;
            
            auto manifest_result = parse_manifest(entry.path().string());
            if (manifest_result.is_err()) continue;
            
            if (manifest_result.get().id == plugin_id) {
                return load_plugin(entry.path().string());
            }
        }
    }
    
    return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Plugin manifest not found: " + plugin_id);
}

// ========================================
// 卸载插件
// ========================================

core::Result<bool> MemoryPluginManager::unload_plugin(const std::string& plugin_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 1. 检查插件是否已加载
    if (plugins_.find(plugin_id) == plugins_.end()) {
        return core::Result<bool>::Err("Plugin not loaded: " + plugin_id);
    }
    
    auto plugin = plugins_[plugin_id];
    
    // 2. 调用 shutdown
    auto shutdown_result = plugin->shutdown();
    if (shutdown_result.is_err()) {
        log()->warn("MemoryPluginManager: plugin {} shutdown failed: {}",
            plugin_id, shutdown_result.error);
    }
    
    // 3. 从 Provider Registry 移除
    auto& registry = MemoryProviderRegistry::instance();
    registry.unregister_provider(plugin_id);
    
    // 4. dlclose 卸载 .so
    if (plugin_handles_.find(plugin_id) != plugin_handles_.end()) {
        void* handle = plugin_handles_[plugin_id];
        dlclose(handle);
        plugin_handles_.erase(plugin_id);
    }
    
    // 5. 移除插件
    plugins_.erase(plugin_id);
    
    log()->info("MemoryPluginManager: unloaded plugin {}", plugin_id);
    return core::Result<bool>::Ok(true);
}

// ========================================
// 重载插件
// ========================================

core::Result<bool> MemoryPluginManager::reload_plugin(const std::string& plugin_id) {
    // 1. 卸载插件
    auto unload_result = unload_plugin(plugin_id);
    if (unload_result.is_err()) {
        return core::Result<bool>::Err("Failed to unload plugin: " + unload_result.error);
    }
    
    // 2. 重新加载插件
    auto load_result = load_plugin_by_id(plugin_id);
    if (load_result.is_err()) {
        return core::Result<bool>::Err("Failed to reload plugin: " + load_result.error);
    }
    
    log()->info("MemoryPluginManager: reloaded plugin {}", plugin_id);
    return core::Result<bool>::Ok(true);
}

// ========================================
// 发现并加载所有插件
// ========================================

core::Result<int> MemoryPluginManager::discover_and_load_all() {
    // 1. 发现插件
    auto discover_result = discover_plugins();
    if (discover_result.is_err()) {
        return core::Result<int>::Err(discover_result.error);
    }
    
    auto plugin_ids = discover_result.get();
    
    // 2. 加载所有插件
    int loaded_count = 0;
    for (const auto& plugin_id : plugin_ids) {
        auto load_result = load_plugin_by_id(plugin_id);
        if (load_result.is_ok()) {
            loaded_count++;
        } else {
            log()->warn("MemoryPluginManager: failed to load plugin {}: {}",
                plugin_id, load_result.error);
        }
    }
    
    log()->info("MemoryPluginManager: loaded {}/{} plugin(s)", loaded_count, plugin_ids.size());
    return core::Result<int>::Ok(loaded_count);
}

// ========================================
// 查询和管理
// ========================================

std::shared_ptr<MemoryPlugin> MemoryPluginManager::get_plugin(const std::string& plugin_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = plugins_.find(plugin_id);
    if (it != plugins_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<MemoryPlugin>> MemoryPluginManager::list_plugins() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<MemoryPlugin>> result;
    for (const auto& [id, plugin] : plugins_) {
        result.push_back(plugin);
    }
    return result;
}

bool MemoryPluginManager::is_loaded(const std::string& plugin_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return plugins_.find(plugin_id) != plugins_.end();
}

size_t MemoryPluginManager::plugin_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return plugins_.size();
}

core::Result<bool> MemoryPluginManager::unload_all() {
    std::vector<std::string> plugin_ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, _] : plugins_) {
            plugin_ids.push_back(id);
        }
    }
    
    for (const auto& id : plugin_ids) {
        auto result = unload_plugin(id);
        if (result.is_err()) {
            log()->warn("MemoryPluginManager: failed to unload plugin {}: {}", id, result.error);
        }
    }
    
    return core::Result<bool>::Ok(true);
}

// ========================================
// Manifest 解析
// ========================================

core::Result<MemoryPluginManager::PluginManifest> MemoryPluginManager::parse_manifest(
    const std::string& manifest_path
) {
    std::ifstream file(manifest_path);
    if (!file.is_open()) {
        return core::Result<PluginManifest>::Err("Failed to open manifest file: " + manifest_path);
    }
    
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    
    if (!Json::parseFromStream(builder, file, &root, &errs)) {
        return core::Result<PluginManifest>::Err("Failed to parse manifest JSON: " + errs);
    }
    
    PluginManifest manifest;
    
    // 必填字段
    if (!root.isMember("id") || !root["id"].isString()) {
        return core::Result<PluginManifest>::Err("Missing or invalid field: id");
    }
    manifest.id = root["id"].asString();
    
    if (!root.isMember("name") || !root["name"].isString()) {
        return core::Result<PluginManifest>::Err("Missing or invalid field: name");
    }
    manifest.name = root["name"].asString();
    
    if (!root.isMember("version") || !root["version"].isString()) {
        return core::Result<PluginManifest>::Err("Missing or invalid field: version");
    }
    manifest.version = root["version"].asString();
    
    if (!root.isMember("type") || !root["type"].isString()) {
        return core::Result<PluginManifest>::Err("Missing or invalid field: type");
    }
    manifest.type = root["type"].asString();
    
    if (!root.isMember("main") || !root["main"].isString()) {
        return core::Result<PluginManifest>::Err("Missing or invalid field: main");
    }
    manifest.main = root["main"].asString();
    
    // 可选字段
    if (root.isMember("author") && root["author"].isString()) {
        manifest.author = root["author"].asString();
    }
    
    if (root.isMember("description") && root["description"].isString()) {
        manifest.description = root["description"].asString();
    }
    
    if (root.isMember("license") && root["license"].isString()) {
        manifest.license = root["license"].asString();
    }
    
    if (root.isMember("homepage") && root["homepage"].isString()) {
        manifest.homepage = root["homepage"].asString();
    }
    
    // config (对象转 map)
    if (root.isMember("config") && root["config"].isObject()) {
        const Json::Value& config = root["config"];
        for (const auto& key : config.getMemberNames()) {
            if (config[key].isObject() && config[key].isMember("default")) {
                manifest.config[key] = config[key]["default"].asString();
            } else if (config[key].isString()) {
                manifest.config[key] = config[key].asString();
            }
        }
    }
    
    return core::Result<PluginManifest>::Ok(manifest);
}

// ========================================
// Manifest 验证
// ========================================

core::Result<bool> MemoryPluginManager::validate_manifest(const PluginManifest& manifest) {
    if (manifest.id.empty()) {
        return core::Result<bool>::Err("Manifest id is empty");
    }
    
    if (manifest.type != "memory") {
        return core::Result<bool>::Err("Manifest type is not 'memory': " + manifest.type);
    }
    
    if (manifest.main.empty()) {
        return core::Result<bool>::Err("Manifest main is empty");
    }
    
    return core::Result<bool>::Ok(true);
}

} // namespace memory
} // namespace hunterclaw
