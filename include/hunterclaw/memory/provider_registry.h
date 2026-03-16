#pragma once

#include "hunterclaw/memory/provider.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <functional>

namespace hunterclaw {
namespace memory {

// 🎯 100% 对齐 OpenClaw: MemoryProviderRegistry
// 管理所有 Memory Provider 的注册、发现和生命周期

class MemoryProviderRegistry {
public:
    // 获取全局单例
    static MemoryProviderRegistry& instance();
    
    // ========================================
    // Provider 注册
    // ========================================
    
    // 注册 Provider
    core::Result<bool> register_provider(
        const std::string& provider_id,
        std::shared_ptr<MemoryProvider> provider
    );
    
    // 注销 Provider
    core::Result<bool> unregister_provider(const std::string& provider_id);
    
    // 检查 Provider 是否已注册
    bool has_provider(const std::string& provider_id) const;
    
    // ========================================
    // Provider 获取
    // ========================================
    
    // 获取 Provider (by ID)
    core::Result<std::shared_ptr<MemoryProvider>> get_provider(
        const std::string& provider_id
    ) const;
    
    // 获取默认 Provider
    core::Result<std::shared_ptr<MemoryProvider>> get_default_provider() const;
    
    // 设置默认 Provider
    void set_default_provider(const std::string& provider_id);
    
    // ========================================
    // Provider 发现
    // ========================================
    
    // 列出所有已注册的 Provider
    std::vector<std::string> list_providers() const;
    
    // 获取 Provider 统计信息
    struct ProviderInfo {
        std::string id;
        std::string name;
        std::string version;
        bool is_default;
        int64_t memory_count;
        int64_t total_size;
    };
    
    std::vector<ProviderInfo> get_all_provider_info() const;
    
    // ========================================
    // Provider Factory 注册
    // ========================================
    
    // Provider 工厂函数类型
    using ProviderFactory = std::function<
        core::Result<std::shared_ptr<MemoryProvider>>(
            const std::map<std::string, std::string>& config
        )
    >;
    
    // 注册 Provider 工厂 (用于延迟创建)
    void register_factory(
        const std::string& provider_type,
        ProviderFactory factory
    );
    
    // 从配置创建并注册 Provider
    core::Result<std::string> create_and_register_provider(
        const std::string& provider_type,
        const std::string& provider_id,
        const std::map<std::string, std::string>& config
    );
    
    // ========================================
    // Session-Memory 辅助
    // ========================================
    
    // 创建 Session-scoped Provider
    core::Result<std::shared_ptr<MemoryProvider>> create_session_provider(
        const std::string& underlying_provider_id,
        const SessionMemoryContext& context
    );
    
    // ========================================
    // 生命周期管理
    // ========================================
    
    // 清理所有 Provider (关闭时调用)
    void shutdown_all();
    
private:
    MemoryProviderRegistry() = default;
    ~MemoryProviderRegistry() = default;
    
    // 禁止复制和移动
    MemoryProviderRegistry(const MemoryProviderRegistry&) = delete;
    MemoryProviderRegistry& operator=(const MemoryProviderRegistry&) = delete;
    
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<MemoryProvider>> providers_;
    std::map<std::string, ProviderFactory> factories_;
    std::string default_provider_id_;
};

// ========================================
// 🎯 对齐 OpenClaw: 自动注册辅助宏
// ========================================

// 自动注册 Provider 的辅助宏
#define HUNTERCLAW_REGISTER_MEMORY_PROVIDER(provider_id, provider_type, ...) \
    namespace { \
        struct AutoRegister_##provider_type { \
            AutoRegister_##provider_type() { \
                auto provider = std::make_shared<provider_type>(__VA_ARGS__); \
                hunterclaw::memory::MemoryProviderRegistry::instance().register_provider( \
                    provider_id, provider \
                ); \
            } \
        }; \
        static AutoRegister_##provider_type auto_register_##provider_type; \
    }

// 自动注册 Provider 工厂的辅助宏
#define HUNTERCLAW_REGISTER_MEMORY_FACTORY(provider_type, factory_fn) \
    namespace { \
        struct AutoRegisterFactory_##provider_type { \
            AutoRegisterFactory_##provider_type() { \
                hunterclaw::memory::MemoryProviderRegistry::instance().register_factory( \
                    #provider_type, factory_fn \
                ); \
            } \
        }; \
        static AutoRegisterFactory_##provider_type auto_register_factory_##provider_type; \
    }

} // namespace memory
} // namespace hunterclaw
