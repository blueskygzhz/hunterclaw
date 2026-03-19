#pragma once

#include "hunterclaw/queue/delivery_queue.h"
#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <atomic>

namespace hunterclaw {
namespace outbound {

// ============================================================================
// OutboundDeliveryResult
// Re-exported from channels::OutboundDeliveryResult (aligned with openclaw)
// ============================================================================
using OutboundDeliveryResult = channels::OutboundDeliveryResult;

// ============================================================================
// NormalizedOutboundPayload (对齐 openclaw NormalizedOutboundPayload)
// Passed to onPayload / onError callbacks.
// ============================================================================
struct NormalizedOutboundPayload {
    std::string text;
    std::vector<std::string> media_urls;
    nlohmann::json channel_data;  // channel-specific rich payload data
};

// ============================================================================
// DeliverOutboundPayloadsParams (对齐 openclaw DeliverOutboundPayloadsParams)
// ============================================================================
struct DeliverOutboundPayloadsParams {
    std::string channel;
    std::string to;
    std::optional<std::string> account_id;
    std::vector<queue::MessagePayload> payloads;
    std::optional<std::string> reply_to_id;
    std::optional<std::string> thread_id;
    bool best_effort = false;
    bool silent = false;
    bool gif_playback = false;

    // Abort signal (对齐 openclaw abortSignal)
    std::shared_ptr<std::atomic<bool>> abort_signal;

    // Callbacks (对齐 openclaw onError / onPayload)
    std::function<void(const std::string& error, const NormalizedOutboundPayload&)> on_error;
    std::function<void(const NormalizedOutboundPayload&)> on_payload;

    // Write-ahead queue (对齐 openclaw skipQueue)
    // skipQueue=true: 跳过入队 (崩溃恢复路径使用，避免重复入队)
    bool skip_queue = false;

    // Delivery queue instance (required unless skip_queue=true)
    std::shared_ptr<queue::DeliveryQueue> delivery_queue;
};

// ============================================================================
// deliverOutboundPayloads (对齐 openclaw deliverOutboundPayloads)
//
// 完整的 outbound 投递入口:
//   1. Write-ahead: enqueueDelivery (skip_queue=false 时)
//   2. deliverOutboundPayloadsCore: 通过 ChannelOutboundAdapter 实际发送
//   3. 成功: ackDelivery; 失败: failDelivery
//   4. AbortError: 视为成功 ack (对齐 openclaw)
// ============================================================================
core::Result<std::vector<OutboundDeliveryResult>> deliver_outbound_payloads(
    const DeliverOutboundPayloadsParams& params
);

// ============================================================================
// deliverOutboundPayloadsCore (对齐 openclaw deliverOutboundPayloadsCore)
//
// 纯发送逻辑 (不涉及队列):
//   - 加载 ChannelOutboundAdapter (createChannelHandler)
//   - normalizePayloadsForChannelDelivery
//   - 逐 payload:
//     - throwIfAborted (abortSignal check)
//     - sendPayload (channelData path, if adapter supports it)
//     - sendTextChunks (text-only, with chunker support)
//     - sendMedia (per mediaUrl)
//     - supportsMedia=false → fallback to sendText
//   - bestEffort: per-payload errors caught, passed to onError, don't throw
// ============================================================================
core::Result<std::vector<OutboundDeliveryResult>> deliver_outbound_payloads_core(
    const DeliverOutboundPayloadsParams& params
);

} // namespace outbound
} // namespace hunterclaw
