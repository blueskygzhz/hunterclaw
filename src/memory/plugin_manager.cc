#include "hunterclaw/memory/plugin_manager.h"
#include "hunterclaw/memory/provider_registry.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <dlfcn.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

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

// ============================================================================
// MemoryPluginRegistry
// ============================================================================

MemoryPluginRegistry& MemoryPluginRegistry::instance() {
    static MemoryPluginRegistry inst;
    return inst;
}

void MemoryPluginRegistry::register_plugin(const MemoryPluginDefinition& def) {
    plugins_[def.id] = def;
    log()->info("MemoryPluginRegistry: registered plugin '{}'", def.id);
}

const MemoryPluginDefinition* MemoryPluginRegistry::get_plugin(const std::string& id) const {
    auto it = plugins_.find(id);
    return it != plugins_.end() ? &it->second : nullptr;
}

std::vector<std::string> MemoryPluginRegistry::list_plugin_ids() const {
    std::vector<std::string> ids;
    ids.reserve(plugins_.size());
    for (const auto& [id, _] : plugins_) {
        ids.push_back(id);
    }
    return ids;
}

bool MemoryPluginRegistry::has_plugin(const std::string& id) const {
    return plugins_.find(id) != plugins_.end();
}

// ============================================================================
// MemoryPluginLoader
// ============================================================================

MemoryPluginLoader& MemoryPluginLoader::instance() {
    static MemoryPluginLoader inst;
    return inst;
}

MemoryPluginLoader::~MemoryPluginLoader() {
    // Close all loaded .so handles
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, handle] : handles_) {
        if (handle) {
            dlclose(handle);
        }
    }
    handles_.clear();
}

void MemoryPluginLoader::add_plugins_directory(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    plugin_dirs_.push_back(dir);
}

void MemoryPluginLoader::set_plugins_directories(const std::vector<std::string>& dirs) {
    std::lock_guard<std::mutex> lock(mutex_);
    plugin_dirs_ = dirs;
}

std::vector<std::string> MemoryPluginLoader::get_plugins_directories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return plugin_dirs_;
}

core::Result<std::vector<PluginManifest>> MemoryPluginLoader::discover_plugins() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Default directories if none configured
    if (plugin_dirs_.empty()) {
        const char* home = std::getenv("HOME");
        if (home) {
            plugin_dirs_.push_back(std::string(home) + "/.hunterclaw/plugins");
        }
        plugin_dirs_.push_back("/usr/lib/hunterclaw/plugins");
        plugin_dirs_.push_back("/usr/local/lib/hunterclaw/plugins");
    }

    std::vector<PluginManifest> manifests;

    for (const auto& dir : plugin_dirs_) {
        if (!fs::exists(dir)) {
            log()->debug("MemoryPluginLoader: directory {} does not exist, skipping", dir);
            continue;
        }

        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;

            const std::string filename = entry.path().filename().string();
            if (filename.size() < 12 ||
                filename.compare(filename.size() - 12, 12, ".plugin.json") != 0) {
                continue;
            }

            auto result = parse_manifest(entry.path().string());
            if (!result.ok()) {
                log()->warn("MemoryPluginLoader: failed to parse {}: {}",
                    entry.path().string(), result.error());
                continue;
            }

            auto manifest = result.value();
            if (manifest.kind != "memory") {
                log()->debug("MemoryPluginLoader: skipping non-memory plugin {} (kind: {})",
                    manifest.id, manifest.kind);
                continue;
            }

            log()->info("MemoryPluginLoader: discovered plugin '{}' at {}",
                manifest.id, entry.path().string());
            manifests.push_back(std::move(manifest));
        }
    }

    return core::Result<std::vector<PluginManifest>>::ok(std::move(manifests));
}

core::Result<void> MemoryPluginLoader::load_plugin(const std::string& manifest_path) {
    auto manifest_result = parse_manifest(manifest_path);
    if (!manifest_result.ok()) {
        return core::Result<void>::err(manifest_result.error());
    }

    const auto& manifest = manifest_result.value();

    auto validate_result = validate_manifest(manifest);
    if (!validate_result.ok()) {
        return core::Result<void>::err(validate_result.error());
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (handles_.count(manifest.id)) {
        log()->debug("MemoryPluginLoader: plugin '{}' already loaded", manifest.id);
        return core::Result<void>::ok();
    }

    // Locate .so file
    const fs::path manifest_dir = fs::path(manifest_path).parent_path();
    const fs::path so_path = manifest_dir / manifest.main;

    if (!fs::exists(so_path)) {
        return core::Result<void>::err("Plugin library not found: " + so_path.string());
    }

    // dlopen
    void* handle = dlopen(so_path.c_str(), RTLD_LAZY);
    if (!handle) {
        return core::Result<void>::err(
            std::string("dlopen failed: ") + dlerror());
    }

    // Locate register_memory_plugin symbol
    // Convention: each memory plugin .so exports:
    //   extern "C" void register_memory_plugin(MemoryPluginRegistry&);
    using RegisterFn = void (*)(MemoryPluginRegistry&);
    dlerror();
    auto register_fn = reinterpret_cast<RegisterFn>(dlsym(handle, "register_memory_plugin"));
    const char* sym_err = dlerror();
    if (sym_err) {
        dlclose(handle);
        return core::Result<void>::err(
            std::string("register_memory_plugin not found: ") + sym_err);
    }

    // Call register function — plugin registers its MemoryPluginDefinition
    register_fn(MemoryPluginRegistry::instance());

    handles_[manifest.id] = handle;
    log()->info("MemoryPluginLoader: loaded plugin '{}' from {}", manifest.id, so_path.string());

    return core::Result<void>::ok();
}

core::Result<int> MemoryPluginLoader::load_all_plugins() {
    auto discover_result = discover_plugins();
    if (!discover_result.ok()) {
        return core::Result<int>::err(discover_result.error());
    }

    int loaded = 0;
    for (const auto& manifest : discover_result.value()) {
        // Re-derive manifest path from directories
        for (const auto& dir : plugin_dirs_) {
            const fs::path candidate = fs::path(dir) / (manifest.id + ".plugin.json");
            if (fs::exists(candidate)) {
                auto r = load_plugin(candidate.string());
                if (r.ok()) {
                    ++loaded;
                } else {
                    log()->warn("MemoryPluginLoader: failed to load '{}': {}",
                        manifest.id, r.error());
                }
                break;
            }
        }
    }

    log()->info("MemoryPluginLoader: loaded {} plugin(s)", loaded);
    return core::Result<int>::ok(loaded);
}

core::Result<void> MemoryPluginLoader::unload_plugin(const std::string& plugin_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = handles_.find(plugin_id);
    if (it == handles_.end()) {
        return core::Result<void>::err("Plugin not loaded: " + plugin_id);
    }

    dlclose(it->second);
    handles_.erase(it);

    log()->info("MemoryPluginLoader: unloaded plugin '{}'", plugin_id);
    return core::Result<void>::ok();
}

// ============================================================================
// Manifest parsing
// ============================================================================

core::Result<PluginManifest> MemoryPluginLoader::parse_manifest(
    const std::string& manifest_path)
{
    std::ifstream file(manifest_path);
    if (!file.is_open()) {
        return core::Result<PluginManifest>::err("Cannot open: " + manifest_path);
    }

    json root;
    try {
        file >> root;
    } catch (const json::exception& e) {
        return core::Result<PluginManifest>::err(
            std::string("JSON parse error: ") + e.what());
    }

    PluginManifest m;

    auto require_string = [&](const char* field) -> core::Result<std::string> {
        if (!root.contains(field) || !root[field].is_string()) {
            return core::Result<std::string>::err(
                std::string("Missing or invalid field: ") + field);
        }
        return core::Result<std::string>::ok(root[field].get<std::string>());
    };

    auto id_r = require_string("id");
    if (!id_r.ok()) return core::Result<PluginManifest>::err(id_r.error());
    m.id = id_r.value();

    auto name_r = require_string("name");
    if (!name_r.ok()) return core::Result<PluginManifest>::err(name_r.error());
    m.name = name_r.value();

    auto version_r = require_string("version");
    if (!version_r.ok()) return core::Result<PluginManifest>::err(version_r.error());
    m.version = version_r.value();

    // "kind" (preferred) or legacy "type"
    if (root.contains("kind") && root["kind"].is_string()) {
        m.kind = root["kind"].get<std::string>();
    } else if (root.contains("type") && root["type"].is_string()) {
        m.kind = root["type"].get<std::string>();
    } else {
        return core::Result<PluginManifest>::err("Missing field: kind (or type)");
    }

    auto main_r = require_string("main");
    if (!main_r.ok()) return core::Result<PluginManifest>::err(main_r.error());
    m.main = main_r.value();

    // Optional fields
    if (root.contains("description") && root["description"].is_string())
        m.description = root["description"].get<std::string>();
    if (root.contains("author") && root["author"].is_string())
        m.author = root["author"].get<std::string>();
    if (root.contains("license") && root["license"].is_string())
        m.license = root["license"].get<std::string>();
    if (root.contains("homepage") && root["homepage"].is_string())
        m.homepage = root["homepage"].get<std::string>();

    // config defaults
    if (root.contains("config") && root["config"].is_object()) {
        for (auto& [key, val] : root["config"].items()) {
            if (val.is_object() && val.contains("default")) {
                m.config[key] = val["default"].dump();
            } else if (val.is_string()) {
                m.config[key] = val.get<std::string>();
            }
        }
    }

    return core::Result<PluginManifest>::ok(std::move(m));
}

core::Result<void> MemoryPluginLoader::validate_manifest(const PluginManifest& m) {
    if (m.id.empty())
        return core::Result<void>::err("Manifest id is empty");
    if (m.kind != "memory")
        return core::Result<void>::err("Manifest kind is not 'memory': " + m.kind);
    if (m.main.empty())
        return core::Result<void>::err("Manifest main is empty");
    return core::Result<void>::ok();
}

} // namespace memory
} // namespace hunterclaw
