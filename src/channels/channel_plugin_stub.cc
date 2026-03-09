#include "hunterclaw/channels/channel.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::channels {

core::Result<bool> ChannelPlugin::init(const plugins::PluginContext&) {
    static auto logger = spdlog::get("channels");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("channels");
    }
    logger->info("ChannelPlugin::init called");
    
    core::Result<bool> result;
    result.value = true;
    return result;
}

core::Result<bool> ChannelPlugin::shutdown() {
    static auto logger = spdlog::get("channels");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("channels");
    }
    logger->info("ChannelPlugin::shutdown called");
    
    core::Result<bool> result;
    result.value = true;
    return result;
}

core::Result<bool> ChannelPlugin::on_load() {
    static auto logger = spdlog::get("channels");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("channels");
    }
    logger->info("ChannelPlugin::on_load called");
    
    core::Result<bool> result;
    result.value = true;
    return result;
}

core::Result<bool> ChannelPlugin::on_unload() {
    static auto logger = spdlog::get("channels");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("channels");
    }
    logger->info("ChannelPlugin::on_unload called");
    
    core::Result<bool> result;
    result.value = true;
    return result;
}

} // namespace hunterclaw::channels
