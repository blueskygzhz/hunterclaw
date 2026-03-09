#pragma once

#include "hunterclaw/memory/plugin.h"
#include <string>
#include <map>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace hunterclaw {
namespace memory {

/**
 * Memory Provider 注册表 (100% 对齐 OpenClaw)
 * 
 * 单例模式，管理所有已注册的 Memory Provider。
 * 插件在初始化时注册 Provider，Agent 在运行时从注册表获取。
 * 
 * 使用示例:
 * ```cpp
 * // 注册 Provider (插件中)
 * MemoryProviderRegistry::instance().register_provider("sqlite", provider);
 * 
 * // 获取 Provider (Agent 中)
 * auto provider = MemoryProviderRegistry::instance().get("sqlite");
 * ```
 */
class MemoryProviderRegistry {
public:
    /**
     * 获取单例实例
     */
    static MemoryProviderRegistry& instance();
    
    /**
     * 注册 Memory Provider
     * @param id Provider ID (唯一标识)
     * @param provider Provider 实例
     */
    void register_provider(
        const std::string& id,
        std::shared_ptr<MemoryProvider> provider
    );
    
    /**
     * 注销 Memory Provider
     * @param id Provider ID
     */
    void unregister_provider(const std::string& id);
    
    /**
     * 获取 Memory Provider
     * @param id Provider ID
     * @return Provider 实例 (未找到返回 nullptr)
     */
    std::shared_ptr<MemoryProvider> get(const std::string& id) const;
    
    /**
     * 列出所有已注册的 Provider ID
     */
    std::vector<std::string> list() const;
    
    /**
     * 检查 Provider 是否存在
     */
    bool has(const std::string& id) const;
    
    /**
     * 清空注册表 (仅用于测试)
     */
    void clear();

private:
    MemoryProviderRegistry() = default;
    ~MemoryProviderRegistry() = default;
    
    // 禁止拷贝和赋值
    MemoryProviderRegistry(const MemoryProviderRegistry&) = delete;
    MemoryProviderRegistry& operator=(const MemoryProviderRegistry&) = delete;
    
    std::map<std::string, std::shared_ptr<MemoryProvider>> providers_;
    mutable std::shared_mutex mutex_;
};

/**
 * 自动注册宏 (用于静态注册内置 Provider)
 * 
 * 使用示例:
 * ```cpp
 * REGISTER_MEMORY_PROVIDER("sqlite", SQLiteMemoryProvider)
 * ```
 */
#define REGISTER_MEMORY_PROVIDER(id, provider_class) \
    static struct MemoryProviderRegistrar_##provider_class { \
        MemoryProviderRegistrar_##provider_class() { \
            auto provider = std::make_shared<provider_class>(); \
            hunterclaw::memory::MemoryProviderRegistry::instance().register_provider(id, provider); \
        } \
    } _registrar_##provider_class;

} // namespace memory
} // namespace hunterclaw
