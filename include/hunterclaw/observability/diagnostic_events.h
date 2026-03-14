#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace observability {

// Use nlohmann::json for diagnostic events
using json = nlohmann::json;

// ============================================================================
// 完全对齐 OpenClaw - 13 种诊断事件
// 参考: openclaw-analysis/src/infra/diagnostic-events.ts
// ============================================================================

enum class DiagnosticEventType {
    // 1. LLM Token 使用
    MODEL_USAGE,              // model.usage
    
    // 2-4. Webhook 事件
    WEBHOOK_RECEIVED,         // webhook.received
    WEBHOOK_PROCESSED,        // webhook.processed
    WEBHOOK_ERROR,            // webhook.error
    
    // 5-6. 消息事件
    MESSAGE_QUEUED,           // message.queued
    MESSAGE_PROCESSED,        // message.processed
    
    // 7-8. Session 状态
    SESSION_STATE,            // session.state (idle/processing/waiting)
    SESSION_STUCK,            // session.stuck (超过2分钟无响应)
    
    // 9-10. Queue Lane 事件
    QUEUE_LANE_ENQUEUE,       // queue.lane.enqueue
    QUEUE_LANE_DEQUEUE,       // queue.lane.dequeue
    
    // 11. React Run 重试
    RUN_ATTEMPT,              // run.attempt
    
    // 12. 心跳事件
    DIAGNOSTIC_HEARTBEAT,     // diagnostic.heartbeat
    
    // 13. Tool Loop 检测
    TOOL_LOOP,                // tool.loop
    
    // 14-17. HTTP and Delivery 事件
    HTTP_REQUEST,             // http.request
    DELIVERY_SUCCESS,         // delivery.success
    DELIVERY_FAILURE,         // delivery.failure
    DELIVERY_RETRY            // delivery.retry
};

// 将枚举转为字符串 (用于日志和指标)
inline std::string event_type_to_string(DiagnosticEventType type) {
    switch (type) {
        case DiagnosticEventType::MODEL_USAGE:           return "model.usage";
        case DiagnosticEventType::WEBHOOK_RECEIVED:      return "webhook.received";
        case DiagnosticEventType::WEBHOOK_PROCESSED:     return "webhook.processed";
        case DiagnosticEventType::WEBHOOK_ERROR:         return "webhook.error";
        case DiagnosticEventType::MESSAGE_QUEUED:        return "message.queued";
        case DiagnosticEventType::MESSAGE_PROCESSED:     return "message.processed";
        case DiagnosticEventType::SESSION_STATE:         return "session.state";
        case DiagnosticEventType::SESSION_STUCK:         return "session.stuck";
        case DiagnosticEventType::QUEUE_LANE_ENQUEUE:    return "queue.lane.enqueue";
        case DiagnosticEventType::QUEUE_LANE_DEQUEUE:    return "queue.lane.dequeue";
        case DiagnosticEventType::RUN_ATTEMPT:           return "run.attempt";
        case DiagnosticEventType::DIAGNOSTIC_HEARTBEAT:  return "diagnostic.heartbeat";
        case DiagnosticEventType::TOOL_LOOP:             return "tool.loop";
        default:                                         return "unknown";
    }
}

// ============================================================================
// 基础事件结构
// ============================================================================

struct DiagnosticEvent {
    DiagnosticEventType type;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> session_id;
    std::optional<std::string> session_key;
    std::optional<std::string> trace_id;
    std::optional<std::string> span_id;
    
    DiagnosticEvent(DiagnosticEventType t) 
        : type(t), timestamp(std::chrono::system_clock::now()) {}
    
    virtual ~DiagnosticEvent() = default;
    virtual std::string to_json() const = 0;
};

// ============================================================================
// 1. Model Usage Event (LLM Token 统计)
// ============================================================================

struct ModelUsageEvent : DiagnosticEvent {
    std::string provider;          // openai, anthropic, deepseek
    std::string model;             // gpt-4, claude-3-opus
    int64_t prompt_tokens;
    int64_t completion_tokens;
    int64_t total_tokens;
    double cost_usd;               // 费用 (美元)
    int64_t duration_ms;           // 调用耗时
    bool cached;                   // 是否命中缓存
    
    ModelUsageEvent() : DiagnosticEvent(DiagnosticEventType::MODEL_USAGE) {}
    
    std::string to_json() const override;
};

// ============================================================================
// 2-4. Webhook Events (HTTP 网关)
// ============================================================================

struct WebhookReceivedEvent : DiagnosticEvent {
    std::string channel;           // whatsapp, telegram, http
    std::string method;            // POST, GET
    std::string path;              // /v1/chat/completions
    size_t body_size_bytes;
    std::string remote_ip;
    
    WebhookReceivedEvent() : DiagnosticEvent(DiagnosticEventType::WEBHOOK_RECEIVED) {}
    
    std::string to_json() const override;
};

struct WebhookProcessedEvent : DiagnosticEvent {
    std::string channel;
    int64_t duration_ms;
    int status_code;               // 200, 500
    bool success;
    
    WebhookProcessedEvent() : DiagnosticEvent(DiagnosticEventType::WEBHOOK_PROCESSED) {}
    
    std::string to_json() const override;
};

struct WebhookErrorEvent : DiagnosticEvent {
    std::string channel;
    std::string error_type;        // timeout, validation_error, internal_error
    std::string error_message;
    std::optional<std::string> stack_trace;
    
    WebhookErrorEvent() : DiagnosticEvent(DiagnosticEventType::WEBHOOK_ERROR) {}
    
    std::string to_json() const override;
};

// ============================================================================
// 5-6. Message Events (消息队列)
// ============================================================================

struct MessageQueuedEvent : DiagnosticEvent {
    std::string message_id;
    std::string channel;
    size_t queue_depth;            // 当前队列长度
    int priority;
    
    MessageQueuedEvent() : DiagnosticEvent(DiagnosticEventType::MESSAGE_QUEUED) {}
    
    std::string to_json() const override;
};

struct MessageProcessedEvent : DiagnosticEvent {
    std::string message_id;
    std::string channel;
    int64_t duration_ms;
    bool success;
    std::optional<std::string> error_message;
    int tool_calls_count;          // 调用了多少个工具
    int agent_iterations;          // Agent 迭代次数
    
    MessageProcessedEvent() : DiagnosticEvent(DiagnosticEventType::MESSAGE_PROCESSED) {}
    
    std::string to_json() const override;
};

// ============================================================================
// 7-8. Session Events (Session 生命周期)
// ============================================================================

enum class SessionState {
    IDLE,                          // 空闲
    PROCESSING,                    // 处理中
    WAITING_FOR_USER,              // 等待用户输入
    WAITING_FOR_TOOL,              // 等待工具执行
    STUCK                          // 卡住 (超时)
};

inline std::string session_state_to_string(SessionState state) {
    switch (state) {
        case SessionState::IDLE:               return "idle";
        case SessionState::PROCESSING:         return "processing";
        case SessionState::WAITING_FOR_USER:   return "waiting_for_user";
        case SessionState::WAITING_FOR_TOOL:   return "waiting_for_tool";
        case SessionState::STUCK:              return "stuck";
        default:                               return "unknown";
    }
}

struct SessionStateEvent : DiagnosticEvent {
    SessionState old_state;
    SessionState new_state;
    std::string reason;            // user_message, tool_response, timeout
    int64_t state_duration_ms;     // 上一个状态持续时间
    
    SessionStateEvent() : DiagnosticEvent(DiagnosticEventType::SESSION_STATE) {}
    
    std::string to_json() const override;
};

struct SessionStuckEvent : DiagnosticEvent {
    SessionState stuck_state;
    int64_t stuck_duration_ms;     // 卡住时长 (毫秒)
    std::string last_activity;     // 最后一次活动
    std::chrono::system_clock::time_point last_activity_time;
    
    SessionStuckEvent() : DiagnosticEvent(DiagnosticEventType::SESSION_STUCK) {}
    
    std::string to_json() const override;
};

// ============================================================================
// 9-10. Queue Lane Events (多优先级队列)
// ============================================================================

struct QueueLaneEnqueueEvent : DiagnosticEvent {
    std::string lane_name;         // high, normal, low
    std::string task_id;
    int priority;
    size_t lane_depth;
    
    QueueLaneEnqueueEvent() : DiagnosticEvent(DiagnosticEventType::QUEUE_LANE_ENQUEUE) {}
    
    std::string to_json() const override;
};

struct QueueLaneDequeueEvent : DiagnosticEvent {
    std::string lane_name;
    std::string task_id;
    int64_t wait_duration_ms;      // 在队列中等待时长
    
    QueueLaneDequeueEvent() : DiagnosticEvent(DiagnosticEventType::QUEUE_LANE_DEQUEUE) {}
    
    std::string to_json() const override;
};

// ============================================================================
// 11. Run Attempt Event (React 重试)
// ============================================================================

struct RunAttemptEvent : DiagnosticEvent {
    int attempt_number;            // 第几次重试
    int max_attempts;              // 最大重试次数
    std::string failure_reason;    // 失败原因
    int64_t backoff_ms;            // 退避时长
    
    RunAttemptEvent() : DiagnosticEvent(DiagnosticEventType::RUN_ATTEMPT) {}
    
    std::string to_json() const override;
};

// ============================================================================
// 12. Heartbeat Event (30秒心跳)
// ============================================================================

struct HeartbeatEvent : DiagnosticEvent {
    int active_sessions;
    int queued_messages;
    double cpu_percent;
    size_t memory_mb;
    int goroutines;                // 对应 C++ 的线程数
    std::string version;
    
    HeartbeatEvent() : DiagnosticEvent(DiagnosticEventType::DIAGNOSTIC_HEARTBEAT) {}
    
    std::string to_json() const override;
};

// ============================================================================
// 13. Tool Loop Event (工具循环检测)
// ============================================================================

struct ToolLoopEvent : DiagnosticEvent {
    std::string tool_name;
    int consecutive_calls;         // 连续调用次数
    std::vector<std::string> last_args_hash;  // 最近参数的哈希
    bool is_loop_detected;
    
    ToolLoopEvent() : DiagnosticEvent(DiagnosticEventType::TOOL_LOOP) {}
    
    std::string to_json() const override;
};

// ============================================================================
// ✅ 14. HTTP Request Event (HTTP请求监控 - 对齐 OpenClaw)
// ============================================================================

struct HttpRequestEvent : DiagnosticEvent {
    std::string method;            // POST, GET, PUT, DELETE
    std::string path;              // /v1/sessions/create
    int status_code;               // 200, 404, 500
    int64_t duration_ms;           // 请求处理耗时
    size_t request_size_bytes;
    size_t response_size_bytes;
    std::string user_agent;
    std::string remote_ip;
    
    HttpRequestEvent() : DiagnosticEvent(DiagnosticEventType::HTTP_REQUEST) {}
    
    std::string to_json() const override;
    
    // 便捷创建方法
    static std::shared_ptr<HttpRequestEvent> create(
        const std::string& method,
        int status_code,
        int64_t duration_ms
    ) {
        auto event = std::make_shared<HttpRequestEvent>();
        event->method = method;
        event->status_code = status_code;
        event->duration_ms = duration_ms;
        event->request_size_bytes = 0;
        event->response_size_bytes = 0;
        return event;
    }
};

// ============================================================================
// ✅ 15-17. Delivery Events (投递监控 - 对齐 OpenClaw)
// ============================================================================

struct DeliverySuccessEvent : DiagnosticEvent {
    std::string delivery_id;
    std::string channel;
    std::string to;
    int retry_count;
    int64_t duration_ms;           // 从入队到成功的总耗时
    
    DeliverySuccessEvent() : DiagnosticEvent(DiagnosticEventType::DELIVERY_SUCCESS) {}
    
    std::string to_json() const override {
        json j;
        j["type"] = "delivery.success";
        j["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count();
        
        if (session_id) j["session_id"] = *session_id;
        
        j["delivery_id"] = delivery_id;
        j["channel"] = channel;
        j["to"] = to;
        j["retry_count"] = retry_count;
        j["duration_ms"] = duration_ms;
        
        return j.dump();
    }
};

struct DeliveryFailureEvent : DiagnosticEvent {
    std::string delivery_id;
    std::string channel;
    std::string to;
    int retry_count;
    std::string error_message;
    bool is_permanent;             // 是否永久失败 (不再重试)
    
    DeliveryFailureEvent() : DiagnosticEvent(DiagnosticEventType::DELIVERY_FAILURE) {}
    
    std::string to_json() const override {
        json j;
        j["type"] = "delivery.failure";
        j["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count();
        
        if (session_id) j["session_id"] = *session_id;
        
        j["delivery_id"] = delivery_id;
        j["channel"] = channel;
        j["to"] = to;
        j["retry_count"] = retry_count;
        j["error_message"] = error_message;
        j["is_permanent"] = is_permanent;
        
        return j.dump();
    }
};

struct DeliveryRetryEvent : DiagnosticEvent {
    std::string delivery_id;
    int retry_count;
    int max_retries;
    int64_t next_retry_ms;         // 下次重试时间 (毫秒)
    int64_t backoff_ms;            // 退避时长
    
    DeliveryRetryEvent() : DiagnosticEvent(DiagnosticEventType::DELIVERY_RETRY) {}
    
    std::string to_json() const override {
        json j;
        j["type"] = "delivery.retry";
        j["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count();
        
        if (session_id) j["session_id"] = *session_id;
        
        j["delivery_id"] = delivery_id;
        j["retry_count"] = retry_count;
        j["max_retries"] = max_retries;
        j["next_retry_ms"] = next_retry_ms;
        j["backoff_ms"] = backoff_ms;
        
        return j.dump();
    }
};

// ============================================================================
// Event Bus (观察者模式)
// ============================================================================

using EventCallback = std::function<void(const std::shared_ptr<DiagnosticEvent>&)>;

class DiagnosticEventBus {
public:
    static DiagnosticEventBus& instance();
    
    // 订阅事件
    void subscribe(EventCallback callback);
    void subscribe(DiagnosticEventType type, EventCallback callback);
    
    // 发射事件 (异步, 非阻塞)
    void emit(std::shared_ptr<DiagnosticEvent> event);
    
    // 启动/停止事件处理线程
    void start();
    void stop();
    
private:
    DiagnosticEventBus() = default;
    ~DiagnosticEventBus();
    
    std::vector<EventCallback> global_subscribers_;
    std::map<DiagnosticEventType, std::vector<EventCallback>> type_subscribers_;
    std::mutex mutex_;
    
    // 异步处理队列
    struct EventQueue;
    std::unique_ptr<EventQueue> queue_;
};

// 便捷函数
template<typename EventType, typename... Args>
void emit_diagnostic_event(Args&&... args) {
    auto event = std::make_shared<EventType>(std::forward<Args>(args)...);
    DiagnosticEventBus::instance().emit(event);
}

}  // namespace observability
}  // namespace hunterclaw
