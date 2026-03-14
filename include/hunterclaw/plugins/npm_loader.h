#pragma once

#include "hunterclaw/core/types.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace hunterclaw {
namespace plugins {

// ============================================================================
// NpmPluginInfo - npm 包插件信息
// ============================================================================

struct NpmPluginInfo {
    std::string package_dir;       // npm 包安装目录
    std::string entry_point;       // 入口文件路径
    std::string package_name;      // package.json 中的 name
    std::string version;           // package.json 中的 version
    std::string description;       // package.json 中的 description
    
    bool is_hunterclaw_plugin;     // 是否是 HunterClaw 插件
    std::string plugin_id;         // 插件 ID (来自 hunterclaw.plugin_id 或 name)
    std::string plugin_type;       // 插件类型 (memory/skills/channels/tools)
};

// ============================================================================
// NpmPluginLoader - npm 包插件加载器
// ============================================================================

class NpmPluginLoader {
public:
    explicit NpmPluginLoader(std::shared_ptr<core::Config> config);
    ~NpmPluginLoader();
    
    // 加载 npm 包插件
    // package_spec 格式:
    //   - package-name
    //   - package-name@version
    //   - @scope/package-name
    //   - @scope/package-name@version
    core::Result<NpmPluginInfo> load(const std::string& package_spec);
    
    // 从本地路径加载 (不通过 npm install)
    core::Result<NpmPluginInfo> load_from_path(const std::string& package_dir);
    
    // 卸载 npm 包插件
    core::Result<bool> uninstall(const std::string& package_spec);
    
    // 清除所有缓存
    void clear_cache();
    
    // 列出已安装的插件
    std::vector<std::string> list_installed() const;
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace plugins
} // namespace hunterclaw
