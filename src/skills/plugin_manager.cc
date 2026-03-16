#include "hunterclaw/skills/plugin_interface.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <dlfcn.h>

namespace hunterclaw {
namespace skills {

using json = nlohmann::json;

namespace {
    auto log() {
        static auto logger = spdlog::get("skills");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("skills");
        }
        return logger;
    }
}

// ============================================================================
// SkillPluginLoader 实现
// ============================================================================

std::vector<SkillPluginManifest> SkillPluginLoader::discover_plugins(
    const fs::path& plugin_dir,
    SkillPluginOrigin origin
) {
    std::vector<SkillPluginManifest> manifests;
    
    if (!fs::exists(plugin_dir) || !fs::is_directory(plugin_dir)) {
        return manifests;
    }
    
    log()->debug("Discovering plugins in: {}", plugin_dir.string());
    
    try {
        // 遍历插件目录
        for (const auto& entry : fs::directory_iterator(plugin_dir)) {
            if (!entry.is_directory()) {
                continue;
            }
            
            // 查找 manifest.json
            fs::path manifest_path = entry.path() / "manifest.json";
            if (!fs::exists(manifest_path)) {
                continue;
            }
            
            // 解析 Manifest
            auto result = parse_manifest(manifest_path);
            if (result.is_err()) {
                log()->warn("Failed to parse manifest {}: {}", 
                           manifest_path.string(), result.error);
                continue;
            }
            
            auto manifest = result.value;
            manifest.root_dir = entry.path();
            
            // 设置来源
            switch (origin) {
                case SkillPluginOrigin::Bundled:
                    manifest.origin = "bundled";
                    break;
                case SkillPluginOrigin::User:
                    manifest.origin = "user";
                    break;
                case SkillPluginOrigin::Workspace:
                    manifest.origin = "workspace";
                    break;
                case SkillPluginOrigin::Extensions:
                    manifest.origin = "extensions";
                    break;
            }
            
            // 验证 Manifest
            auto validate_result = validate_manifest(manifest);
            if (validate_result.is_err()) {
                log()->warn("Invalid manifest {}: {}", 
                           manifest_path.string(), validate_result.error);
                continue;
            }
            
            manifests.push_back(manifest);
            log()->info("Discovered plugin: {} ({})", manifest.id, manifest.version);
        }
    } catch (const std::exception& e) {
        log()->error("Error discovering plugins: {}", e.what());
    }
    
    return manifests;
}

core::Result<std::shared_ptr<SkillPlugin>> SkillPluginLoader::load_plugin(
    const SkillPluginManifest& manifest
) {
    log()->info("Loading plugin: {} ({})", manifest.id, manifest.version);
    
    // 1. 构建 .so 路径
    fs::path plugin_so = manifest.root_dir / manifest.main;
    if (!fs::exists(plugin_so)) {
        return core::Result<std::shared_ptr<SkillPlugin>>::Err(
            "Plugin library not found: " + plugin_so.string()
        );
    }
    
    // 2. dlopen 加载 .so
    void* handle = dlopen(plugin_so.string().c_str(), RTLD_LAZY);
    if (!handle) {
        return core::Result<std::shared_ptr<SkillPlugin>>::Err(
            std::string("Failed to load plugin library: ") + dlerror()
        );
    }
    
    // 3. dlsym 查找 create_skill_plugin
    dlerror(); // 清除错误
    
    auto create_fn = reinterpret_cast<CreateSkillPluginFn>(
        dlsym(handle, "create_skill_plugin")
    );
    
    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        dlclose(handle);
        return core::Result<std::shared_ptr<SkillPlugin>>::Err(
            std::string("Failed to find create_skill_plugin: ") + dlsym_error
        );
    }
    
    // 4. 调用 create_skill_plugin()
    SkillPlugin* raw_plugin = create_fn();
    if (!raw_plugin) {
        dlclose(handle);
        return core::Result<std::shared_ptr<SkillPlugin>>::Err(
            "create_skill_plugin() returned nullptr"
        );
    }
    
    // 5. 包装为 shared_ptr (自定义 deleter)
    auto plugin = std::shared_ptr<SkillPlugin>(
        raw_plugin,
        [handle](SkillPlugin* p) {
            // 查找 destroy_skill_plugin
            auto destroy_fn = reinterpret_cast<DestroySkillPluginFn>(
                dlsym(handle, "destroy_skill_plugin")
            );
            
            if (destroy_fn) {
                destroy_fn(p);
            } else {
                delete p; // 回退到 delete
            }
            
            dlclose(handle);
        }
    );
    
    // 6. 初始化插件 (TODO: 传递配置)
    std::map<std::string, std::string> config;
    auto init_result = plugin->init(config);
    if (init_result.is_err()) {
        return core::Result<std::shared_ptr<SkillPlugin>>::Err(
            "Plugin init failed: " + init_result.error
        );
    }
    
    log()->info("Loaded plugin: {}", manifest.id);
    return core::Result<std::shared_ptr<SkillPlugin>>::Ok(plugin);
}

void SkillPluginLoader::unload_plugin(
    std::shared_ptr<SkillPlugin> plugin,
    void* handle
) {
    if (!plugin) return;
    
    log()->info("Unloading plugin: {}", plugin->get_id());
    
    // 1. shutdown()
    plugin->shutdown();
    
    // 2. shared_ptr 自动调用 deleter (destroy + dlclose)
    plugin.reset();
}

core::Result<SkillPluginManifest> SkillPluginLoader::parse_manifest(
    const fs::path& manifest_path
) {
    try {
        std::ifstream file(manifest_path);
        if (!file.is_open()) {
            return core::Result<SkillPluginManifest>::Err(
                "Failed to open manifest: " + manifest_path.string()
            );
        }
        
        json j;
        file >> j;
        
        SkillPluginManifest manifest;
        
        // 必填字段
        manifest.id = j.at("id").get<std::string>();
        manifest.name = j.at("name").get<std::string>();
        manifest.version = j.at("version").get<std::string>();
        manifest.kind = j.at("kind").get<std::string>();
        
        // 可选字段
        if (j.contains("description")) {
            manifest.description = j["description"].get<std::string>();
        }
        if (j.contains("author")) {
            manifest.author = j["author"].get<std::string>();
        }
        if (j.contains("homepage")) {
            manifest.homepage = j["homepage"].get<std::string>();
        }
        if (j.contains("skills")) {
            manifest.skills = j["skills"].get<std::vector<std::string>>();
        }
        if (j.contains("configSchema")) {
            manifest.config_schema = j["configSchema"].dump();
        }
        if (j.contains("enabled")) {
            manifest.enabled = j["enabled"].get<bool>();
        }
        if (j.contains("main")) {
            manifest.main = j["main"].get<std::string>();
        } else {
            // 默认值
            manifest.main = "lib" + manifest.id + ".so";
        }
        
        return core::Result<SkillPluginManifest>::Ok(manifest);
        
    } catch (const json::exception& e) {
        return core::Result<SkillPluginManifest>::Err(
            std::string("JSON parse error: ") + e.what()
        );
    } catch (const std::exception& e) {
        return core::Result<SkillPluginManifest>::Err(
            std::string("Parse error: ") + e.what()
        );
    }
}

core::Result<bool> SkillPluginLoader::validate_manifest(
    const SkillPluginManifest& manifest
) {
    // 1. 必填字段
    if (manifest.id.empty()) {
        return core::Result<bool>::Err("Missing required field: id");
    }
    if (manifest.name.empty()) {
        return core::Result<bool>::Err("Missing required field: name");
    }
    if (manifest.version.empty()) {
        return core::Result<bool>::Err("Missing required field: version");
    }
    if (manifest.kind.empty()) {
        return core::Result<bool>::Err("Missing required field: kind");
    }
    
    // 2. kind 必须是 "skills"
    if (manifest.kind != "skills") {
        return core::Result<bool>::Err(
            "Invalid kind (expected 'skills', got '" + manifest.kind + "')"
        );
    }
    
    // 3. main 必须存在
    if (manifest.main.empty()) {
        return core::Result<bool>::Err("Missing required field: main");
    }
    
    // 4. 验证 skills 路径 (安全检查)
    for (const auto& skill_path : manifest.skills) {
        fs::path resolved = manifest.root_dir / skill_path;
        
        // 确保路径在插件根目录内 (防止路径遍历攻击)
        fs::path canonical_root = fs::canonical(manifest.root_dir);
        fs::path canonical_path = fs::canonical(resolved);
        
        std::string root_str = canonical_root.string();
        std::string path_str = canonical_path.string();
        
        if (path_str.find(root_str) != 0) {
            return core::Result<bool>::Err(
                "Skill path escapes plugin root: " + skill_path
            );
        }
    }
    
    return core::Result<bool>::Ok(true);
}

// ============================================================================
// SkillPluginRegistry 实现
// ============================================================================

void SkillPluginRegistry::register_plugin(const SkillPluginRecord& record) {
    std::unique_lock lock(mutex_);
    plugins_[record.manifest.id] = record;
}

void SkillPluginRegistry::unregister_plugin(const std::string& id) {
    std::unique_lock lock(mutex_);
    plugins_.erase(id);
}

std::optional<SkillPluginRecord> SkillPluginRegistry::get_plugin(
    const std::string& id
) const {
    std::shared_lock lock(mutex_);
    auto it = plugins_.find(id);
    if (it != plugins_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<SkillPluginRecord> SkillPluginRegistry::list_plugins() const {
    std::shared_lock lock(mutex_);
    std::vector<SkillPluginRecord> result;
    result.reserve(plugins_.size());
    for (const auto& [id, record] : plugins_) {
        result.push_back(record);
    }
    return result;
}

std::vector<SkillPluginRecord> SkillPluginRegistry::list_enabled_plugins() const {
    std::shared_lock lock(mutex_);
    std::vector<SkillPluginRecord> result;
    for (const auto& [id, record] : plugins_) {
        if (record.manifest.enabled) {
            result.push_back(record);
        }
    }
    return result;
}

void SkillPluginRegistry::clear() {
    std::unique_lock lock(mutex_);
    plugins_.clear();
}

// ============================================================================
// SkillPluginManager::Impl
// ============================================================================

struct SkillPluginManager::Impl {
    SkillPluginRegistry registry_;
    std::map<std::string, void*> plugin_handles_;  // id -> dlopen handle
    
    core::Result<bool> load_plugins(const std::optional<std::string>& workspace_dir) {
        log()->info("Loading skill plugins...");
        
        // 1. Bundled Plugins
        auto bundled_dir = core::utils::expand_path("~/.hunterclaw/plugins-bundled");
        auto bundled_manifests = SkillPluginLoader::discover_plugins(
            bundled_dir, SkillPluginOrigin::Bundled
        );
        for (const auto& manifest : bundled_manifests) {
            load_plugin_internal(manifest);
        }
        
        // 2. OpenClaw Extensions
        auto extensions_dir = core::utils::expand_path("~/.openclaw/extensions");
        if (fs::exists(extensions_dir)) {
            for (const auto& ext_entry : fs::directory_iterator(extensions_dir)) {
                if (!ext_entry.is_directory()) continue;
                
                fs::path plugin_dir = ext_entry.path() / "plugins";
                if (!fs::exists(plugin_dir)) continue;
                
                auto ext_manifests = SkillPluginLoader::discover_plugins(
                    plugin_dir, SkillPluginOrigin::Extensions
                );
                for (const auto& manifest : ext_manifests) {
                    load_plugin_internal(manifest);
                }
            }
        }
        
        // 3. User Plugins
        auto user_dir = core::utils::expand_path("~/.hunterclaw/plugins");
        auto user_manifests = SkillPluginLoader::discover_plugins(
            user_dir, SkillPluginOrigin::User
        );
        for (const auto& manifest : user_manifests) {
            load_plugin_internal(manifest);
        }
        
        // 4. Workspace Plugins
        if (workspace_dir) {
            fs::path workspace_plugin_dir = fs::path(*workspace_dir) / ".hunterclaw" / "plugins";
            auto workspace_manifests = SkillPluginLoader::discover_plugins(
                workspace_plugin_dir, SkillPluginOrigin::Workspace
            );
            for (const auto& manifest : workspace_manifests) {
                load_plugin_internal(manifest);
            }
        }
        
        log()->info("Loaded {} skill plugin(s)", registry_.list_plugins().size());
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> load_plugin_internal(const SkillPluginManifest& manifest) {
        // 检查是否已加载
        auto existing = registry_.get_plugin(manifest.id);
        if (existing && existing->loaded) {
            log()->debug("Plugin already loaded: {}", manifest.id);
            return core::Result<bool>::Ok(true);
        }
        
        // 检查是否启用
        if (!manifest.enabled) {
            log()->debug("Plugin disabled: {}", manifest.id);
            SkillPluginRecord record;
            record.manifest = manifest;
            record.loaded = false;
            registry_.register_plugin(record);
            return core::Result<bool>::Ok(true);
        }
        
        // 加载插件
        auto result = SkillPluginLoader::load_plugin(manifest);
        if (result.is_err()) {
            log()->error("Failed to load plugin {}: {}", manifest.id, result.error);
            return core::Result<bool>::Err(result.error);
        }
        
        // 注册到 Registry
        SkillPluginRecord record;
        record.manifest = manifest;
        record.instance = result.value;
        record.loaded = true;
        
        registry_.register_plugin(record);
        
        return core::Result<bool>::Ok(true);
    }
    
    std::vector<Skill> collect_plugin_skills() const {
        std::vector<Skill> all_skills;
        
        for (const auto& record : registry_.list_enabled_plugins()) {
            if (!record.loaded || !record.instance) {
                continue;
            }
            
            auto skills = record.instance->list_skills();
            all_skills.insert(all_skills.end(), skills.begin(), skills.end());
        }
        
        return all_skills;
    }
};

// ============================================================================
// SkillPluginManager 实现
// ============================================================================

std::shared_ptr<SkillPluginManager> SkillPluginManager::create() {
    return std::shared_ptr<SkillPluginManager>(new SkillPluginManager());
}

SkillPluginManager::SkillPluginManager()
    : impl_(std::make_unique<Impl>()) {}

SkillPluginManager::~SkillPluginManager() = default;

core::Result<bool> SkillPluginManager::load_plugins(
    const std::optional<std::string>& workspace_dir
) {
    return impl_->load_plugins(workspace_dir);
}

core::Result<bool> SkillPluginManager::load_plugin(const std::string& plugin_id) {
    auto record_opt = impl_->registry_.get_plugin(plugin_id);
    if (!record_opt) {
        return core::Result<bool>::Err("Plugin not found: " + plugin_id);
    }
    
    return impl_->load_plugin_internal(record_opt->manifest);
}

core::Result<bool> SkillPluginManager::unload_plugin(const std::string& plugin_id) {
    auto record_opt = impl_->registry_.get_plugin(plugin_id);
    if (!record_opt) {
        return core::Result<bool>::Err("Plugin not found: " + plugin_id);
    }
    
    auto& record = record_opt.value();
    
    if (!record.loaded || !record.instance) {
        return core::Result<bool>::Ok(true);
    }
    
    // shutdown()
    record.instance->shutdown();
    record.instance.reset();
    
    // 更新 Registry
    SkillPluginRecord updated = record;
    updated.loaded = false;
    updated.instance = nullptr;
    impl_->registry_.register_plugin(updated);
    
    log()->info("Unloaded plugin: {}", plugin_id);
    return core::Result<bool>::Ok(true);
}

core::Result<bool> SkillPluginManager::reload_plugin(const std::string& plugin_id) {
    auto unload_result = unload_plugin(plugin_id);
    if (unload_result.is_err()) {
        return unload_result;
    }
    
    return load_plugin(plugin_id);
}

const SkillPluginRegistry& SkillPluginManager::get_registry() const {
    return impl_->registry_;
}

std::vector<Skill> SkillPluginManager::collect_plugin_skills() const {
    return impl_->collect_plugin_skills();
}

core::Result<bool> SkillPluginManager::refresh_plugin_skills() {
    for (const auto& record : impl_->registry_.list_enabled_plugins()) {
        if (!record.loaded || !record.instance) {
            continue;
        }
        
        auto result = record.instance->refresh_skills();
        if (result.is_err()) {
            log()->warn("Failed to refresh skills for plugin {}: {}", 
                       record.manifest.id, result.error);
        }
    }
    
    return core::Result<bool>::Ok(true);
}

} // namespace skills
} // namespace hunterclaw
