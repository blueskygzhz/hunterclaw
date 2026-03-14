#pragma once

#include "hunterclaw/channels/channel.h"
#include "hunterclaw/plugins/plugin.h"
#include <memory>
#include <map>
#include <string>
#include <functional>

namespace hunterclaw {
namespace channels {

/// 渠道插件工厂 - 用于动态注册和创建渠道适配器
class ChannelPluginRegistry {
public:
    using AdapterFactory = std::function<std::shared_ptr<ChannelAdapter>()>;
    
    /// 获取全局单例
    static ChannelPluginRegistry& instance() {
        static ChannelPluginRegistry registry;
        return registry;
    }
    
    /// 注册渠道适配器工厂
    bool register_adapter(const std::string& channel_id, AdapterFactory factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (factories_.find(channel_id) != factories_.end()) {
            return false; // 已注册
        }
        
        factories_[channel_id] = factory;
        return true;
    }
    
    /// 创建渠道适配器实例
    std::shared_ptr<ChannelAdapter> create_adapter(const std::string& channel_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = factories_.find(channel_id);
        if (it == factories_.end()) {
            return nullptr;
        }
        
        return it->second();
    }
    
    /// 获取所有已注册的渠道 ID
    std::vector<std::string> get_registered_channels() const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<std::string> channels;
        for (const auto& [id, _] : factories_) {
            channels.push_back(id);
        }
        return channels;
    }
    
    /// 检查渠道是否已注册
    bool has_channel(const std::string& channel_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return factories_.find(channel_id) != factories_.end();
    }
    
    /// 取消注册渠道
    bool unregister_adapter(const std::string& channel_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return factories_.erase(channel_id) > 0;
    }

private:
    ChannelPluginRegistry() = default;
    ~ChannelPluginRegistry() = default;
    
    ChannelPluginRegistry(const ChannelPluginRegistry&) = delete;
    ChannelPluginRegistry& operator=(const ChannelPluginRegistry&) = delete;
    
    mutable std::mutex mutex_;
    std::map<std::string, AdapterFactory> factories_;
};

/// 渠道适配器自动注册器 (RAII)
template<typename AdapterType>
class ChannelAdapterRegistrar {
public:
    explicit ChannelAdapterRegistrar(const std::string& channel_id) {
        ChannelPluginRegistry::instance().register_adapter(
            channel_id,
            []() -> std::shared_ptr<ChannelAdapter> {
                return std::make_shared<AdapterType>();
            }
        );
    }
};

/// 便捷宏 - 自动注册渠道适配器
#define HUNTERCLAW_REGISTER_CHANNEL_ADAPTER(AdapterClass, channel_id) \
    namespace { \
        static ::hunterclaw::channels::ChannelAdapterRegistrar<AdapterClass> \
            __channel_registrar_##AdapterClass(channel_id); \
    }

} // namespace channels
} // namespace hunterclaw
