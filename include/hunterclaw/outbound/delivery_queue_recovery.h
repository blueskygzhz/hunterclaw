// ============================================================================
// delivery_queue_recovery.h
// 严格对齐 OpenClaw: infra/outbound/delivery-queue-recovery.ts
//
// 暴露 drainPendingDeliveries 供 channel plugin reconnect 使用。
// OC 调用链: WhatsApp monitor → plugin-sdk/infra-runtime.ts:drainPendingDeliveries
//           → infra/outbound/delivery-queue-recovery.ts:drainPendingDeliveries
//
// HC 对应: discord_channel.cc (reconnect) → drain_pending_deliveries(...)
// ============================================================================
#pragma once

#include "hunterclaw/queue/delivery_queue.h"
#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace hunterclaw {
namespace outbound {

// ============================================================================
// RecoverySummary (same fields as queue::RecoverySummary, redefined in outbound
// namespace to avoid cross-namespace coupling)
// ============================================================================
struct RecoverySummary {
    int recovered = 0;
    int failed = 0;
    int skipped_max_retries = 0;
    int deferred_backoff = 0;
};

// ============================================================================
// PendingDeliveryDrainDecision
// 对齐 OC: PendingDeliveryDrainDecision (delivery-queue-recovery.ts:213)
//
// selectEntry(entry) 的返回值。每个 pending entry 被传入 matcher，
// 由调用方决定：
//   match:         该 entry 是否属于本次 drain 的目标范围
//   bypass_backoff: 是否跳过退避等待直接重试
//                   (WhatsApp "No active listener" 错误时使用)
// ============================================================================
struct PendingDeliveryDrainDecision {
    bool match = false;
    bool bypass_backoff = false;
};

// ============================================================================
// DrainEntryInfo
// 传给 matcher 回调的 entry 摘要信息。
// 对齐 OC selectEntry 回调收到的完整 QueuedDelivery 字段子集。
// ============================================================================
struct DrainEntryInfo {
    std::string channel;
    std::string to;
    std::string account_id;    // 对齐 OC: entry.accountId
    std::string last_error;    // 对齐 OC: entry.lastError (用于 bypassBackoff 判断)
    int retry_count = 0;
};

// ============================================================================
// DrainDeliverFn
// 投递回调签名。对齐 OC DeliverFn。
// skip_queue=true: 跳过 write-ahead 入队（因为 entry 已在队列里）。
// ============================================================================
using DrainDeliverFn = std::function<bool(
    const std::string& channel,
    const std::string& to,
    const std::vector<queue::MessagePayload>& payloads,
    bool skip_queue)>;

// ============================================================================
// drain_pending_deliveries
// 严格对齐 OC: delivery-queue-recovery.ts:drainPendingDeliveries (L213-310)
//
// 条件性排空 pending delivery queue 中匹配的条目。
// 典型场景：channel reconnect 后调用，只重试属于该 channel+account 的积压。
//
// drain_key:   并发锁 key（如 "discord:bot1"），防止同一 key 多个并发 drain
// log_label:   日志标签（如 "Discord reconnect drain"）
// state_dir:   delivery-queue 目录路径
// matcher:     selectEntry 回调，per-entry 决策
// deliver_fn:  实际投递回调
//
// 返回 RecoverySummary：recovered / failed / deferred_backoff / skipped_max_retries
// ============================================================================
RecoverySummary drain_pending_deliveries(
    const std::string& drain_key,
    const std::string& log_label,
    const std::string& state_dir,
    std::function<PendingDeliveryDrainDecision(const DrainEntryInfo& entry)> matcher,
    DrainDeliverFn deliver_fn);

} // namespace outbound
} // namespace hunterclaw
