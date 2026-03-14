#include "hunterclaw/observability/diagnostic_events.h"
#include <atomic>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

using json = nlohmann::json;

namespace hunterclaw {
namespace observability {

// ============================================================================
// JSON 序列化辅助函数
// ============================================================================

static std::string timestamp_to_iso8601(const std::chrono::system_clock::time_point& tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = *std::gmtime(&tt);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

static json event_base_to_json(const DiagnosticEvent& evt) {
    json j = {
        {"type", event_type_to_string(evt.type)},
        {"timestamp", timestamp_to_iso8601(evt.timestamp)}
    };
    
    if (evt.session_id) j["sessionId"] = *evt.session_id;
    if (evt.session_key) j["sessionKey"] = *evt.session_key;
    if (evt.trace_id) j["traceId"] = *evt.trace_id;
    if (evt.span_id) j["spanId"] = *evt.span_id;
    
    return j;
}

// ============================================================================
// 1. Model Usage Event
// ============================================================================

std::string ModelUsageEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["provider"] = provider;
    j["model"] = model;
    j["usage"] = {
        {"promptTokens", prompt_tokens},
        {"completionTokens", completion_tokens},
        {"totalTokens", total_tokens}
    };
    j["costUsd"] = cost_usd;
    j["durationMs"] = duration_ms;
    j["cached"] = cached;
    return j.dump();
}

// ============================================================================
// 2-4. Webhook Events
// ============================================================================

std::string WebhookReceivedEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["channel"] = channel;
    j["method"] = method;
    j["path"] = path;
    j["bodySizeBytes"] = body_size_bytes;
    j["remoteIp"] = remote_ip;
    return j.dump();
}

std::string WebhookProcessedEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["channel"] = channel;
    j["durationMs"] = duration_ms;
    j["statusCode"] = status_code;
    j["success"] = success;
    return j.dump();
}

std::string WebhookErrorEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["channel"] = channel;
    j["errorType"] = error_type;
    j["errorMessage"] = error_message;
    if (stack_trace) j["stackTrace"] = *stack_trace;
    return j.dump();
}

// ============================================================================
// 5-6. Message Events
// ============================================================================

std::string MessageQueuedEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["messageId"] = message_id;
    j["channel"] = channel;
    j["queueDepth"] = queue_depth;
    j["priority"] = priority;
    return j.dump();
}

std::string MessageProcessedEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["messageId"] = message_id;
    j["channel"] = channel;
    j["durationMs"] = duration_ms;
    j["success"] = success;
    if (error_message) j["errorMessage"] = *error_message;
    j["toolCallsCount"] = tool_calls_count;
    j["agentIterations"] = agent_iterations;
    return j.dump();
}

// ============================================================================
// 7-8. Session Events
// ============================================================================

std::string SessionStateEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["oldState"] = session_state_to_string(old_state);
    j["newState"] = session_state_to_string(new_state);
    j["reason"] = reason;
    j["stateDurationMs"] = state_duration_ms;
    return j.dump();
}

std::string SessionStuckEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["stuckState"] = session_state_to_string(stuck_state);
    j["stuckDurationMs"] = stuck_duration_ms;
    j["lastActivity"] = last_activity;
    j["lastActivityTime"] = timestamp_to_iso8601(last_activity_time);
    return j.dump();
}

// ============================================================================
// 9-10. Queue Lane Events
// ============================================================================

std::string QueueLaneEnqueueEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["laneName"] = lane_name;
    j["taskId"] = task_id;
    j["priority"] = priority;
    j["laneDepth"] = lane_depth;
    return j.dump();
}

std::string QueueLaneDequeueEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["laneName"] = lane_name;
    j["taskId"] = task_id;
    j["waitDurationMs"] = wait_duration_ms;
    return j.dump();
}

// ============================================================================
// 11. Run Attempt Event
// ============================================================================

std::string RunAttemptEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["attemptNumber"] = attempt_number;
    j["maxAttempts"] = max_attempts;
    j["failureReason"] = failure_reason;
    j["backoffMs"] = backoff_ms;
    return j.dump();
}

// ============================================================================
// 12. Heartbeat Event
// ============================================================================

std::string HeartbeatEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["activeSessions"] = active_sessions;
    j["queuedMessages"] = queued_messages;
    j["cpuPercent"] = cpu_percent;
    j["memoryMb"] = memory_mb;
    j["threads"] = goroutines;  // 对应线程数
    j["version"] = version;
    return j.dump();
}

// ============================================================================
// 13. Tool Loop Event
// ============================================================================

std::string ToolLoopEvent::to_json() const {
    json j = event_base_to_json(*this);
    j["toolName"] = tool_name;
    j["consecutiveCalls"] = consecutive_calls;
    j["lastArgsHash"] = last_args_hash;
    j["isLoopDetected"] = is_loop_detected;
    return j.dump();
}

// ============================================================================
// Event Bus Implementation (异步事件队列)
// ============================================================================

struct DiagnosticEventBus::EventQueue {
    std::queue<std::shared_ptr<DiagnosticEvent>> events;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> running{true};
    std::thread worker;
    
    void push(std::shared_ptr<DiagnosticEvent> event) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push(std::move(event));
        }
        cv.notify_one();
    }
    
    std::shared_ptr<DiagnosticEvent> pop() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return !events.empty() || !running; });
        
        if (!running && events.empty()) {
            return nullptr;
        }
        
        auto event = events.front();
        events.pop();
        return event;
    }
};

DiagnosticEventBus& DiagnosticEventBus::instance() {
    static DiagnosticEventBus instance;
    return instance;
}

DiagnosticEventBus::~DiagnosticEventBus() {
    stop();
}

void DiagnosticEventBus::subscribe(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    global_subscribers_.push_back(std::move(callback));
}

void DiagnosticEventBus::subscribe(DiagnosticEventType type, EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    type_subscribers_[type].push_back(std::move(callback));
}

void DiagnosticEventBus::emit(std::shared_ptr<DiagnosticEvent> event) {
    if (!queue_) return;
    
    // 异步推入队列 (非阻塞)
    try {
        queue_->push(std::move(event));
    } catch (const std::exception& e) {
        // 故障隔离: 永不阻塞主流程
        // (可以选择记录到错误日志)
    }
}

void DiagnosticEventBus::start() {
    if (queue_) return;  // 已启动
    
    queue_ = std::make_unique<EventQueue>();
    
    // 启动事件处理线程
    queue_->worker = std::thread([this]() {
        while (queue_->running) {
            auto event = queue_->pop();
            if (!event) break;
            
            try {
                // 调用全局订阅者
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (auto& callback : global_subscribers_) {
                        try {
                            callback(event);
                        } catch (const std::exception& e) {
                            // 故障隔离: 单个订阅者失败不影响其他订阅者
                        }
                    }
                }
                
                // 调用类型订阅者
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = type_subscribers_.find(event->type);
                    if (it != type_subscribers_.end()) {
                        for (auto& callback : it->second) {
                            try {
                                callback(event);
                            } catch (const std::exception& e) {
                                // 故障隔离
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                // 故障隔离
            }
        }
    });
}

void DiagnosticEventBus::stop() {
    if (!queue_) return;
    
    queue_->running = false;
    queue_->cv.notify_all();
    
    if (queue_->worker.joinable()) {
        queue_->worker.join();
    }
    
    queue_.reset();
}

}  // namespace observability
}  // namespace hunterclaw
