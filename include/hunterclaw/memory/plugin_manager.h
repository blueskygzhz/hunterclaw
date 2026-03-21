#pragma once

#include "hunterclaw/memory/plugin.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>

namespace hunterclaw {
namespace memory {

/**
 * 🎯 100% 对齐 OpenClaw: Memory Plugin Manager
 * 
 * 负责 Memory Plugin 的发现、加载、管理和卸载。
 * 
 * OpenClaw 对齐功能:
 * - ✅ 插件发现 (扫描 plugins/ 目录)
 * - ✅ 动态加载 (dlopen + manifest 解析)
 * - ✅ Provider 注册 (自动注册到 Registry)
 * - ✅ 热重载 (支持插件更新)
 * - ✅ 生命周期管理 (init → running → shutdown)
 * 
 * 使用示例:
 * 
 * // 1. 初始化 Plugin Manager
 * auto& pm = MemoryPluginManager::instance();
 * pm.set_plugins_directory("/usr/lib/hunterclaw/plugins");
 * 
 * // 2. 发现和加载所有插件
 * auto result = pm.discover_and_load_all();
 * 
 * // 3. 获取特定插件
 * auto plugin = pm.get_plugin("sqlite-memory");
 * auto provider = plugin->get_provider();
 * 
 * // 4. Agent 自动使用
 * // config.yaml:
 * // memory:
 * //   provider: sqlite-memory
 * //   config:
 * //     db_path: ./memory.db
 */
class MemoryPluginManager {
public:
    // ========================================
    // 🎯 单例模式
    // ========================================
    
    static MemoryPluginManager& instance();
    
    // 禁止拷贝和移动
    MemoryPluginManager(const MemoryPluginManager&) = delete;
    MemoryPluginManager& operator=(const MemoryPluginManager&) = delete;
    
    // ========================================
    // 🎯 OpenClaw 核心 API: 插件管理
    // ========================================
    
    /**
     * 设置插件目录
     * 
     * OpenClaw 默认路径:
     * - ~/.openclaw/plugins
     * - /usr/local/lib/openclaw/plugins
     * 
     * HunterClaw 默认路径:
     * - ~/.hunterclaw/plugins
     * - /usr/lib/hunterclaw/plugins
     * - /usr/local/lib/hunterclaw/plugins
     */
    void set_plugins_directory(const std::string& dir);
    void add_plugins_directory(const std::string& dir);
    
    /**
     * 发现所有插件 (扫描目录)
     * 
     * 查找规则:
     * - *.so + *.plugin.json (manifest)
     * - manifest 必须包含 "type": "memory"
     * 
     * OpenClaw: async discoverPlugins(): Promise<PluginManifest[]>
     */
    core::Result<std::vector<std::string>> discover_plugins();
    
    /**
     * 加载插件 (from manifest)
     * 
     * 流程:
     * 1. 读取 manifest (.plugin.json)
     * 2. 验证 manifest (id, type, main)
     * 3. dlopen 加载 .so
     * 4. 调用 create_plugin()
     * 5. 调用 plugin->init()
     * 6. 注册到 Provider Registry
     * 
     * OpenClaw: async loadPlugin(manifest: PluginManifest): Promise<Plugin>
     */
    core::Result<std::shared_ptr<MemoryPlugin>> load_plugin(
        const std::string& manifest_path
    );
    
    /**
     * 加载插件 (by ID)
     * 
     * 从已发现的插件中加载指定 ID 的插件。
     */
    core::Result<std::shared_ptr<MemoryPlugin>> load_plugin_by_id(
        const std::string& plugin_id
    );
    
    /**
     * 卸载插件
     * 
     * 流程:
     * 1. 调用 plugin->shutdown()
     * 2. 从 Provider Registry 移除
     * 3. dlclose 卸载 .so
     * 
     * OpenClaw: async unloadPlugin(id: string): Promise<void>
     */
    core::Result<bool> unload_plugin(const std::string& plugin_id);
    
    /**
     * 重载插件 (热更新)
     * 
     * 流程:
     * 1. unload_plugin(id)
     * 2. load_plugin(id)
     * 
     * OpenClaw: async reloadPlugin(id: string): Promise<void>
     */
    core::Result<bool> reload_plugin(const std::string& plugin_id);
    
    /**
     * 发现并加载所有插件 (便利函数)
     * 
     * = discover_plugins() + load_plugin() for all
     */
    core::Result<int> discover_and_load_all();
    
    // ========================================
    // 🎯 OpenClaw 扩展 API: 查询和管理
    // ========================================
    
    /**
     * 获取插件 (by ID)
     */
    std::shared_ptr<MemoryPlugin> get_plugin(const std::string& plugin_id) const;
    
    /**
     * 列出所有已加载的插件
     */
    std::vector<std::shared_ptr<MemoryPlugin>> list_plugins() const;
    
    /**
     * 检查插件是否已加载
     */
    bool is_loaded(const std::string& plugin_id) const;
    
    /**
     * 获取插件数量
     */
    size_t plugin_count() const;
    
    /**
     * 清空所有插件 (卸载)
     */
    core::Result<bool> unload_all();
    
    // ========================================
    // 🎯 OpenClaw 扩展: Manifest 解析
    // ========================================
    
    /**
     * Plugin Manifest (插件清单)
     * 
     * OpenClaw 示例:
     * {
     *   "id": "sqlite-memory",
     *   "name": "SQLite Memory",
     *   "version": "1.0.0",
     *   "type": "memory",
     *   "author": "HunterClaw Team",
     *   "description": "Local SQLite-based memory storage",
     *   "main": "libsqlite_memory_plugin.so",
     *   "config": {
     *     "db_path": {"type": "string", "default": "./memory.db"},
     *     "embedding_provider": {"type": "string", "default": "openai"}
     *   }
     * }
     */
    struct PluginManifest {
        std::string id;
        std::string name;
        std::string version;
        std::string type;  // 必须是 "memory"
        std::string author;
        std::string description;
        std::string main;  // .so 文件名
        std::map<std::string, std::string> config;  // 默认配置
        
        // 可选字段
        std::string license;
        std::string homepage;
        std::vector<std::string> keywords;
        std::map<std::string, std::string> dependencies;
    };
    
    /**
     * 解析 manifest 文件
     * 
     * OpenClaw: parseManifest(path: string): Promise<PluginManifest>
     */
    static core::Result<PluginManifest> parse_manifest(
        const std::string& manifest_path
    );
    
    /**
     * 验证 manifest
     * 
     * 检查:
     * - id 非空
     * - type == "memory"
     * - main 存在
     */
    static core::Result<bool> validate_manifest(const PluginManifest& manifest);
    
private:
    MemoryPluginManager() = default;
    ~MemoryPluginManager() = default;
    
    // 插件目录列表
    std::vector<std::string> plugin_directories_;
    
    // 已加载的插件 (plugin_id -> Plugin)
    std::map<std::string, std::shared_ptr<MemoryPlugin>> plugins_;
    
    // 已发现的插件清单 (plugin_id -> Manifest)
    std::map<std::string, PluginManifest> discovered_manifests_;
    
    // 动态库句柄 (plugin_id -> dlopen handle)
    std::map<std::string, void*> plugin_handles_;
    
    // 线程安全
    mutable std::mutex mutex_;
};

// ========================================
// 🎯 对齐 OpenClaw: Plugin 创建函数签名
// ========================================

/**
 * Plugin 创建函数类型
 * 
 * 每个 Memory Plugin .so 必须导出此函数:
 * 
 * extern "C" MemoryPlugin* create_plugin();
 * 
 * 示例:
 * 
 * // libsqlite_memory_plugin.so
 * extern "C" MemoryPlugin* create_plugin() {
 *     return new SQLiteMemoryPlugin();
 * }
 */
using CreatePluginFn = MemoryPlugin* (*)();

/**
 * Plugin 销毁函数类型 (可选)
 * 
 * extern "C" void destroy_plugin(MemoryPlugin* plugin);
 */
using DestroyPluginFn = void (*)(MemoryPlugin*);

} // namespace memory
} // namespace hunterclaw
