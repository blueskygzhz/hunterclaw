#pragma once

#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <functional>
#include <shared_mutex>

namespace hunterclaw {
namespace skills {

/**
 * @brief Skill Plugin 接口 (100% 对齐 OpenClaw Plugin 系统)
 * 
 * OpenClaw 对齐:
 * - src/plugins/manifest.ts - Plugin Manifest 定义
 * - src/plugins/loader.ts - Plugin 加载逻辑
 * - src/agents/skills/plugin-skills.ts - Plugin Skills 集成
 * 
 * 架构:
 * ```
 * SkillPluginManager → SkillPlugin → SkillProvider
 *        ↓                  ↓              ↓
 *  load_plugin()     init()       list_skills()
 *  discover()        shutdown()   get_skill()
 * ```
 */
class SkillPlugin {
public:
    virtual ~SkillPlugin() = default;
    
    /**
     * @brief 初始化插件
     * @param config 插件配置 (JSON 格式)
     * @return 成功返回 true
     * 
     * OpenClaw: plugin.init(config)
     */
    virtual core::Result<bool> init(const std::map<std::string, std::string>& config) = 0;
    
    /**
     * @brief 关闭插件
     * 
     * OpenClaw: plugin.shutdown()
     */
    virtual void shutdown() = 0;
    
    /**
     * @brief 获取插件 ID
     * 
     * OpenClaw: plugin.id
     */
    virtual std::string get_id() const = 0;
    
    /**
     * @brief 获取插件名称
     * 
     * OpenClaw: plugin.name
     */
    virtual std::string get_name() const = 0;
    
    /**
     * @brief 获取插件版本
     * 
     * OpenClaw: plugin.version
     */
    virtual std::string get_version() const = 0;
    
    /**
     * @brief 列出插件提供的所有 Skills
     * @return Skill 列表
     * 
     * OpenClaw: plugin.skills 字段 + loadSkillsFromDir()
     */
    virtual std::vector<Skill> list_skills() = 0;
    
    /**
     * @brief 获取单个 Skill
     * @param name Skill 名称
     * @return Skill (如果存在)
     * 
     * OpenClaw: skills.find(s => s.name === name)
     */
    virtual std::optional<Skill> get_skill(const std::string& name) = 0;
    
    /**
     * @brief 刷新 Skills (支持热重载)
     * @return 成功返回 true
     * 
     * OpenClaw: refreshSkills()
     */
    virtual core::Result<bool> refresh_skills() = 0;
};

/**
 * @brief Skill Plugin Manifest (100% 对齐 OpenClaw)
 * 
 * OpenClaw: src/plugins/manifest.ts
 * 
 * 示例:
 * ```json
 * {
 *   "id": "my-skill-plugin",
 *   "name": "My Skill Plugin",
 *   "version": "1.0.0",
 *   "kind": "skills",
 *   "description": "Custom skills plugin",
 *   "author": "John Doe",
 *   "homepage": "https://example.com",
 *   "skills": [
 *     "skills/my-skill",
 *     "skills/another-skill"
 *   ],
 *   "configSchema": {
 *     "type": "object",
 *     "properties": {
 *       "api_key": {"type": "string"},
 *       "endpoint": {"type": "string"}
 *     }
 *   },
 *   "enabled": true,
 *   "main": "libmy_skill_plugin.so"
 * }
 * ```
 */
struct SkillPluginManifest {
    std::string id;                     ///< 插件 ID (唯一)
    std::string name;                   ///< 插件名称
    std::string version;                ///< 插件版本 (semver)
    std::string kind;                   ///< 插件类型 (skills, tools, channels)
    std::string description;            ///< 描述
    std::string author;                 ///< 作者
    std::string homepage;               ///< 主页 URL
    
    std::vector<std::string> skills;    ///< Skill 路径列表 (相对于插件根目录)
    
    std::string config_schema;          ///< 配置 Schema (JSON Schema)
    bool enabled = true;                ///< 是否启用
    
    std::string main;                   ///< 入口文件 (.so 文件)
    
    fs::path root_dir;                  ///< 插件根目录 (解析时填充)
    std::string origin;                 ///< 来源 (bundled, workspace, user)
};

/**
 * @brief Skill Plugin 来源
 * 
 * OpenClaw: PluginOrigin
 */
enum class SkillPluginOrigin {
    Bundled,    ///< 系统内置插件 (~/.hunterclaw/plugins-bundled)
    User,       ///< 用户插件 (~/.hunterclaw/plugins)
    Workspace,  ///< 工作区插件 (<workspace>/.hunterclaw/plugins)
    Extensions  ///< OpenClaw 扩展 (~/.openclaw/extensions)
};

/**
 * @brief Skill Plugin 注册表记录
 * 
 * OpenClaw: PluginRegistryRecord
 */
struct SkillPluginRecord {
    SkillPluginManifest manifest;
    SkillPluginOrigin origin;
    std::shared_ptr<SkillPlugin> instance;  ///< 插件实例 (已加载)
    bool loaded = false;                    ///< 是否已加载
};

/**
 * @brief Skill Plugin Factory (C ABI 导出函数)
 * 
 * 每个插件 .so 文件必须导出:
 * 
 * extern "C" {
 *     SkillPlugin* create_skill_plugin();
 *     void destroy_skill_plugin(SkillPlugin* plugin);
 * }
 */
using CreateSkillPluginFn = SkillPlugin* (*)();
using DestroySkillPluginFn = void (*)(SkillPlugin*);

/**
 * @brief Skill Plugin 加载器
 * 
 * 负责:
 * - 扫描插件目录
 * - 解析 Manifest
 * - 加载 .so 文件
 * - 创建插件实例
 */
class SkillPluginLoader {
public:
    /**
     * @brief 发现插件
     * @param plugin_dir 插件目录
     * @param origin 插件来源
     * @return 发现的 Manifest 列表
     * 
     * OpenClaw: discoverPlugins()
     */
    static std::vector<SkillPluginManifest> discover_plugins(
        const fs::path& plugin_dir,
        SkillPluginOrigin origin
    );
    
    /**
     * @brief 加载插件
     * @param manifest 插件 Manifest
     * @return 插件实例
     * 
     * OpenClaw: loadPlugin()
     * 
     * 流程:
     * 1. 读取 manifest.json
     * 2. 验证 kind == "skills"
     * 3. dlopen manifest.main
     * 4. dlsym create_skill_plugin
     * 5. 调用 create_skill_plugin()
     * 6. 调用 plugin->init(config)
     * 7. 返回插件实例
     */
    static core::Result<std::shared_ptr<SkillPlugin>> load_plugin(
        const SkillPluginManifest& manifest
    );
    
    /**
     * @brief 卸载插件
     * @param plugin 插件实例
     * @param handle dlopen 句柄
     * 
     * OpenClaw: unloadPlugin()
     * 
     * 流程:
     * 1. plugin->shutdown()
     * 2. destroy_skill_plugin(plugin)
     * 3. dlclose(handle)
     */
    static void unload_plugin(
        std::shared_ptr<SkillPlugin> plugin,
        void* handle
    );
    
    /**
     * @brief 解析 Manifest JSON
     * @param manifest_path manifest.json 路径
     * @return Manifest
     * 
     * OpenClaw: parseManifest()
     */
    static core::Result<SkillPluginManifest> parse_manifest(
        const fs::path& manifest_path
    );
    
    /**
     * @brief 验证 Manifest
     * @param manifest Manifest
     * @return 验证结果
     * 
     * OpenClaw: validateManifest()
     */
    static core::Result<bool> validate_manifest(
        const SkillPluginManifest& manifest
    );
};

/**
 * @brief Skill Plugin Registry (插件注册表)
 * 
 * 管理所有已发现和已加载的插件
 * 
 * OpenClaw: PluginManifestRegistry
 */
class SkillPluginRegistry {
public:
    /**
     * @brief 注册插件
     * @param record 插件记录
     * 
     * OpenClaw: registry.register(record)
     */
    void register_plugin(const SkillPluginRecord& record);
    
    /**
     * @brief 注销插件
     * @param id 插件 ID
     * 
     * OpenClaw: registry.unregister(id)
     */
    void unregister_plugin(const std::string& id);
    
    /**
     * @brief 获取插件
     * @param id 插件 ID
     * @return 插件记录
     * 
     * OpenClaw: registry.get(id)
     */
    std::optional<SkillPluginRecord> get_plugin(const std::string& id) const;
    
    /**
     * @brief 列出所有插件
     * @return 插件记录列表
     * 
     * OpenClaw: registry.list()
     */
    std::vector<SkillPluginRecord> list_plugins() const;
    
    /**
     * @brief 列出已启用的插件
     * @return 插件记录列表
     * 
     * OpenClaw: registry.listEnabled()
     */
    std::vector<SkillPluginRecord> list_enabled_plugins() const;
    
    /**
     * @brief 清空注册表
     * 
     * OpenClaw: registry.clear()
     */
    void clear();

private:
    std::map<std::string, SkillPluginRecord> plugins_;
    mutable std::shared_mutex mutex_;
};

/**
 * @brief Skill Plugin Manager (插件管理器)
 * 
 * 统一管理所有 Skill Plugins
 * 
 * OpenClaw: 集成在 SkillManager 中
 */
class SkillPluginManager {
public:
    static std::shared_ptr<SkillPluginManager> create();
    
    ~SkillPluginManager();
    
    /**
     * @brief 加载所有插件
     * @param workspace_dir 工作区目录 (可选)
     * @return 成功返回 true
     * 
     * OpenClaw: loadPlugins()
     * 
     * 扫描顺序 (优先级从低到高):
     * 1. ~/.hunterclaw/plugins-bundled/ (Bundled)
     * 2. ~/.openclaw/extensions/     (Extensions)
     * 3. ~/.hunterclaw/plugins/        (User)
     * 4. <workspace>/.hunterclaw/plugins/ (Workspace)
     */
    core::Result<bool> load_plugins(const std::optional<std::string>& workspace_dir);
    
    /**
     * @brief 加载单个插件
     * @param plugin_id 插件 ID
     * @return 成功返回 true
     * 
     * OpenClaw: loadPlugin(id)
     */
    core::Result<bool> load_plugin(const std::string& plugin_id);
    
    /**
     * @brief 卸载插件
     * @param plugin_id 插件 ID
     * @return 成功返回 true
     * 
     * OpenClaw: unloadPlugin(id)
     */
    core::Result<bool> unload_plugin(const std::string& plugin_id);
    
    /**
     * @brief 重新加载插件
     * @param plugin_id 插件 ID
     * @return 成功返回 true
     * 
     * OpenClaw: reloadPlugin(id)
     */
    core::Result<bool> reload_plugin(const std::string& plugin_id);
    
    /**
     * @brief 获取插件注册表
     * @return 插件注册表
     * 
     * OpenClaw: getRegistry()
     */
    const SkillPluginRegistry& get_registry() const;
    
    /**
     * @brief 收集所有插件的 Skills
     * @return Skill 列表
     * 
     * OpenClaw: collectPluginSkills()
     */
    std::vector<Skill> collect_plugin_skills() const;
    
    /**
     * @brief 刷新所有插件的 Skills
     * @return 成功返回 true
     * 
     * OpenClaw: refreshPluginSkills()
     */
    core::Result<bool> refresh_plugin_skills();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    
    SkillPluginManager();
};

} // namespace skills
} // namespace hunterclaw
