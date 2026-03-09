#include "hunterclaw/channels/feishu_channel.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::channels {

core::Result<bool> FeishuChannelAdapter::reply_message(const std::string& message_id, const std::string& content) {
    static auto logger = spdlog::get("feishu");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("feishu");
    }
    logger->info("FeishuChannelAdapter::reply_message: {} -> {}", message_id, content);
    
    core::Result<bool> result;
    result.value = true;
    return result;
}

core::Result<bool> FeishuChannelAdapter::add_reaction(const std::string& message_id, const std::string& emoji) {
    static auto logger = spdlog::get("feishu");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("feishu");
    }
    logger->info("FeishuChannelAdapter::add_reaction: {} -> {}", message_id, emoji);
    
    core::Result<bool> result;
    result.value = true;
    return result;
}

} // namespace hunterclaw::channels
