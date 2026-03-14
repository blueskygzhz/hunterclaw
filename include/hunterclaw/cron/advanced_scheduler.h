#pragma once

#include "hunterclaw/cron/types.h"
#include "hunterclaw/core/result.h"
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace hunterclaw {
namespace cron {

// 条件触发类型
enum class TriggerType {
    EventBased,    // 基于事件 (message_sent, file_uploaded)
    Threshold,     // 阈值触发 (error_count > 10)
    RateBased,     // 速率触发 (requests > 100/min)
    ChainedJob     // 链式任务 (job_a 成功后触发 job_b)
};

// 事件条件
struct EventCondition {
    std::string event_type;
    std::optional<std::string> source;
    std::map<std::string, std::string> filters;
};

// 阈值条件
struct ThresholdCondition {
    std::string metric_name;
    std::string comparison;  // "gt", "lt", "eq"
    double threshold_value;
    std::chrono::milliseconds window_ms{60000};
};

// 链式任务条件
struct ChainedJobCondition {
    std::string dependent_job_id;
    std::string trigger_on;  // "success", "failure", "completion"
    std::optional<std::chrono::milliseconds> delay_ms;
};

// 高级调度配置
struct AdvancedScheduleConfig {
    TriggerType type;
    std::optional<EventCondition> event;
    std::optional<ThresholdCondition> threshold;
    std::optional<ChainedJobCondition> chained;
    
    // Retry 策略
    bool retry_enabled = true;
    std::vector<int64_t> backoff_ms = {30000, 60000, 300000, 900000};
    std::vector<std::string> retry_on = {"rate_limit", "network", "timeout"};
    
    core::JsonValue to_json() const;
    static core::Result<AdvancedScheduleConfig> from_json(const core::JsonValue& json);
};

// 高级调度器
class AdvancedScheduler {
public:
    using EventCallback = std::function<void(const std::string& job_id)>;
    
    explicit AdvancedScheduler(EventCallback on_trigger);
    
    core::Result<void> add_job(const std::string& job_id, const AdvancedScheduleConfig& config);
    core::Result<void> remove_job(const std::string& job_id);
    
    // 事件通知
    void notify_event(const std::string& event_type, const std::map<std::string, std::string>& data);
    void notify_metric(const std::string& metric_name, double value);
    void notify_job_completed(const std::string& job_id, bool success);
    
private:
    bool evaluate_condition(const std::string& job_id, const AdvancedScheduleConfig& config);
    
    std::map<std::string, AdvancedScheduleConfig> jobs_;
    EventCallback on_trigger_;
    std::mutex mutex_;
};

}  // namespace cron
}  // namespace hunterclaw
