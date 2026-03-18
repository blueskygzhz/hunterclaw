#include "hunterclaw/channels/channel.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::channels {

namespace {

// 获取 Channels logger
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

core::Result<bool> ChannelPlugin::init(const plugins::PluginContext&) {
    get_logger()->info("ChannelPlugin::init called");
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> ChannelPlugin::shutdown() {
    get_logger()->info("ChannelPlugin::shutdown called");
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> ChannelPlugin::on_load() {
    get_logger()->info("ChannelPlugin::on_load called");
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> ChannelPlugin::on_unload() {
    get_logger()->info("ChannelPlugin::on_unload called");
    
    return core::Result<bool>::Ok(true);
}

} // namespace hunterclaw::channels
