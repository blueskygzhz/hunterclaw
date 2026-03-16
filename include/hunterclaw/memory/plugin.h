#pragma once

#include "hunterclaw/memory/provider.h"  // 🎯 使用统一的 provider 定义
#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/core/result.h"

namespace hunterclaw {
namespace memory {

/**
 * 🎯 100% 对齐 OpenClaw: Memory Plugin 接口
 * 
 * Memory Plugin 是提供 MemoryProvider 实现的插件。
 * 插件系统允许第三方扩展为 Agent 提供长期记忆能力。
 * 
 * OpenClaw 官方 Memory Plugin 示例:
 * - MemOS Cloud Plugin (https://github.com/MemTensor/MemOS-Cloud-OpenClaw-Plugin)
 * - SQLite Memory Plugin (内置)
 * - Supabase Memory Plugin (社区)
 * 
 * 插件清单示例 (.plugin.json):
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
 * 
 * 使用示例:
 * 
 * // 1. 实现 MemoryPlugin
 * class SQLiteMemoryPlugin : public MemoryPlugin {
 * public:
 *     Result<bool> init(const PluginContext& ctx) override {
 *         // 从配置创建 provider
 *         std::string db_path = ctx.config["db_path"];
 *         provider_ = std::make_shared<SQLiteMemoryProvider>(db_path);
 *         
 *         // 注册到 Registry
 *         MemoryProviderRegistry::instance().register_provider(id(), provider_);
 *         return Ok(true);
 *     }
 *     
 *     std::shared_ptr<MemoryProvider> get_provider() override {
 *         return provider_;
 *     }
 *     
 *     std::string id() const override { return "sqlite-memory"; }
 *     std::string name() const override { return "SQLite Memory"; }
 *     std::string version() const override { return "1.0.0"; }
 *     
 * private:
 *     std::shared_ptr<MemoryProvider> provider_;
 * };
 * 
 * // 2. Agent 自动使用
 * // Agent 配置 (config.yaml):
 * // agents:
 * //   default:
 * //     memory:
 * //       enabled: true
 * //       provider: "sqlite-memory"  # 插件 ID
 * //       config:
 * //         contextWindow: 10
 * //         minScore: 0.7
 * 
 * // Agent 运行时:
 * auto agent = Agent::create(config);
 * agent->process_message("记住我最喜欢的颜色是蓝色");
 * // → 自动存储到 memory provider
 * 
 * agent->process_message("我喜欢什么颜色?");
 * // → 自动召回: "你最喜欢的颜色是蓝色"
 */
class MemoryPlugin : public plugins::Plugin {
public:
    virtual ~MemoryPlugin() = default;
    
    /**
     * 🎯 OpenClaw 核心 API: 获取 Memory Provider 实例
     * 
     * Plugin 必须在 init() 后返回可用的 Provider。
     * 
     * @return MemoryProvider 实例 (非空)
     */
    virtual std::shared_ptr<MemoryProvider> get_provider() = 0;
    
    /**
     * Plugin 类型 (固定为 "memory")
     */
    std::string type() const { return "memory"; }
    
    /**
     * 🎯 OpenClaw 扩展: 支持热重载
     * 
     * Memory Plugin 可以选择性支持热重载。
     * 如果支持，Plugin Manager 可以在运行时更新插件。
     */
    bool supports_hot_reload() const override { 
        return false;  // 默认不支持
    }
};

// ========================================
// 🎯 对齐 OpenClaw: Plugin 辅助工厂
// ========================================

/**
 * Memory Plugin 工厂接口
 * 
 * 用于动态创建 Memory Plugin 实例。
 * 
 * 示例:
 * 
 * class SQLiteMemoryPluginFactory : public MemoryPluginFactory {
 * public:
 *     Result<std::shared_ptr<MemoryPlugin>> create(
 *         const std::map<std::string, std::string>& config
 *     ) override {
 *         auto plugin = std::make_shared<SQLiteMemoryPlugin>();
 *         
 *         PluginContext ctx;
 *         ctx.config = config;
 *         
 *         auto result = plugin->init(ctx);
 *         if (!result) {
 *             return Err(result.error());
 *         }
 *         
 *         return Ok(plugin);
 *     }
 * };
 */
class MemoryPluginFactory {
public:
    virtual ~MemoryPluginFactory() = default;
    
    virtual core::Result<std::shared_ptr<MemoryPlugin>> create(
        const std::map<std::string, std::string>& config
    ) = 0;
};

} // namespace memory
} // namespace hunterclaw
