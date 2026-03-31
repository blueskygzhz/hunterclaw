#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
 * Cron Failure Destination - 失败通知目标
 * 对齐 OpenClaw cron/types.ts CronFailureDestination
 */
struct CronFailureDestination {
  std::optional<std::string> channel;
  std::optional<std::string> to;
  std::optional<std::string> account_id;
  std::optional<std::string> mode;  // "announce" | "webhook"
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
  std::optional<CronFailureDestination> failure_destination;
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
  std::optional<int64_t> running_at_ms;   // 并发控制：正在运行的时间戳
  std::optional<int64_t> last_run_at_ms;
  std::optional<CronRunStatus> last_run_status;
  // 对齐 OpenClaw lastStatus (back-compat alias for lastRunStatus)
  std::optional<CronRunStatus> last_status;
  std::optional<std::string> last_error;
  std::optional<std::string> last_error_reason;  // 对齐 lastErrorReason
  std::optional<int64_t> last_duration_ms;
  int consecutive_errors = 0;
  std::optional<int64_t> last_failure_alert_at_ms;
  int schedule_error_count = 0;
  std::optional<std::string> last_delivery_status;
  std::optional<std::string> last_delivery_error;
  std::optional<bool> last_delivered;  // 对齐 OpenClaw: optional<bool> 区分 delivered/not-delivered/unknown
};

/**
 * Cron Job - 定时任务
 * 对齐 OpenClaw cron/types.ts CronJob
 */
struct CronJob {
  std::string id;
  std::string name;
  std::optional<std::string> description;   // 对齐 description
  std::string session_target = "main";  // "main" or "isolated"
  std::string wake_mode = "next-heartbeat";  // "next-heartbeat" or "now"
  CronSchedule schedule;
  CronPayload payload;
  CronDelivery delivery;
  // failureAlert: nullopt=未设置, has_value=已设置, disabled_=false 禁用
  // 用 optional<CronFailureAlert> 表示，通过 failure_alert_disabled 标志表示 false
  std::optional<CronFailureAlert> failure_alert;
  bool failure_alert_disabled = false;  // 对齐 failureAlert: false
  bool enabled = true;
  bool delete_after_run = false;  // 对齐 deleteAfterRun
  int64_t created_at_ms = 0;
  int64_t updated_at_ms = 0;
  CronJobState state;
  std::optional<std::string> agent_id;
  std::optional<std::string> session_key;
};

/**
 * Cron Job Create - 创建任务参数
 * 对齐 OpenClaw cron/types.ts CronJobCreate
 */
struct CronJobCreate {
  std::string name;
  std::optional<std::string> description;
  std::string session_target = "main";
  std::string wake_mode = "next-heartbeat";
  CronSchedule schedule;
  CronPayload payload;
  CronDelivery delivery;
  std::optional<CronFailureAlert> failure_alert;
  bool failure_alert_disabled = false;
  bool enabled = true;
  bool delete_after_run = false;
  std::optional<CronJobState> state;
  std::optional<std::string> agent_id;
  std::optional<std::string> session_key;
};

/**
 * Cron Job Patch - 更新任务参数
 * 对齐 OpenClaw cron/types.ts CronJobPatch
 */
struct CronJobPatch {
  std::optional<std::string> name;
  std::optional<std::string> description;
  std::optional<bool> enabled;
  std::optional<bool> delete_after_run;
  std::optional<CronSchedule> schedule;
  std::optional<CronPayload> payload;
  std::optional<CronDelivery> delivery;
  std::optional<CronFailureAlert> failure_alert;
  std::optional<bool> failure_alert_disabled;
  std::optional<CronJobState> state;
  std::optional<std::string> agent_id;
  std::optional<std::string> session_key;
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
 * Cron Run Outcome - 执行结果
 * 对齐 OpenClaw cron/types.ts CronRunOutcome + CronRunTelemetry
 */
struct CronRunOutcome {
  CronRunStatus status;
  std::optional<std::string> error;
  std::optional<std::string> summary;
  std::optional<std::string> session_id;
  std::optional<std::string> session_key;
  std::optional<bool> delivered;
  std::optional<bool> delivery_attempted;
  // CronRunTelemetry 字段 (对齐 OpenClaw)
  std::optional<std::string> model;
  std::optional<std::string> provider;
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
  static std::optional<int64_t> compute_next_run(
      const CronSchedule& schedule,
      int64_t now_ms,
      std::optional<int64_t> last_run_ms = std::nullopt);
  
  static bool is_overdue(int64_t next_run_at_ms, int64_t now_ms, int64_t grace_ms = 1000);
  static bool should_skip(int64_t next_run_at_ms, int64_t now_ms, int64_t skip_threshold_ms);
};

/**
 * Heartbeat 运行结果 (对齐 OpenClaw HeartbeatRunResult)
 */
struct HeartbeatRunResult {
  enum class Status { Ran, Skipped, Error };
  Status status;
  std::string reason;  // skipped/error 时的原因
};

/**
 * Cron 全局配置 (对齐 OpenClaw CronConfig)
 */
struct CronConfig {
  // 最大并发执行数 (对齐 OpenClaw cronConfig.maxConcurrentRuns)
  int max_concurrent_runs = 1;

  // 失败告警全局配置
  struct FailureAlertConfig {
    bool enabled = false;
    int after = 2;
    int64_t cooldown_ms = 60 * 60'000;
    std::optional<std::string> channel;
    std::optional<std::string> mode;
    std::optional<std::string> account_id;
  };
  std::optional<FailureAlertConfig> failure_alert;

  // 重试配置 (对齐 OpenClaw cronConfig.retry)
  struct RetryConfig {
    int max_attempts = 3;
    std::vector<int64_t> backoff_ms;  // 空则使用默认
  };
  std::optional<RetryConfig> retry;
};

/**
 * Cron Service Deps - 依赖注入
 * 对齐 OpenClaw cron/service/state.ts CronServiceDeps
 */
struct CronServiceDeps {
  // 时间函数
  std::function<int64_t()> now_ms = []() -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
  };

  // 是否启用 cron
  bool cron_enabled = true;

  // 默认 agent id
  std::optional<std::string> default_agent_id;

  // 启动时最多补跑的错过任务数
  int max_missed_jobs_per_restart = 5;

  // 错过任务的错峰间隔 (ms)
  int64_t missed_job_stagger_ms = 5000;

  // wakeMode=now 时等待 heartbeat 的最大时间
  int64_t wake_now_heartbeat_busy_max_wait_ms = 2 * 60 * 1000;

  // wakeMode=now 时 heartbeat 重试间隔
  int64_t wake_now_heartbeat_busy_retry_delay_ms = 250;

  // Cron 全局配置 (对齐 OpenClaw cronConfig)
  std::optional<CronConfig> cron_config;

  // 注入系统事件 (main session 路径)
  std::function<void(const std::string& text,
                     const std::optional<std::string>& agent_id,
                     const std::optional<std::string>& session_key,
                     const std::optional<std::string>& context_key)> enqueue_system_event;

  // 请求立即触发心跳 (next-heartbeat 模式)
  std::function<void(const std::optional<std::string>& reason,
                     const std::optional<std::string>& agent_id,
                     const std::optional<std::string>& session_key)> request_heartbeat_now;

  // 同步执行一次 heartbeat 并等待结果 (wakeMode=now 路径)
  // 对齐 OpenClaw runHeartbeatOnce
  std::function<HeartbeatRunResult(
      const std::optional<std::string>& reason,
      const std::optional<std::string>& agent_id,
      const std::optional<std::string>& session_key)> run_heartbeat_once;

  // 执行 isolated agent job
  // 对齐 OpenClaw server-cron.ts: runIsolatedAgentJob({ job, message, abortSignal })
  // lane 参数由调用方注入 ("cron")，内部通过 resolveNestedAgentLane 转换为 "nested" 防死锁
  std::function<CronRunOutcome(const CronJob& job,
                               const std::string& message,
                               const std::string& lane)> run_isolated_agent_job;

  // 发送失败告警
  std::function<void(const CronJob& job,
                     const std::string& text,
                     const std::string& channel,
                     const std::optional<std::string>& to,
                     const std::optional<std::string>& mode,
                     const std::optional<std::string>& account_id)> send_cron_failure_alert;

  // 事件回调
  std::function<void(const std::string& job_id,
                     const std::string& action,
                     const std::optional<int64_t>& next_run_at_ms)> on_event;
};

/**
 * Cron Service - 定时任务服务
 * 对齐 OpenClaw cron/service.ts
 */
class CronService : public std::enable_shared_from_this<CronService> {
 public:
  explicit CronService(std::shared_ptr<CronStore> store, CronServiceDeps deps = {});
  ~CronService();
  
  void start();
  void stop();
  
  CronJob add_job(const CronJobCreate& create);
  std::optional<CronJob> update_job(const std::string& job_id, const CronJobPatch& patch);
  bool delete_job(const std::string& job_id);
  std::optional<CronJob> get_job(const std::string& job_id);
  std::vector<CronJob> list_jobs(bool enabled_only = false);
  
  // 手动触发任务 (force=true 忽略 enabled 和 nextRunAtMs)
  void force_run(const std::string& job_id);

  // 异步入队手动触发 (对齐 OpenClaw enqueueRun)
  bool enqueue_run(const std::string& job_id, bool force = false);

  // 设置执行器 (兼容旧接口)
  void set_executor(std::function<void(const CronJob&)> executor);

 private:
  // 调度主循环 (对齐 OpenClaw onTimer)
  void scheduler_loop();
  
  // 执行单个任务核心逻辑 (对齐 OpenClaw executeJobCore)
  CronRunOutcome execute_job_core(const CronJob& job);
  
  // 应用执行结果，更新状态，返回是否应删除任务 (对齐 OpenClaw applyJobResult)
  bool apply_job_result(
      CronJob& job,
      CronRunStatus status,
      const std::optional<std::string>& error,
      const std::optional<bool>& delivered,
      int64_t started_at,
      int64_t ended_at,
      bool preserve_schedule = false);
  
  // 启动时补跑错过的任务 (对齐 OpenClaw runMissedJobs)
  void run_missed_jobs(const std::set<std::string>& skip_job_ids = {});
  
  // 计算 job 的下次运行时间 (含 stagger)
  std::optional<int64_t> compute_job_next_run_at_ms(const CronJob& job, int64_t now_ms);

  // 计算 job 的上次应运行时间 (对齐 OpenClaw computeJobPreviousRunAtMs，用于 missed-run 检测)
  std::optional<int64_t> compute_job_previous_run_at_ms(const CronJob& job, int64_t now_ms);

  // 记录 schedule 计算错误，超过阈值自动禁用 (对齐 OpenClaw recordScheduleComputeError)
  bool record_schedule_compute_error(CronJob& job, const std::string& err);

  // 解析 delivery 状态 (对齐 OpenClaw resolveDeliveryStatus)
  std::string resolve_delivery_status(const CronJob& job, const std::optional<bool>& delivered);
  
  // 重新计算所有 job 的 nextRunAtMs
  void recompute_next_runs(int64_t now_ms);
  
  // 维护性重计算 (不推进 past-due nextRunAtMs)
  bool recompute_next_runs_for_maintenance(int64_t now_ms, bool recompute_expired = false);
  
  // 重新 arm 定时器
  void rearm_timer();
  
  // 发送失败告警
  void emit_failure_alert(const CronJob& job, const std::string& error, int consecutive_errors);
  
  // 更新 job 状态到 store
  void update_job_state(const std::string& job_id, const CronJobState& state);
  
  std::shared_ptr<CronStore> store_;
  CronServiceDeps deps_;
  std::function<void(const CronJob&)> executor_;  // 兼容旧接口
  
  std::atomic<bool> running_{false};
  bool tick_running_ = false;  // 对齐 OpenClaw state.running (防并发 tick)
  std::thread scheduler_thread_;
  std::mutex mutex_;
  std::condition_variable cv_;

  // 下次唤醒时间 (ms)
  std::optional<int64_t> next_wake_at_ms_;

  // 手动触发队列 (对齐 OpenClaw enqueueRun)
  std::queue<std::pair<std::string, bool>> manual_run_queue_;
  std::mutex manual_run_mutex_;
};

/**
 * createCronService - 创建 Cron 服务 (工厂函数)
 */
std::shared_ptr<CronService> createCronService(std::shared_ptr<CronStore> store,
                                                CronServiceDeps deps = {});

/**
 * createMemoryCronStore - 创建内存存储
 */
std::shared_ptr<CronStore> createMemoryCronStore();

// 辅助函数
std::string cron_run_status_to_string(CronRunStatus status);
CronRunStatus cron_run_status_from_string(const std::string& str);

}  // namespace cron
}  // namespace hunterclaw
