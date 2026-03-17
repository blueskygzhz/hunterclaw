#include "hunterclaw/plugins/npm_loader.h"
#include "hunterclaw/core/json.h"
#include <filesystem>  // ✅ 使用标准库而非自定义 filesystem.h
#include "hunterclaw/core/subprocess.h"  // ✅ Process 在 subprocess.h 中定义
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace plugins {

// ============================================================================
// NpmPluginLoader Implementation
// ============================================================================

struct NpmPluginLoader::Impl {
    std::shared_ptr<core::Config> config;
    std::string cache_dir;
    std::mutex mutex;
    
    // npm 包缓存 (package_name -> install_path)
    std::map<std::string, std::string> npm_cache;
    
    core::Result<std::string> ensure_npm_available() {
        // 检查 npm 是否可用
        auto npm_check = core::Process::execute("npm --version");
        if (npm_check.is_err()) {
            return core::Result<std::string>::Err("npm is not installed or not in PATH");
        }
        
        auto [stdout_content, stderr_content, exit_code] = npm_check.unwrap();
        if (exit_code != 0) {
            return core::Result<std::string>::Err("npm is not available");
        }
        
        return core::Result<std::string>::Ok(stdout_content);
    }
    
    core::Result<std::string> install_npm_package(
        const std::string& package_spec,
        const std::string& target_dir
    ) {
        // 创建目标目录
        if (!core::fs::create_directories(target_dir)) {
            return core::Result<std::string>::Err("Failed to create target directory");
        }
        
        // 初始化 package.json
        std::string package_json_path = target_dir + "/package.json";
        if (!core::fs::exists(package_json_path)) {
            std::ofstream ofs(package_json_path);
            ofs << R"({
  "name": "hunterclaw-plugin-temp",
  "version": "1.0.0",
  "private": true
})" << std::endl;
            ofs.close();
        }
        
        // 执行 npm install
        std::string cmd = "cd " + target_dir + " && npm install --no-save " + package_spec;
        auto result = core::Process::execute(cmd);
        if (result.is_err()) {
            return core::Result<std::string>::Err("Failed to execute npm install: " + result.unwrap_err());
        }
        
        auto [stdout_content, stderr_content, exit_code] = result.unwrap();
        if (exit_code != 0) {
            return core::Result<std::string>::Err("npm install failed: " + stderr_content);
        }
        
        // 查找安装的包路径
        std::string node_modules = target_dir + "/node_modules";
        std::string package_name = extract_package_name(package_spec);
        std::string install_path = node_modules + "/" + package_name;
        
        if (!core::fs::exists(install_path)) {
            return core::Result<std::string>::Err("Package installed but not found: " + install_path);
        }
        
        return core::Result<std::string>::Ok(install_path);
    }
    
    std::string extract_package_name(const std::string& package_spec) {
        // 从 package_spec 提取包名
        // 示例: @scope/package@1.0.0 -> @scope/package
        size_t at_pos = package_spec.find_last_of('@');
        if (at_pos == std::string::npos || at_pos == 0) {
            return package_spec;
        }
        
        // 处理 scoped package (@scope/package@version)
        if (package_spec[0] == '@') {
            // 跳过第一个 @, 找第二个 @
            size_t second_at = package_spec.find('@', 1);
            if (second_at != std::string::npos) {
                return package_spec.substr(0, second_at);
            }
        }
        
        return package_spec.substr(0, at_pos);
    }
    
    core::Result<core::JsonValue> read_package_json(const std::string& package_dir) {
        std::string package_json_path = package_dir + "/package.json";
        if (!core::fs::exists(package_json_path)) {
            return core::Result<core::JsonValue>::Err("package.json not found");
        }
        
        std::ifstream ifs(package_json_path);
        if (!ifs.is_open()) {
            return core::Result<core::JsonValue>::Err("Failed to open package.json");
        }
        
        std::stringstream ss;
        ss << ifs.rdbuf();
        ifs.close();
        
        auto parse_result = core::Json::parse(ss.str());
        if (parse_result.is_err()) {
            return core::Result<core::JsonValue>::Err("Failed to parse package.json: " + parse_result.unwrap_err());
        }
        
        return core::Result<core::JsonValue>::Ok(parse_result.unwrap());
    }
    
    core::Result<std::string> resolve_entry_point(
        const std::string& package_dir,
        const core::JsonValue& package_json
    ) {
        // 优先级: hunterclaw.main > main > index.js
        
        // 1. 检查 hunterclaw.main
        if (package_json.has("hunterclaw") && package_json["hunterclaw"].has("main")) {
            std::string main_path = package_json["hunterclaw"]["main"].as_string();
            std::string full_path = package_dir + "/" + main_path;
            if (core::fs::exists(full_path)) {
                return core::Result<std::string>::Ok(full_path);
            }
        }
        
        // 2. 检查 main
        if (package_json.has("main")) {
            std::string main_path = package_json["main"].as_string();
            std::string full_path = package_dir + "/" + main_path;
            if (core::fs::exists(full_path)) {
                return core::Result<std::string>::Ok(full_path);
            }
        }
        
        // 3. 默认 index.js
        std::string default_path = package_dir + "/index.js";
        if (core::fs::exists(default_path)) {
            return core::Result<std::string>::Ok(default_path);
        }
        
        return core::Result<std::string>::Err("Entry point not found");
    }
};

// ============================================================================
// NpmPluginLoader
// ============================================================================

NpmPluginLoader::NpmPluginLoader(std::shared_ptr<core::Config> config)
    : impl_(std::make_unique<Impl>())
{
    impl_->config = config;
    impl_->cache_dir = config->get_string("plugins.npm_cache_dir", "~/.hunterclaw/npm_cache");
    
    // 展开 ~ 符号
    if (impl_->cache_dir[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            impl_->cache_dir = std::string(home) + impl_->cache_dir.substr(1);
        }
    }
    
    // 确保缓存目录存在
    core::fs::create_directories(impl_->cache_dir);
}

NpmPluginLoader::~NpmPluginLoader() = default;

core::Result<NpmPluginInfo> NpmPluginLoader::load(const std::string& package_spec) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    // 1. 检查缓存
    auto cache_it = impl_->npm_cache.find(package_spec);
    if (cache_it != impl_->npm_cache.end()) {
        std::string cached_path = cache_it->second;
        if (core::fs::exists(cached_path)) {
            // 从缓存加载
            return load_from_path(cached_path);
        } else {
            // 缓存失效, 移除
            impl_->npm_cache.erase(cache_it);
        }
    }
    
    // 2. 检查 npm 可用性
    auto npm_check = impl_->ensure_npm_available();
    if (npm_check.is_err()) {
        return core::Result<NpmPluginInfo>::Err(npm_check.unwrap_err());
    }
    
    // 3. 安装 npm 包
    std::string package_name = impl_->extract_package_name(package_spec);
    std::string target_dir = impl_->cache_dir + "/" + package_name;
    
    auto install_result = impl_->install_npm_package(package_spec, target_dir);
    if (install_result.is_err()) {
        return core::Result<NpmPluginInfo>::Err(install_result.unwrap_err());
    }
    
    std::string install_path = install_result.unwrap();
    
    // 4. 加载插件信息
    auto load_result = load_from_path(install_path);
    if (load_result.is_ok()) {
        // 缓存路径
        impl_->npm_cache[package_spec] = install_path;
    }
    
    return load_result;
}

core::Result<NpmPluginInfo> NpmPluginLoader::load_from_path(const std::string& package_dir) {
    // 1. 读取 package.json
    auto package_json_result = impl_->read_package_json(package_dir);
    if (package_json_result.is_err()) {
        return core::Result<NpmPluginInfo>::Err(package_json_result.unwrap_err());
    }
    
    core::JsonValue package_json = package_json_result.unwrap();
    
    // 2. 解析入口点
    auto entry_result = impl_->resolve_entry_point(package_dir, package_json);
    if (entry_result.is_err()) {
        return core::Result<NpmPluginInfo>::Err(entry_result.unwrap_err());
    }
    
    std::string entry_point = entry_result.unwrap();
    
    // 3. 构建 NpmPluginInfo
    NpmPluginInfo info;
    info.package_dir = package_dir;
    info.entry_point = entry_point;
    info.package_name = package_json.get("name", "unknown");
    info.version = package_json.get("version", "0.0.0");
    info.description = package_json.get("description", "");
    
    // 4. 检查是否是 HunterClaw 插件
    if (package_json.has("hunterclaw")) {
        auto hc = package_json["hunterclaw"];
        info.is_hunterclaw_plugin = true;
        
        if (hc.has("plugin_id")) {
            info.plugin_id = hc["plugin_id"].as_string();
        } else {
            info.plugin_id = info.package_name;
        }
        
        if (hc.has("plugin_type")) {
            info.plugin_type = hc["plugin_type"].as_string();
        }
    } else {
        info.is_hunterclaw_plugin = false;
        info.plugin_id = info.package_name;
    }
    
    return core::Result<NpmPluginInfo>::Ok(info);
}

core::Result<bool> NpmPluginLoader::uninstall(const std::string& package_spec) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    // 从缓存移除
    auto cache_it = impl_->npm_cache.find(package_spec);
    if (cache_it == impl_->npm_cache.end()) {
        return core::Result<bool>::Err("Package not found in cache");
    }
    
    std::string package_dir = cache_it->second;
    
    // 删除包目录
    if (core::fs::exists(package_dir)) {
        if (!core::fs::remove_all(package_dir)) {
            return core::Result<bool>::Err("Failed to remove package directory");
        }
    }
    
    impl_->npm_cache.erase(cache_it);
    
    return core::Result<bool>::Ok(true);
}

void NpmPluginLoader::clear_cache() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    // 清除所有缓存
    impl_->npm_cache.clear();
    
    // 删除缓存目录
    if (core::fs::exists(impl_->cache_dir)) {
        core::fs::remove_all(impl_->cache_dir);
        core::fs::create_directories(impl_->cache_dir);
    }
}

std::vector<std::string> NpmPluginLoader::list_installed() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    std::vector<std::string> installed;
    for (const auto& [package_spec, path] : impl_->npm_cache) {
        if (core::fs::exists(path)) {
            installed.push_back(package_spec);
        }
    }
    
    return installed;
}

} // namespace plugins
} // namespace hunterclaw
