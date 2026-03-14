#pragma once

#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace hunterclaw {
namespace cron {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Schedule Types (对齐 OpenClaw CronSchedule)
// ============================================================================

enum class ScheduleKind {
    At,      // 一次性任务: "2026-03-10T14:00:00Z"
    Every,   // 周期性任务: 每隔X毫秒
    Cron     // Cron表达式: "0 7 * * *"
};

struct Schedule {
    ScheduleKind kind;
    
    // For 'at'
    std::optional<std::string> at;  // ISO8601 timestamp
    
    // For 'every'
    std::optional<int64_t> every_ms;
    std::optional<int64_t> anchor_ms;
    
    // For 'cron'
    std::optional<std::string> expr;  // Cron expression
    std::optional<std::string> tz;    // Timezone (e.g., "America/Los_Angeles")
    std::optional<int64_t> stagger_ms; // 错峰调度偏移
    
    // Helpers
    static Schedule at_time(const std::string& timestamp);
    static Schedule every_interval(int64_t interval_ms, std::optional<int64_t> anchor = std::nullopt);
    static Schedule cron_expr(const std::string& expression, 
                        std::optional<std::string> timezone = std::nullopt,
                        std::optional<int64_t> stagger = std::nullopt);
};

// ============================================================================
// Session Target (对齐 OpenClaw CronSessionTarget)
// ============================================================================

enum class SessionTarget {
    Main,      // 注入到主会话 (系统事件)
    Isolated   // 创建独立会话
};

// ============================================================================
// Wake Mode (对齐 OpenClaw CronWakeMode)
// ============================================================================

enum class WakeMode {
    NextHeartbeat,  // 等待下次心跳
    Now             // 立即触发心跳
};

// ============================================================================
// Delivery Mode (对齐 OpenClaw CronDeliveryMode)
// ============================================================================

enum class DeliveryMode {
    None,      // 不投递
    Announce,  // 发送到消息通道
    Webhook    // 调用 Webhook
};

// ============================================================================
// Payload (对齐 OpenClaw CronPayload)
// ============================================================================

struct Payload {
    enum class Kind {
        AgentTurn,     // Agent 执行
        SystemEvent    // 系统事件
    };
    
    Kind kind;
    std::string message;  // 任务描述或系统事件文本
    
    // For AgentTurn
    std::optional<std::string> model;
    std::optional<bool> thinking;
    std::optional<int> timeout_seconds;
    
    static Payload agent_turn(const std::string& msg);
    static Payload system_event(const std::string& text);
};

// ============================================================================
// Delivery Config (对齐 OpenClaw CronDelivery)
// ============================================================================

struct Delivery {
    DeliveryMode mode;
    
    std::optional<std::string> channel;    // "slack", "discord", etc.
    std::optional<std::string> to;         // "channel:C123", "user:U456"
    std::optional<std::string> account_id;
    std::optional<std::string> webhook_url;
    
    static Delivery none();
    static Delivery announce(const std::string& channel, const std::string& to);
    static Delivery webhook(const std::string& url);
};

// ============================================================================
// Job State (对齐 OpenClaw CronJobState)
// ============================================================================

struct JobState {
    std::optional<int64_t> next_run_at_ms;    // 下次运行时间
    std::optional<int64_t> last_run_at_ms;    // 上次运行时间
    std::optional<std::string> last_run_status;  // "ok", "error", "skipped"
    std::optional<std::string> last_run_error;
    std::optional<std::string> last_run_summary;
    
    // Retry logic (for one-shot jobs)
    std::optional<int> retry_count;           // 重试次数
    std::optional<int64_t> retry_at_ms;       // 重试时间
    
    // Failure alert tracking
    std::optional<int> consecutive_failures;
    std::optional<int64_t> last_alert_at_ms;
};

// ============================================================================
// Job Definition (对齐 OpenClaw CronJob)
// ============================================================================

struct Job {
    std::string id;          // 唯一标识
    std::string name;        // 任务名称
    bool enabled;            // 是否启用
    
    Schedule schedule;       // 调度配置
    SessionTarget session_target;
    WakeMode wake_mode;
    Payload payload;
    
    std::optional<Delivery> delivery;  // 投递配置
    
    JobState state;          // 运行状态
    
    // Metadata
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<std::string> lane;
    
    int64_t created_at_ms;
    int64_t updated_at_ms;
    
    // Helpers
    bool is_due(int64_t now_ms) const;
    bool is_one_shot() const;  // at 类型为 one-shot
};

// ============================================================================
// Run Log Entry (对齐 OpenClaw CronRunLogEntry)
// ============================================================================

struct RunLogEntry {
    int64_t ts;              // 时间戳
    std::string job_id;
    std::string action;      // "finished"
    std::string status;      // "ok", "error", "skipped"
    
    std::optional<std::string> error;
    std::optional<std::string> summary;
    std::optional<bool> delivered;
    std::optional<std::string> session_id;
    std::optional<int64_t> run_at_ms;
    std::optional<int64_t> duration_ms;
    std::optional<int64_t> next_run_at_ms;
    std::optional<std::string> model;
    
    json to_json() const;
    static RunLogEntry from_json(const json& j);
};

// ============================================================================
// Job Create/Update (对齐 OpenClaw CronJobCreate/CronJobPatch)
// ============================================================================

struct JobCreate {
    std::string name;
    bool enabled = true;
    
    Schedule schedule;
    SessionTarget session_target = SessionTarget::Isolated;
    WakeMode wake_mode = WakeMode::Now;
    Payload payload;
    
    std::optional<Delivery> delivery;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<std::string> lane;
};

struct JobPatch {
    std::optional<std::string> name;
    std::optional<bool> enabled;
    std::optional<Schedule> schedule;
    std::optional<SessionTarget> session_target;
    std::optional<WakeMode> wake_mode;
    std::optional<Payload> payload;
    std::optional<Delivery> delivery;
};

// ============================================================================
// Config (对齐 OpenClaw CronConfig)
// ============================================================================

struct Config {
    bool enabled = true;
    std::string store_path = "~/.hunterclaw/cron/jobs.json";
    int max_concurrent_runs = 3;
    int64_t session_retention_ms = 24 * 3600 * 1000;  // 24小时
    
    // Retry config (for one-shot jobs)
    struct Retry {
        bool enabled = true;
        std::vector<int64_t> backoff_ms = {
            30'000,      // 30秒
            60'000,      // 1分钟
            5 * 60'000,  // 5分钟
            15 * 60'000, // 15分钟
            60 * 60'000  // 60分钟
        };
        std::vector<std::string> retry_on = {
            "rate_limit", "overloaded", "network", "timeout", "server_error"
        };
    } retry;
    
    // Failure alert config
    struct FailureAlert {
        int after = 2;  // 连续失败N次后告警
        int64_t cooldown_ms = 3600'000;  // 1小时冷却
    } failure_alert;
};

// ============================================================================
// JSON Conversion
// ============================================================================

void to_json(json& j, const Schedule& s);
void from_json(const json& j, Schedule& s);

void to_json(json& j, const Payload& p);
void from_json(const json& j, Payload& p);

void to_json(json& j, const Delivery& d);
void from_json(const json& j, Delivery& d);

void to_json(json& j, const JobState& s);
void from_json(const json& j, JobState& s);

void to_json(json& j, const Job& job);
void from_json(const json& j, Job& job);

} // namespace cron
} // namespace hunterclaw
