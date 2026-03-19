#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/channels/registry.h"
#include <algorithm>

namespace hunterclaw {
namespace channels {

ChannelPluginRegistry& ChannelPluginRegistry::instance() {
    static ChannelPluginRegistry registry;
    return registry;
}

void ChannelPluginRegistry::register_plugin(const ChannelPlugin& plugin, int priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    plugins_[plugin.id] = PluginEntry{plugin, priority};
}

void ChannelPluginRegistry::unregister_plugin(ChannelId channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    plugins_.erase(channel_id);
}

std::optional<ChannelPlugin> ChannelPluginRegistry::get_plugin(ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = plugins_.find(channel_id);
    if (it == plugins_.end()) {
        return std::nullopt;
    }
    return it->second.plugin;
}

std::vector<ChannelPlugin> ChannelPluginRegistry::list_plugins() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChannelPlugin> result;
    result.reserve(plugins_.size());
    for (const auto& [id, entry] : plugins_) {
        result.push_back(entry.plugin);
    }
    return result;
}

std::vector<ChannelPlugin> ChannelPluginRegistry::list_plugins_sorted() const {
    std::vector<ChannelPlugin> plugins;
    std::map<ChannelId, int> priorities;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        plugins.reserve(plugins_.size());
        for (const auto& [id, entry] : plugins_) {
            plugins.push_back(entry.plugin);
            priorities[id] = entry.priority;
        }
    }
    // Sort by priority descending, then by CHAT_CHANNEL_ORDER index, then by id ascending
    // (对齐 OpenClaw listChannelDocks sort logic)
    std::sort(plugins.begin(), plugins.end(), [&priorities](const ChannelPlugin& a, const ChannelPlugin& b) {
        int pa = priorities.count(a.id) ? priorities.at(a.id) : 0;
        int pb = priorities.count(b.id) ? priorities.at(b.id) : 0;
        if (pa != pb) return pa > pb;

        // Secondary: CHAT_CHANNEL_ORDER index
        auto order_index = [](const std::string& id) -> int {
            for (int i = 0; i < (int)CHAT_CHANNEL_ORDER.size(); ++i) {
                if (CHAT_CHANNEL_ORDER[i] == id) return i;
            }
            return 999;
        };
        int ia = order_index(a.id);
        int ib = order_index(b.id);
        if (ia != ib) return ia < ib;

        return a.id < b.id;
    });
    return plugins;
}

std::optional<ChannelGatewayAdapter> ChannelPluginRegistry::get_gateway(ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = plugins_.find(channel_id);
    if (it == plugins_.end()) {
        return std::nullopt;
    }
    return it->second.plugin.gateway;
}

} // namespace channels
} // namespace hunterclaw
