#include "hunterclaw/channels/plugin.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::channels {

namespace {

auto get_logger() {
    static auto logger = []() {
        auto log = spdlog::get("channels");
        if (!log) {
            log = spdlog::stdout_logger_mt("channels");
        }
        return log;
    }();
    return logger;
}

} // namespace

core::Result<bool> ChannelPluginBase::init(const plugins::PluginContext&) {
    get_logger()->info("ChannelPlugin::init called");
    // Register this plugin's descriptor into the global registry
    if (plugin_descriptor_) {
        ChannelPluginRegistry::instance().register_plugin(*plugin_descriptor_, 0);
    }
    return core::Result<bool>::Ok(true);
}

core::Result<bool> ChannelPluginBase::shutdown() {
    get_logger()->info("ChannelPlugin::shutdown called");
    if (plugin_descriptor_) {
        ChannelPluginRegistry::instance().unregister_plugin(plugin_descriptor_->id);
    }
    return core::Result<bool>::Ok(true);
}

core::Result<bool> ChannelPluginBase::on_load() {
    get_logger()->info("ChannelPlugin::on_load called");
    // Build and cache the plugin descriptor
    auto desc = describe();
    plugin_descriptor_ = std::make_shared<ChannelPlugin>(std::move(desc));
    return core::Result<bool>::Ok(true);
}

core::Result<bool> ChannelPluginBase::on_unload() {
    get_logger()->info("ChannelPlugin::on_unload called");
    plugin_descriptor_.reset();
    return core::Result<bool>::Ok(true);
}

} // namespace hunterclaw::channels
