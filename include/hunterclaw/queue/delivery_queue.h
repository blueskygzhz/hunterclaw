#ifndef HUNTERCLAW_QUEUE_DELIVERY_QUEUE_H
#define HUNTERCLAW_QUEUE_DELIVERY_QUEUE_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include "hunterclaw/core/result.h"
#include <mutex>

namespace hunterclaw {
namespace queue {

// 队列模式 (对齐 OpenClaw QueueMode)
enum class QueueMode {
    Steer,         // 立即注入当前运行
    Followup,      // 等待当前结束后执行
    Collect,       // 合并多条消息为单次执行 (默认)
    SteerBacklog,  // 注入并保留为后续执行
    Interrupt      // 中止当前运行
};

// 溢出策略 (对齐 OpenClaw QueueDropPolicy)
enum class DropPolicy {
    Old,        // 丢弃旧消息
    New,        // 拒绝新消息
    Summarize   // 生成摘要保留
};

// 队列配置
struct QueueSettings {
    QueueMode mode = QueueMode::Collect;
    int debounce_ms = 1000;    // 防抖延迟 (默认1s)
    int cap = 20;              // 队列容量 (默认20)
    DropPolicy drop_policy = DropPolicy::Old;
};

// 消息Payload (对齐 OpenClaw ReplyPayload)
struct MessagePayload {
    std::string text;
    std::vector<std::string> media_urls;
    std::optional<std::string> markdown;
    // Channel-specific rich payload data (对齐 OpenClaw ReplyPayload.channelData)
    std::optional<nlohmann::json> channel_data;
};

// 投递目标
struct DeliveryTarget {
    std::string channel;              // 频道 (feishu, slack, etc.)
    std::string to;                   // 目标ID (群ID, 用户ID)
    std::optional<std::string> account_id;
    std::optional<std::string> thread_id;  // 线程ID
    std::optional<std::string> reply_to_id;
    bool best_effort = false;         // 对齐 OpenClaw QueuedDelivery.bestEffort
    bool gif_playback = false;        // 对齐 OpenClaw QueuedDelivery.gifPlayback
    bool silent = false;              // 对齐 OpenClaw QueuedDelivery.silent
};

// 队列投递条目 (对齐 OpenClaw QueuedDelivery)
struct QueuedDelivery {
    std::string id;                    // UUID
    DeliveryTarget target;
    std::vector<MessagePayload> payloads;
    
    uint64_t enqueued_at;              // 入队时间戳 (ms)
    int retry_count = 0;               // 重试次数
    std::optional<uint64_t> last_attempt_at;
    std::optional<std::string> last_error;
    
    bool best_effort = false;          // 尽力投递 (失败不报错)
    bool silent = false;               // 静默发送
};

// 投递回调
using DeliveryCallback = std::function<core::Result<bool>(const QueuedDelivery&)>;

// 恢复摘要
struct RecoverySummary {
    int recovered = 0;
    int failed = 0;
    int skipped_max_retries = 0;
    int deferred_backoff = 0;
};

// 投递队列接口 (对齐 OpenClaw delivery-queue.ts)
class DeliveryQueue {
public:
    using Ptr = std::shared_ptr<DeliveryQueue>;
    
    virtual ~DeliveryQueue() = default;
    
    // 入队投递
    virtual core::Result<std::string> enqueue(
        const DeliveryTarget& target,
        const std::vector<MessagePayload>& payloads
    ) = 0;
    
    // 确认投递成功 (从队列移除)
    virtual core::Result<bool> ack(const std::string& delivery_id) = 0;
    
    // 标记投递失败 (更新重试计数)
    virtual core::Result<bool> fail(
        const std::string& delivery_id,
        const std::string& error
    ) = 0;
    
    // 加载待处理投递 (启动时恢复)
    virtual core::Result<std::vector<QueuedDelivery>> load_pending() = 0;
    
    // 执行恢复 (自动重试)
    virtual core::Result<RecoverySummary> recover(DeliveryCallback callback) = 0;
    
    // 工厂方法
    static Ptr create(const std::string& queue_dir);
};

// 默认实现 (文件持久化)
class FileDeliveryQueue : public DeliveryQueue {
public:
    explicit FileDeliveryQueue(const std::string& queue_dir);
    
    core::Result<std::string> enqueue(
        const DeliveryTarget& target,
        const std::vector<MessagePayload>& payloads
    ) override;
    
    core::Result<bool> ack(const std::string& delivery_id) override;
    
    core::Result<bool> fail(
        const std::string& delivery_id,
        const std::string& error
    ) override;
    
    core::Result<std::vector<QueuedDelivery>> load_pending() override;
    
    core::Result<RecoverySummary> recover(DeliveryCallback callback) override;
    
private:
    std::string queue_dir_;
    std::string failed_dir_;
    
    static constexpr int MAX_RETRIES = 5;
    static constexpr int BACKOFF_MS[] = {
        5000,      // retry 1: 5s
        25000,     // retry 2: 25s
        120000,    // retry 3: 2m
        600000     // retry 4: 10m
    };
    
    // 辅助方法
    core::Result<bool> ensure_dirs();
    std::string get_queue_file_path(const std::string& delivery_id) const;
    std::string get_delivered_file_path(const std::string& delivery_id) const;
    std::string get_failed_file_path(const std::string& delivery_id) const;
    core::Result<bool> write_delivery(const QueuedDelivery& delivery);
    core::Result<QueuedDelivery> read_delivery(const std::string& file_path);
    // Aligned with openclaw moveToFailed: atomic rename to failed/ subdirectory.
    core::Result<bool> move_to_failed(const std::string& delivery_id);
    bool should_retry(const QueuedDelivery& delivery) const;
    int get_backoff_ms(int retry_count) const;
    bool is_permanent_delivery_error(const std::string& error) const;
};

// Followup 队列 (内存队列,用于消息合并)
struct FollowupRun {
    std::string prompt;
    std::optional<std::string> message_id;
    std::optional<std::string> summary_line;
    uint64_t enqueued_at;
    
    // 路由信息
    std::optional<std::string> originating_channel;
    std::optional<std::string> originating_to;
    std::optional<std::string> originating_account_id;
    std::optional<std::string> originating_thread_id;
};

// Followup 队列 (按 sessionKey 分组)
class FollowupQueue {
public:
    using Ptr = std::shared_ptr<FollowupQueue>;
    
    // 入队 followup
    void enqueue(const std::string& session_key, const FollowupRun& run);
    
    // 出队所有 followup (合并)
    std::vector<FollowupRun> dequeue_all(const std::string& session_key);
    
    // 获取队列大小
    size_t size(const std::string& session_key) const;
    
    // 清空队列
    void clear(const std::string& session_key);
    
    static Ptr create(const QueueSettings& settings);
    
private:
    QueueSettings settings_;
    std::unordered_map<std::string, std::vector<FollowupRun>> queues_;
    mutable std::mutex mutex_;
};

} // namespace queue
} // namespace hunterclaw

#endif // HUNTERCLAW_QUEUE_DELIVERY_QUEUE_H
