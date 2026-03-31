#pragma once

#include "hunterclaw/cron/types.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/manager.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/outbound/deliver.h"
#include "hunterclaw/queue/delivery_queue.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace hunterclaw {
namespace cron {

/**
 * DeliveryService (对齐 OpenClaw dispatchCronDelivery)
 *
 * 负责将 Cron Job 结果投递到外部系统:
 * - Announce: 通过 ChannelOutboundAdapter 发送消息 (对齐 OpenClaw deliverViaDirect)
 * - Webhook: 调用 HTTP Webhook
 */
class DeliveryService {
public:
    struct Dependencies {
        // ChannelManager (可选，用于状态查询)
        std::shared_ptr<channels::ChannelManager> channel_manager;
        // 全局 Config (用于构建 ChannelOutboundContext)
        core::Config cfg;
        // Write-ahead delivery queue (对齐 OpenClaw skipQueue=false 路径)
        std::shared_ptr<queue::DeliveryQueue> delivery_queue;
    };

    explicit DeliveryService(Dependencies deps);
    ~DeliveryService();

    /**
     * 投递 Cron Job 结果
     *
     * @param delivery 投递配置
     * @param job      任务信息
     * @param result_text 执行结果文本
     * @return 成功返回 true, 失败返回 error
     */
    core::Result<bool> deliver(
        const Delivery& delivery,
        const Job& job,
        const std::string& result_text
    );

private:
    Dependencies deps_;

    // Announce delivery — 通过 ChannelOutboundAdapter 发送 (对齐 OpenClaw deliverViaDirect)
    core::Result<bool> deliver_announce(
        const Delivery& delivery,
        const Job& job,
        const std::string& result_text
    );

    // Webhook delivery (HTTP POST)
    core::Result<bool> deliver_webhook(
        const Delivery& delivery,
        const Job& job,
        const std::string& result_text
    );

    // SSRF Protection (对齐 OpenClaw)
    bool is_safe_webhook_url(const std::string& url);

    // HTTP Client
    core::Result<std::string> send_webhook_post(
        const std::string& url,
        const nlohmann::json& payload
    );

    // Helpers
    std::string format_announce_message(
        const Job& job,
        const std::string& result_text
    );

    std::string schedule_kind_to_string(ScheduleKind kind);
};

} // namespace cron
} // namespace hunterclaw
