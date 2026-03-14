#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hunterclaw {
namespace cron {

/**
 * Cron Schedule - 定时调度配置
 * 对齐 OpenClaw cron/types.ts CronSchedule
 */
struct CronSchedule {
  enum class Kind {
    AT,     // 特定时间执行一次
    EVERY,  // 每隔一段时间执行
    CRON    // Cron 表达式
  };
  
  Kind kind;
  
  // For kind == AT
  std::string at;  // ISO 8601 timestamp
  
  // For kind == EVERY
  int64_t every_ms = 0;
  std::optional<int64_t> anchor_ms;
  
  // For kind == CRON
  std::string expr;  // Cron expression (e.g., "0 9 * * *")
  std::optional<std::string> tz;  // Timezone (e.g., "America/Los_Angeles")
  int64_t stagger_ms = 0;  // Deterministic stagger window
};

/**
 * Cron Run Status - 执行状态
 * 对齐 OpenClaw cron/types.ts CronRunStatus
 */
enum class CronRunStatus {
  OK,       // 执行成功
  ERROR,    // 执行失败
  SKIPPED   // 跳过执行
};

/**
 * Cron Delivery Mode - 投递模式
 * 对齐 OpenClaw cron/types.ts CronDeliveryMode
 */
enum class CronDeliveryMode {
  NONE,      // 不投递
  ANNOUNCE,  // 通过消息频道投递
  WEBHOOK    // 通过 HTTP POST 投递
};

/**
 * Cron Delivery - 投递配置
 * 对齐 OpenClaw cron/types.ts CronDelivery
 */
struct CronDelivery {
  CronDeliveryMode mode = CronDeliveryMode::NONE;
  std::optional<std::string> channel;  // "discord", "slack", "telegram", "last"
  std::optional<std::string> to;  // 目标用户/频道
  std::optional<std::string> account_id;
  bool best_effort = false;
};

/**
 * Cron Failure Alert - 失败告警
 * 对齐 OpenClaw cron/types.ts CronFailureAlert
 */
struct CronFailureAlert {
  int after = 1;  // 连续失败次数
  std::optional<std::string> channel;
  std::optional<std::string> to;
  int64_t cooldown_ms = 0;
  std::string mode = "announce";
  std::optional<std::string> account_id;
};

/**
 * Cron Payload - 执行负载
 * 对齐 OpenClaw cron/types.ts CronPayload
 */
struct CronPayload {
  enum class Kind {
    SYSTEM_EVENT,  // 系统事件
    AGENT_TURN     // Agent 回合
  };
  
  Kind kind;
  
  // For kind == SYSTEM_EVENT
  std::string text;
  
  // For kind == AGENT_TURN
  std::string message;
  std::optional<std::string> model;
  std::vector<std::string> fallbacks;
  std::optional<std::string> thinking;
  int timeout_seconds = 0;
  bool allow_unsafe_external_content = false;
  bool light_context = false;
  bool deliver = false;
  std::optional<std::string> channel_target;
  std::optional<std::string> to_target;
  bool best_effort_deliver = false;
};

/**
 * Cron Job State - 任务状态
 * 对齐 OpenClaw cron/types.ts CronJobState
 */
struct CronJobState {
  std::optional<int64_t> next_run_at_ms;
  std::optional<int64_t> running_at_ms;
  std::optional<int64_t> last_run_at_ms;
  std::optional<CronRunStatus> last_run_status;
  std::optional<std::string> last_error;
  std::optional<int64_t> last_duration_ms;
  int consecutive_errors = 0;
  std::optional<int64_t> last_failure_alert_at_ms;
  int schedule_error_count = 0;
  std::optional<std::string> last_delivery_status;
  std::optional<std::string> last_delivery_error;
  bool last_delivered = false;
};

/**
 * Cron Job - 定时任务
 * 对齐 OpenClaw cron/types.ts CronJob
 */
struct CronJob {
  std::string id;
  std::string name;
  std::string session_target = "main";  // "main" or "isolated"
  std::string wake_mode = "next-heartbeat";  // "next-heartbeat" or "now"
  CronSchedule schedule;
  CronPayload payload;
  CronDelivery delivery;
  std::optional<CronFailureAlert> failure_alert;
  bool enabled = true;
  int64_t created_at_ms = 0;
  int64_t updated_at_ms = 0;
  CronJobState state;
};

/**
 * Cron Job Create - 创建任务参数
 * 对齐 OpenClaw cron/types.ts CronJobCreate
 */
struct CronJobCreate {
  std::string name;
  std::string session_target = "main";
  std::string wake_mode = "next-heartbeat";
  CronSchedule schedule;
  CronPayload payload;
  CronDelivery delivery;
  std::optional<CronFailureAlert> failure_alert;
  bool enabled = true;
  std::optional<CronJobState> state;
};

/**
 * Cron Job Patch - 更新任务参数
 * 对齐 OpenClaw cron/types.ts CronJobPatch
 */
struct CronJobPatch {
  std::optional<std::string> name;
  std::optional<bool> enabled;
  std::optional<CronSchedule> schedule;
  std::optional<CronPayload> payload;
  std::optional<CronDelivery> delivery;
  std::optional<CronFailureAlert> failure_alert;
  std::optional<CronJobState> state;
};

/**
 * Cron Run Log Entry - 执行日志条目
 * 对齐 OpenClaw cron/run-log.ts RunLogEntry
 */
struct CronRunLogEntry {
  std::string job_id;
  std::string job_name;
  int64_t started_at_ms;
  int64_t ended_at_ms;
  CronRunStatus status;
  std::optional<std::string> error;
  std::optional<std::string> summary;
  std::optional<std::string> session_id;
  bool delivered = false;
};

/**
 * Cron Store - 持久化存储
 * 对齐 OpenClaw cron/store.ts
 */
class CronStore {
 public:
  virtual ~CronStore() = default;
  
  virtual std::vector<CronJob> list() = 0;
  virtual std::optional<CronJob> get(const std::string& job_id) = 0;
  virtual CronJob add(const CronJobCreate& create) = 0;
  virtual std::optional<CronJob> update(const std::string& job_id, const CronJobPatch& patch) = 0;
  virtual bool remove(const std::string& job_id) = 0;
  virtual void save() = 0;
};

/**
 * Cron Schedule Calculator - 调度计算器
 * 对齐 OpenClaw cron/schedule.ts
 */
class CronScheduleCalculator {
 public:
  /**
   * computeNextRun - 计算下次执行时间
   * 
   * 参数:
   * - schedule: 调度配置
   * - now_ms: 当前时间 (毫秒)
   * - last_run_ms: 上次执行时间 (可选)
   * 
   * 返回: 下次执行时间 (毫秒), 如果无法计算返回 nullopt
   */
  static std::optional<int64_t> compute_next_run(
      const CronSchedule& schedule,
      int64_t now_ms,
      std::optional<int64_t> last_run_ms = std::nullopt);
  
  /**
   * isOverdue - 检查是否逾期
   */
  static bool is_overdue(int64_t next_run_at_ms, int64_t now_ms, int64_t grace_ms = 1000);
  
  /**
   * shouldSkip - 检查是否应跳过
   */
  static bool should_skip(int64_t next_run_at_ms, int64_t now_ms, int64_t skip_threshold_ms);
};

/**
 * Cron Service - 定时任务服务
 * 对齐 OpenClaw cron/service.ts
 * 
 * 核心功能:
 * 1. 管理定时任务 (CRUD)
 * 2. 调度执行
 * 3. 状态管理
 * 4. 失败重试
 * 5. 投递管理
 */
class CronService {
 public:
  explicit CronService(std::shared_ptr<CronStore> store);
  ~CronService();
  
  /**
   * start - 启动服务
   */
  void start();
  
  /**
   * stop - 停止服务
   */
  void stop();
  
  /**
   * addJob - 添加任务
   */
  CronJob add_job(const CronJobCreate& create);
  
  /**
   * updateJob - 更新任务
   */
  std::optional<CronJob> update_job(const std::string& job_id, const CronJobPatch& patch);
  
  /**
   * deleteJob - 删除任务
   */
  bool delete_job(const std::string& job_id);
  
  /**
   * getJob - 获取任务
   */
  std::optional<CronJob> get_job(const std::string& job_id);
  
  /**
   * listJobs - 列出任务
   */
  std::vector<CronJob> list_jobs(bool enabled_only = false);
  
  /**
   * forceRun - 强制执行任务
   */
  void force_run(const std::string& job_id);
  
  /**
   * setExecutor - 设置执行器
   */
  void set_executor(std::function<void(const CronJob&)> executor);

 private:
  void scheduler_loop();
  void execute_job(const CronJob& job);
  void update_job_state(const std::string& job_id, const CronJobState& state);
  void rearm_timer();
  
  std::shared_ptr<CronStore> store_;
  std::function<void(const CronJob&)> executor_;
  std::atomic<bool> running_{false};
  std::thread scheduler_thread_;
  std::map<std::string, int64_t> next_run_times_;
  mutable std::mutex mutex_;
};

/**
 * createCronService - 创建 Cron 服务 (工厂函数)
 */
std::shared_ptr<CronService> createCronService(std::shared_ptr<CronStore> store);

/**
 * createMemoryCronStore - 创建内存存储
 */
std::shared_ptr<CronStore> createMemoryCronStore();

// 辅助函数
std::string cron_run_status_to_string(CronRunStatus status);
CronRunStatus cron_run_status_from_string(const std::string& str);

}  // namespace cron
}  // namespace hunterclaw
