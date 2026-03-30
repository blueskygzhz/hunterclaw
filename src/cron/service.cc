#include "hunterclaw/cron/service.h"
#include "hunterclaw/cron/schedule.h"
#include "hunterclaw/queue/command_queue.h"
#include "hunterclaw/queue/lanes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace hunterclaw {
namespace cron {

namespace {

// ============================================================================
// Constants (对齐 OpenClaw timer.ts)
// ============================================================================

// 最大定时器延迟，防止调度漂移
constexpr int64_t MAX_TIMER_DELAY_MS = 60'000;

// 同一 cron job 两次触发的最小间隔，防止热循环 (#17821)
constexpr int64_t MIN_REFIRE_GAP_MS = 2'000;

// 默认启动补跑最大任务数
constexpr int DEFAULT_MAX_MISSED_JOBS_PER_RESTART = 5;

// 默认错过任务错峰间隔
constexpr int64_t DEFAULT_MISSED_JOB_STAGGER_MS = 5'000;

// 默认失败告警触发阈值
constexpr int DEFAULT_FAILURE_ALERT_AFTER = 2;

// 默认失败告警冷却时间 (1小时)
constexpr int64_t DEFAULT_FAILURE_ALERT_COOLDOWN_MS = 60 * 60'000;

// stuck run 超时 (2小时)
constexpr int64_t STUCK_RUN_MS = 2 * 60 * 60'000;

// 连续 schedule 计算错误自动禁用阈值 (对齐 OpenClaw MAX_SCHEDULE_ERRORS)
constexpr int MAX_SCHEDULE_ERRORS = 3;

// 默认 job 超时 (10分钟，对齐 OpenClaw DEFAULT_JOB_TIMEOUT_MS)
constexpr int64_t DEFAULT_JOB_TIMEOUT_MS = 10 * 60'000;

// agentTurn 安全超时 (60分钟，对齐 OpenClaw AGENT_TURN_SAFETY_TIMEOUT_MS)
constexpr int64_t AGENT_TURN_SAFETY_TIMEOUT_MS = 60 * 60'000;

// 指数退避表 (对齐 OpenClaw DEFAULT_BACKOFF_SCHEDULE_MS)
const std::vector<int64_t> DEFAULT_BACKOFF_SCHEDULE_MS = {
    30'000,       // 1st error  → 30s
    60'000,       // 2nd error  →  1min
    5 * 60'000,   // 3rd error  →  5min
    15 * 60'000,  // 4th error  → 15min
    60 * 60'000,  // 5th+ error → 60min
};

// one-shot 任务最大重试次数
constexpr int DEFAULT_MAX_TRANSIENT_RETRIES = 3;

// ============================================================================
// Helpers
// ============================================================================

std::string generate_uuid() {
  // 简单 UUID 生成
  static std::atomic<uint64_t> counter{0};
  std::stringstream ss;
  auto now = std::chrono::system_clock::now().time_since_epoch().count();
  ss << std::hex << now << "-" << counter.fetch_add(1);
  return ss.str();
}

int64_t error_backoff_ms(int consecutive_errors) {
  int idx = std::min(consecutive_errors - 1,
                     static_cast<int>(DEFAULT_BACKOFF_SCHEDULE_MS.size()) - 1);
  idx = std::max(0, idx);
  return DEFAULT_BACKOFF_SCHEDULE_MS[idx];
}

// 判断是否为瞬态错误（可重试）
bool is_transient_cron_error(const std::string& error) {
  if (error.empty()) return false;
  // 简单关键词匹配
  static const std::vector<std::string> patterns = {
      "rate_limit", "rate limit", "too many requests", "429",
      "overloaded", "529", "high demand",
      "network", "econnreset", "econnrefused", "fetch failed",
      "timeout", "etimedout",
  };
  std::string lower = error;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  for (const auto& p : patterns) {
    if (lower.find(p) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// ============================================================================
// CronScheduleCalculator Implementation
// ============================================================================

std::optional<int64_t> CronScheduleCalculator::compute_next_run(
    const CronSchedule& schedule,
    int64_t now_ms,
    std::optional<int64_t> /*last_run_ms*/) {
  switch (schedule.kind) {
    case CronSchedule::Kind::AT: {
      int64_t at_ms = std::stoll(schedule.at);
      return (at_ms > now_ms) ? std::optional<int64_t>(at_ms) : std::nullopt;
    }
    case CronSchedule::Kind::EVERY: {
      if (schedule.every_ms <= 0) return std::nullopt;
      int64_t anchor = schedule.anchor_ms.value_or(now_ms);
      if (now_ms < anchor) return anchor;
      int64_t elapsed = now_ms - anchor;
      int64_t steps = std::max<int64_t>(1, (elapsed + schedule.every_ms - 1) / schedule.every_ms);
      return anchor + steps * schedule.every_ms;
    }
    case CronSchedule::Kind::CRON: {
      // 委托给 schedule.cc 的 parse_cron_next_ms
      return parse_cron_next_ms(schedule.expr, now_ms, schedule.tz);
    }
  }
  return std::nullopt;
}

bool CronScheduleCalculator::is_overdue(int64_t next_run_at_ms, int64_t now_ms, int64_t grace_ms) {
  return now_ms >= (next_run_at_ms + grace_ms);
}

bool CronScheduleCalculator::should_skip(int64_t next_run_at_ms, int64_t now_ms,
                                          int64_t skip_threshold_ms) {
  return (now_ms - next_run_at_ms) > skip_threshold_ms;
}

// ============================================================================
// Memory Cron Store Implementation
// ============================================================================

class MemoryCronStore : public CronStore {
 public:
  MemoryCronStore() = default;
  ~MemoryCronStore() override = default;

  std::vector<CronJob> list() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_;
  }

  std::optional<CronJob> get(const std::string& job_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(),
                           [&](const CronJob& job) { return job.id == job_id; });
    if (it == jobs_.end()) return std::nullopt;
    return *it;
  }

  CronJob add(const CronJobCreate& create) override {
    std::lock_guard<std::mutex> lock(mutex_);
    CronJob job;
    job.id = generate_uuid();
    job.name = create.name;
    job.description = create.description;
    job.session_target = create.session_target;
    job.wake_mode = create.wake_mode;
    job.schedule = create.schedule;
    job.payload = create.payload;
    job.delivery = create.delivery;
    job.failure_alert = create.failure_alert;
    job.failure_alert_disabled = create.failure_alert_disabled;
    job.enabled = create.enabled;
    job.delete_after_run = create.delete_after_run;
    job.agent_id = create.agent_id;
    job.session_key = create.session_key;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    job.created_at_ms = now;
    job.updated_at_ms = now;
    job.state = create.state.value_or(CronJobState{});
    jobs_.push_back(job);
    return job;
  }

  std::optional<CronJob> update(const std::string& job_id, const CronJobPatch& patch) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(),
                           [&](const CronJob& job) { return job.id == job_id; });
    if (it == jobs_.end()) return std::nullopt;

    if (patch.name) it->name = *patch.name;
    if (patch.description) it->description = *patch.description;
    if (patch.enabled) it->enabled = *patch.enabled;
    if (patch.delete_after_run) it->delete_after_run = *patch.delete_after_run;
    if (patch.schedule) it->schedule = *patch.schedule;
    if (patch.payload) it->payload = *patch.payload;
    if (patch.delivery) it->delivery = *patch.delivery;
    if (patch.failure_alert) it->failure_alert = *patch.failure_alert;
    if (patch.failure_alert_disabled) it->failure_alert_disabled = *patch.failure_alert_disabled;
    if (patch.agent_id) it->agent_id = *patch.agent_id;
    if (patch.session_key) it->session_key = *patch.session_key;
    if (patch.state) {
      auto& s = it->state;
      const auto& ns = *patch.state;
      if (ns.next_run_at_ms) s.next_run_at_ms = ns.next_run_at_ms;
      if (ns.running_at_ms) s.running_at_ms = ns.running_at_ms;
      if (ns.last_run_at_ms) s.last_run_at_ms = ns.last_run_at_ms;
      if (ns.last_run_status) s.last_run_status = ns.last_run_status;
      if (ns.last_status) s.last_status = ns.last_status;  // back-compat alias
      if (ns.last_error) s.last_error = ns.last_error;
      if (ns.last_error_reason) s.last_error_reason = ns.last_error_reason;
      if (ns.last_duration_ms) s.last_duration_ms = ns.last_duration_ms;
      s.consecutive_errors = ns.consecutive_errors;
      s.schedule_error_count = ns.schedule_error_count;
      s.last_delivered = ns.last_delivered;  // optional<bool>
      if (ns.last_failure_alert_at_ms) s.last_failure_alert_at_ms = ns.last_failure_alert_at_ms;
      if (ns.last_delivery_status) s.last_delivery_status = ns.last_delivery_status;
      if (ns.last_delivery_error) s.last_delivery_error = ns.last_delivery_error;
    }
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    it->updated_at_ms = now;
    return *it;
  }

  bool remove(const std::string& job_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(),
                           [&](const CronJob& job) { return job.id == job_id; });
    if (it == jobs_.end()) return false;
    jobs_.erase(it);
    return true;
  }

  void save() override {}

  // 直接访问 jobs（供 CronService 内部使用）
  std::vector<CronJob>& jobs_ref() { return jobs_; }
  std::mutex& mutex_ref() { return mutex_; }

 private:
  std::vector<CronJob> jobs_;
  std::mutex mutex_;
};

// ============================================================================
// CronService Implementation
// ============================================================================

CronService::CronService(std::shared_ptr<CronStore> store, CronServiceDeps deps)
    : store_(std::move(store)), deps_(std::move(deps)) {}

CronService::~CronService() {
  stop();
}

void CronService::start() {
  if (!deps_.cron_enabled) {
    return;
  }

  // Phase 1: 清理 stale running 标记
  std::set<std::string> startup_interrupted_ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto jobs = store_->list();
    for (auto& job : jobs) {
      if (job.state.running_at_ms.has_value()) {
        job.state.running_at_ms = std::nullopt;
        startup_interrupted_ids.insert(job.id);
        CronJobPatch patch;
        patch.state = job.state;
        store_->update(job.id, patch);
      }
    }
  }

  // Phase 2: 补跑错过的任务
  run_missed_jobs(startup_interrupted_ids);

  // Phase 3: 重新计算 nextRunAtMs，启动调度线程
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = deps_.now_ms();
    recompute_next_runs(now);
    running_ = true;
  }

  scheduler_thread_ = std::thread(&CronService::scheduler_loop, this);
}

void CronService::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
  }
  if (scheduler_thread_.joinable()) {
    scheduler_thread_.join();
  }
}

CronJob CronService::add_job(const CronJobCreate& create) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto job = store_->add(create);
  int64_t now = deps_.now_ms();
  auto next = compute_job_next_run_at_ms(job, now);
  if (next.has_value()) {
    job.state.next_run_at_ms = next;
    CronJobPatch patch;
    patch.state = job.state;
    store_->update(job.id, patch);
  }
  rearm_timer();
  return job;
}

std::optional<CronJob> CronService::update_job(const std::string& job_id,
                                                const CronJobPatch& patch) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = store_->update(job_id, patch);
  if (result.has_value()) {
    int64_t now = deps_.now_ms();
    bool schedule_changed = patch.schedule.has_value();
    bool enabled_changed = patch.enabled.has_value();
    if (schedule_changed || enabled_changed) {
      if (result->enabled) {
        result->state.next_run_at_ms = compute_job_next_run_at_ms(*result, now);
      } else {
        result->state.next_run_at_ms = std::nullopt;
        result->state.running_at_ms = std::nullopt;
      }
      CronJobPatch state_patch;
      state_patch.state = result->state;
      store_->update(job_id, state_patch);
    }
    rearm_timer();
  }
  return result;
}

bool CronService::delete_job(const std::string& job_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool removed = store_->remove(job_id);
  if (removed) {
    rearm_timer();
  }
  return removed;
}

std::optional<CronJob> CronService::get_job(const std::string& job_id) {
  return store_->get(job_id);
}

std::vector<CronJob> CronService::list_jobs(bool enabled_only) {
  auto jobs = store_->list();
  if (!enabled_only) return jobs;
  std::vector<CronJob> result;
  std::copy_if(jobs.begin(), jobs.end(), std::back_inserter(result),
               [](const CronJob& job) { return job.enabled; });
  return result;
}

void CronService::force_run(const std::string& job_id) {
  auto job = store_->get(job_id);
  if (!job) return;
  int64_t now = deps_.now_ms();
  job->state.running_at_ms = now;
  job->state.last_error = std::nullopt;
  CronJobPatch patch;
  patch.state = job->state;
  store_->update(job_id, patch);

  auto outcome = execute_job_core(*job);
  int64_t end = deps_.now_ms();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stored = store_->get(job_id);
    if (stored.has_value()) {
      bool should_delete = apply_job_result(*stored, outcome.status, outcome.error,
                                            outcome.delivered, now, end,
                                            /*preserve_schedule=*/true);
      CronJobPatch sp;
      sp.state = stored->state;
      sp.enabled = stored->enabled;
      store_->update(job_id, sp);
      if (should_delete) {
        store_->remove(job_id);
      }
    }
    rearm_timer();
  }
}

void CronService::set_executor(std::function<void(const CronJob&)> executor) {
  std::lock_guard<std::mutex> lock(mutex_);
  executor_ = std::move(executor);
}

// ============================================================================
// compute_job_next_run_at_ms (对齐 OpenClaw computeJobNextRunAtMs + stagger)
// ============================================================================

std::optional<int64_t> CronService::compute_job_next_run_at_ms(const CronJob& job,
                                                                  int64_t now_ms) {
  if (!job.enabled) return std::nullopt;

  if (job.schedule.kind == CronSchedule::Kind::EVERY) {
    int64_t every_ms = std::max<int64_t>(1, job.schedule.every_ms);
    // 优先从 lastRunAtMs 计算
    if (job.state.last_run_at_ms.has_value()) {
      int64_t next_from_last = *job.state.last_run_at_ms + every_ms;
      if (next_from_last > now_ms) return next_from_last;
    }
    int64_t anchor = job.schedule.anchor_ms.value_or(job.created_at_ms > 0 ? job.created_at_ms : now_ms);
    int64_t elapsed = now_ms - anchor;
    int64_t steps = std::max<int64_t>(1, (elapsed + every_ms - 1) / every_ms);
    return anchor + steps * every_ms;
  }

  if (job.schedule.kind == CronSchedule::Kind::AT) {
    auto at_ms_opt = parse_absolute_time_ms(job.schedule.at);
    if (!at_ms_opt.has_value()) return std::nullopt;
    int64_t at_ms = *at_ms_opt;
    // one-shot: 已成功执行过则不再触发
    if (job.state.last_run_status.has_value() &&
        *job.state.last_run_status == CronRunStatus::OK &&
        job.state.last_run_at_ms.has_value()) {
      if (at_ms > *job.state.last_run_at_ms) return at_ms;
      return std::nullopt;
    }
    return (at_ms > 0) ? std::optional<int64_t>(at_ms) : std::nullopt;
  }

  // CRON: 应用 stagger
  int64_t stagger_ms = job.schedule.stagger_ms;
  if (stagger_ms <= 0) {
    // 检查是否为整点表达式，自动加 5 分钟 stagger
    auto default_stagger = get_default_stagger_ms(job.schedule.expr);
    if (default_stagger.has_value()) {
      stagger_ms = *default_stagger;
    }
  }

  if (stagger_ms > 0 && !job.id.empty()) {
    int64_t offset = compute_stable_stagger_offset(job.id, stagger_ms);
    // 向前移动 cursor 以找到当前 stagger 窗口
    int64_t cursor = std::max<int64_t>(0, now_ms - offset);
    for (int attempt = 0; attempt < 4; ++attempt) {
      auto base_next = parse_cron_next_ms(job.schedule.expr, cursor, job.schedule.tz);
      if (!base_next.has_value()) return std::nullopt;
      int64_t shifted = *base_next + offset;
      if (shifted > now_ms) return shifted;
      cursor = std::max(cursor + 1, *base_next + 1000);
    }
    return std::nullopt;
  }

  // 无 stagger
  auto next = parse_cron_next_ms(job.schedule.expr, now_ms, job.schedule.tz);
  if (!next.has_value()) {
    // croner 边界情况：从下一秒重试
    int64_t next_sec = (now_ms / 1000) * 1000 + 1000;
    next = parse_cron_next_ms(job.schedule.expr, next_sec, job.schedule.tz);
  }
  return next;
}

// ============================================================================
// recompute_next_runs / recompute_next_runs_for_maintenance
// ============================================================================

void CronService::recompute_next_runs(int64_t now_ms) {
  auto jobs = store_->list();
  for (auto& job : jobs) {
    if (!job.enabled) {
      if (job.state.next_run_at_ms.has_value() || job.state.running_at_ms.has_value()) {
        job.state.next_run_at_ms = std::nullopt;
        job.state.running_at_ms = std::nullopt;
        CronJobPatch p;
        p.state = job.state;
        store_->update(job.id, p);
      }
      continue;
    }
    // 清理 stuck running 标记
    if (job.state.running_at_ms.has_value() &&
        now_ms - *job.state.running_at_ms > STUCK_RUN_MS) {
      job.state.running_at_ms = std::nullopt;
    }
    // 只有 nextRunAtMs 缺失或已过期才重新计算
    bool is_due_or_missing = !job.state.next_run_at_ms.has_value() ||
                             now_ms >= *job.state.next_run_at_ms;
    if (is_due_or_missing) {
      job.state.next_run_at_ms = compute_job_next_run_at_ms(job, now_ms);
      CronJobPatch p;
      p.state = job.state;
      store_->update(job.id, p);
    }
  }
}

bool CronService::recompute_next_runs_for_maintenance(int64_t now_ms, bool recompute_expired) {
  bool changed = false;
  auto jobs = store_->list();
  for (auto& job : jobs) {
    if (!job.enabled) {
      if (job.state.next_run_at_ms.has_value() || job.state.running_at_ms.has_value()) {
        job.state.next_run_at_ms = std::nullopt;
        job.state.running_at_ms = std::nullopt;
        CronJobPatch p;
        p.state = job.state;
        store_->update(job.id, p);
        changed = true;
      }
      continue;
    }
    // 清理 stuck running 标记
    if (job.state.running_at_ms.has_value() &&
        now_ms - *job.state.running_at_ms > STUCK_RUN_MS) {
      job.state.running_at_ms = std::nullopt;
      CronJobPatch p;
      p.state = job.state;
      store_->update(job.id, p);
      changed = true;
    }
    if (!job.state.next_run_at_ms.has_value()) {
      // 缺失时修复
      job.state.next_run_at_ms = compute_job_next_run_at_ms(job, now_ms);
      CronJobPatch p;
      p.state = job.state;
      store_->update(job.id, p);
      changed = true;
    } else if (recompute_expired && now_ms >= *job.state.next_run_at_ms &&
               !job.state.running_at_ms.has_value()) {
      // 只有已执行过该 slot 才推进
      bool already_executed = job.state.last_run_at_ms.has_value() &&
                              *job.state.last_run_at_ms >= *job.state.next_run_at_ms;
      if (already_executed) {
        job.state.next_run_at_ms = compute_job_next_run_at_ms(job, now_ms);
        CronJobPatch p;
        p.state = job.state;
        store_->update(job.id, p);
        changed = true;
      }
    }
  }
  return changed;
}

// ============================================================================
// resolve_delivery_status (对齐 OpenClaw resolveDeliveryStatus)
// ============================================================================

std::string CronService::resolve_delivery_status(const CronJob& job,
                                                   const std::optional<bool>& delivered) {
  if (delivered.has_value()) {
    return delivered.value() ? "delivered" : "not-delivered";
  }
  bool delivery_requested = (job.delivery.mode != CronDeliveryMode::NONE);
  return delivery_requested ? "unknown" : "not-requested";
}

// ============================================================================
// record_schedule_compute_error (对齐 OpenClaw recordScheduleComputeError)
// ============================================================================

bool CronService::record_schedule_compute_error(CronJob& job, const std::string& err) {
  int error_count = job.state.schedule_error_count + 1;
  job.state.schedule_error_count = error_count;
  job.state.next_run_at_ms = std::nullopt;
  job.state.last_error = "schedule error: " + err;

  if (error_count >= MAX_SCHEDULE_ERRORS) {
    // 自动禁用并通知 (对齐 OpenClaw #28861)
    job.enabled = false;
    std::string notify_text = "\u26a0\ufe0f Cron job \"" + job.name +
                              "\" has been auto-disabled after " +
                              std::to_string(error_count) +
                              " consecutive schedule errors. Last error: " + err;
    if (deps_.enqueue_system_event) {
      deps_.enqueue_system_event(notify_text, job.agent_id, job.session_key,
                                  "cron:" + job.id + ":auto-disabled");
    }
    if (deps_.request_heartbeat_now) {
      deps_.request_heartbeat_now("cron:" + job.id + ":auto-disabled",
                                   job.agent_id, job.session_key);
    }
  }
  return true;
}

// ============================================================================
// compute_job_previous_run_at_ms (对齐 OpenClaw computeJobPreviousRunAtMs)
// ============================================================================

std::optional<int64_t> CronService::compute_job_previous_run_at_ms(const CronJob& job,
                                                                      int64_t now_ms) {
  if (!job.enabled || job.schedule.kind != CronSchedule::Kind::CRON) {
    return std::nullopt;
  }

  int64_t stagger_ms = job.schedule.stagger_ms;
  if (stagger_ms <= 0) {
    auto default_stagger = get_default_stagger_ms(job.schedule.expr);
    if (default_stagger.has_value()) {
      stagger_ms = *default_stagger;
    }
  }

  if (stagger_ms > 0 && !job.id.empty()) {
    int64_t offset = compute_stable_stagger_offset(job.id, stagger_ms);
    if (offset > 0) {
      // 对齐 OpenClaw computeStaggeredCronPreviousRunAtMs
      // 向前移动 cursor 找上一个 cron slot + offset
      int64_t cursor = std::max<int64_t>(0, now_ms - offset);
      for (int attempt = 0; attempt < 4; ++attempt) {
        auto base_next = parse_cron_next_ms(job.schedule.expr, cursor, job.schedule.tz);
        if (!base_next.has_value()) return std::nullopt;
        int64_t shifted = *base_next + offset;
        if (shifted <= now_ms) return shifted;
        if (cursor <= 0) break;
        cursor = std::max<int64_t>(0, cursor - 60'000);
      }
      return std::nullopt;
    }
  }

  // 无 stagger：找上一个 cron slot
  int64_t cursor = now_ms - 1;
  for (int attempt = 0; attempt < 4; ++attempt) {
    auto next = parse_cron_next_ms(job.schedule.expr, cursor - 60'000, job.schedule.tz);
    if (!next.has_value()) return std::nullopt;
    if (*next <= now_ms) return *next;
    cursor -= 60'000;
    if (cursor <= 0) break;
  }
  return std::nullopt;
}

// ============================================================================
// enqueue_run (对齐 OpenClaw enqueueRun)
// ============================================================================

bool CronService::enqueue_run(const std::string& job_id, bool force) {
  // 对齐 OpenClaw enqueueRun:
  // 1. 先做 preflight 检查（已运行中 / 未到期）
  // 2. 通过 CommandLane::Cron 排队，保证手动触发串行化，对齐 OpenClaw enqueueCommandInLane(CommandLane.Cron, ...)
  auto job = store_->get(job_id);
  if (!job.has_value()) return false;

  // 已在运行中
  if (job->state.running_at_ms.has_value()) return false;

  // 非 force 模式：检查是否到期
  if (!force) {
    int64_t now = deps_.now_ms();
    bool is_due = job->enabled &&
                  job->state.next_run_at_ms.has_value() &&
                  now >= *job->state.next_run_at_ms;
    if (!is_due) return false;
  }

  // 对齐 OpenClaw: void enqueueCommandInLane(CommandLane.Cron, async () => { run(state, id, mode) })
  // 通过 Cron lane 排队，保证手动触发与定时触发的并发控制一致
  // fire-and-forget: 不等待结果
  auto self = shared_from_this();
  auto& cq = hunterclaw::queue::CommandQueue::Instance();
  cq.enqueue_command_in_lane(
    hunterclaw::queue::CommandLane::Cron,
    [self, job_id, force]() {
      // 对齐 OpenClaw: run(state, id, mode) — 在 Cron lane 内串行执行
      // 再次检查是否仍可运行（可能在排队期间已被其他触发执行）
      auto stored = self->store_->get(job_id);
      if (!stored.has_value()) return;
      if (stored->state.running_at_ms.has_value()) {
        // 对齐 OpenClaw: "cron: queued manual run skipped before execution"
        return;
      }
      // 标记为 running
      int64_t now = self->deps_.now_ms();
      stored->state.running_at_ms = now;
      stored->state.last_error = std::nullopt;
      {
        std::lock_guard<std::mutex> lock(self->mutex_);
        hunterclaw::cron::CronJobPatch p;
        p.state = stored->state;
        self->store_->update(job_id, p);
      }
      // 执行
      auto outcome = self->execute_job_core(*stored);
      int64_t ended_at = self->deps_.now_ms();
      {
        std::lock_guard<std::mutex> lock(self->mutex_);
        auto latest = self->store_->get(job_id);
        if (!latest.has_value()) return;
        bool should_delete = self->apply_job_result(
            *latest, outcome.status, outcome.error, outcome.delivered,
            now, ended_at, /*preserve_schedule=*/force);
        hunterclaw::cron::CronJobPatch sp;
        sp.state = latest->state;
        sp.enabled = latest->enabled;
        self->store_->update(job_id, sp);
        if (should_delete) {
          self->store_->remove(job_id);
        }
        self->rearm_timer();
      }
    }
  );
  return true;
}

// ============================================================================
// rearm_timer (对齐 OpenClaw armTimer)
// ============================================================================

void CronService::rearm_timer() {
  // 找到最近的 nextRunAtMs
  auto jobs = store_->list();
  std::optional<int64_t> next_wake;
  for (const auto& job : jobs) {
    if (job.enabled && job.state.next_run_at_ms.has_value()) {
      if (!next_wake.has_value() || *job.state.next_run_at_ms < *next_wake) {
        next_wake = job.state.next_run_at_ms;
      }
    }
  }
  next_wake_at_ms_ = next_wake;
  cv_.notify_all();
}

// ============================================================================
// executeJobCore (对齐 OpenClaw executeJobCore)
// ============================================================================

CronRunOutcome CronService::execute_job_core(const CronJob& job) {
  // main session 路径
  if (job.session_target == "main") {
    if (job.payload.kind != CronPayload::Kind::SYSTEM_EVENT) {
      return {CronRunStatus::SKIPPED, "main job requires payload.kind=systemEvent"};
    }
    const std::string& text = job.payload.text;
    if (text.empty()) {
      return {CronRunStatus::SKIPPED, "main job requires non-empty systemEvent text"};
    }

    if (deps_.enqueue_system_event) {
      deps_.enqueue_system_event(text, job.agent_id, job.session_key,
                                  "cron:" + job.id);
    }

    // wakeMode=now 路径：使用 runHeartbeatOnce 重试循环 (对齐 OpenClaw)
    if (job.wake_mode == "now" && deps_.run_heartbeat_once) {
      const std::string reason = "cron:" + job.id;
      int64_t max_wait_ms = deps_.wake_now_heartbeat_busy_max_wait_ms;
      int64_t retry_delay_ms = deps_.wake_now_heartbeat_busy_retry_delay_ms;
      int64_t wait_started_at = deps_.now_ms();

      for (;;) {
        auto hb_result = deps_.run_heartbeat_once(reason, job.agent_id, job.session_key);

        // 非 "requests-in-flight" 的 skipped 状态：退出循环
        if (hb_result.status != HeartbeatRunResult::Status::Skipped ||
            hb_result.reason != "requests-in-flight") {
          if (hb_result.status == HeartbeatRunResult::Status::Ran) {
            return {CronRunStatus::OK, std::nullopt, text};
          } else if (hb_result.status == HeartbeatRunResult::Status::Skipped) {
            return {CronRunStatus::SKIPPED, hb_result.reason, text};
          } else {
            return {CronRunStatus::ERROR, hb_result.reason, text};
          }
        }

        // busy：检查是否超时
        if (deps_.now_ms() - wait_started_at > max_wait_ms) {
          // 超时：回退到 requestHeartbeatNow (对齐 OpenClaw #28508)
          if (deps_.request_heartbeat_now) {
            deps_.request_heartbeat_now(reason, job.agent_id, job.session_key);
          }
          return {CronRunStatus::OK, std::nullopt, text};
        }

        // 等待后重试
        std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
      }
    }

    // wakeMode=next-heartbeat 路径
    if (deps_.request_heartbeat_now) {
      deps_.request_heartbeat_now("cron:" + job.id, job.agent_id, job.session_key);
    }
    return {CronRunStatus::OK, std::nullopt, text};
  }

  // isolated 路径
  if (job.payload.kind != CronPayload::Kind::AGENT_TURN) {
    return {CronRunStatus::SKIPPED, "isolated job requires payload.kind=agentTurn"};
  }

  if (deps_.run_isolated_agent_job) {
    // 对齐 OpenClaw server-cron.ts: runIsolatedAgentJob 注入时传 lane: "cron"
    // 内部通过 resolveNestedAgentLane("cron") → "nested" 防止死锁
    return deps_.run_isolated_agent_job(job, job.payload.message, "cron");
  }

  // 兼容旧 executor_ 接口
  if (executor_) {
    try {
      executor_(job);
      return {CronRunStatus::OK};
    } catch (const std::exception& e) {
      return {CronRunStatus::ERROR, std::string(e.what())};
    }
  }

  return {CronRunStatus::SKIPPED, "no executor configured"};
}

// ============================================================================
// emit_failure_alert (对齐 OpenClaw emitFailureAlert)
// ============================================================================

void CronService::emit_failure_alert(const CronJob& job, const std::string& error,
                                      int consecutive_errors) {
  std::string job_name = job.name.empty() ? job.id : job.name;
  std::string truncated_error = error.size() > 200 ? error.substr(0, 200) : error;
  std::string text = "Cron job \"" + job_name + "\" failed " +
                     std::to_string(consecutive_errors) + " times\n" +
                     "Last error: " + truncated_error;

  if (deps_.send_cron_failure_alert) {
    std::string channel = "last";
    std::optional<std::string> to;
    std::optional<std::string> mode;
    std::optional<std::string> account_id;
    // 优先使用 job 级别配置，回退到全局配置 (对齐 OpenClaw resolveFailureAlert)
    if (job.failure_alert.has_value()) {
      if (job.failure_alert->channel.has_value()) channel = *job.failure_alert->channel;
      to = job.failure_alert->to;
      mode = job.failure_alert->mode;
      account_id = job.failure_alert->account_id;
    } else if (deps_.cron_config.has_value() && deps_.cron_config->failure_alert.has_value()) {
      const auto& global = *deps_.cron_config->failure_alert;
      if (global.channel.has_value()) channel = *global.channel;
      mode = global.mode;
      account_id = global.account_id;
    }
    deps_.send_cron_failure_alert(job, text, channel, to, mode, account_id);
    return;
  }

  // 回退到 enqueue_system_event
  if (deps_.enqueue_system_event) {
    deps_.enqueue_system_event(text, job.agent_id, job.session_key,
                                "cron:" + job.id + ":failure-alert");
  }
  if (job.wake_mode == "now" && deps_.request_heartbeat_now) {
    deps_.request_heartbeat_now("cron:" + job.id + ":failure-alert",
                                 job.agent_id, job.session_key);
  }
}

// ============================================================================
// apply_job_result (对齐 OpenClaw applyJobResult)
// ============================================================================

bool CronService::apply_job_result(
    CronJob& job,
    CronRunStatus status,
    const std::optional<std::string>& error,
    const std::optional<bool>& delivered,
    int64_t started_at,
    int64_t ended_at,
    bool preserve_schedule) {

  int64_t prev_last_run_at_ms = job.state.last_run_at_ms.value_or(0);

  job.state.running_at_ms = std::nullopt;
  job.state.last_run_at_ms = started_at;
  job.state.last_run_status = status;
  job.state.last_status = status;  // 对齐 OpenClaw lastStatus (back-compat alias)
  job.state.last_duration_ms = std::max<int64_t>(0, ended_at - started_at);
  job.state.last_error = error;
  // 对齐 OpenClaw lastErrorReason
  job.state.last_error_reason = (status == CronRunStatus::ERROR && error.has_value())
                                     ? error
                                     : std::nullopt;
  job.state.last_delivered = delivered;  // 对齐 OpenClaw: optional<bool>
  // 对齐 OpenClaw resolveDeliveryStatus
  std::string delivery_status;
  if (delivered.has_value()) {
    delivery_status = delivered.value() ? "delivered" : "not-delivered";
  } else {
    bool delivery_requested = (job.delivery.mode != CronDeliveryMode::NONE);
    delivery_status = delivery_requested ? "unknown" : "not-requested";
  }
  job.state.last_delivery_status = delivery_status;
  job.state.last_delivery_error =
      (delivery_status == "not-delivered" && error.has_value()) ? error : std::nullopt;
  job.updated_at_ms = ended_at;

  // 连续错误计数
  if (status == CronRunStatus::ERROR) {
    job.state.consecutive_errors++;

    // 失败告警 (对齐 OpenClaw resolveFailureAlert)
    if (!job.failure_alert_disabled) {
      int after = DEFAULT_FAILURE_ALERT_AFTER;
      int64_t cooldown_ms = DEFAULT_FAILURE_ALERT_COOLDOWN_MS;
      bool alert_enabled = false;

      if (job.failure_alert.has_value()) {
        alert_enabled = true;
        after = job.failure_alert->after;
        if (job.failure_alert->cooldown_ms > 0) cooldown_ms = job.failure_alert->cooldown_ms;
      } else if (deps_.cron_config.has_value() &&
                 deps_.cron_config->failure_alert.has_value() &&
                 deps_.cron_config->failure_alert->enabled) {
        // 全局配置 (对齐 OpenClaw globalConfig.enabled)
        alert_enabled = true;
        after = deps_.cron_config->failure_alert->after;
        if (deps_.cron_config->failure_alert->cooldown_ms > 0) {
          cooldown_ms = deps_.cron_config->failure_alert->cooldown_ms;
        }
      }

      if (alert_enabled && job.state.consecutive_errors >= after) {
        // bestEffort 不触发告警 (对齐 OpenClaw)
        bool is_best_effort = (job.delivery.mode != CronDeliveryMode::NONE &&
                               job.delivery.best_effort);
        if (!is_best_effort) {
          int64_t now = deps_.now_ms();
          bool in_cooldown = job.state.last_failure_alert_at_ms.has_value() &&
                             now - *job.state.last_failure_alert_at_ms < cooldown_ms;
          if (!in_cooldown) {
            emit_failure_alert(job, error.value_or(""), job.state.consecutive_errors);
            job.state.last_failure_alert_at_ms = now;
          }
        }
      }
    }
  } else {
    job.state.consecutive_errors = 0;
    job.state.last_failure_alert_at_ms = std::nullopt;
  }

  // deleteAfterRun: at 类型成功后删除
  bool should_delete = (job.schedule.kind == CronSchedule::Kind::AT &&
                        job.delete_after_run && status == CronRunStatus::OK);

  if (!should_delete) {
    if (job.schedule.kind == CronSchedule::Kind::AT) {
      if (status == CronRunStatus::OK || status == CronRunStatus::SKIPPED) {
        // one-shot 完成：禁用
        job.enabled = false;
        job.state.next_run_at_ms = std::nullopt;
      } else {
        // one-shot 错误：判断是否可重试
        bool transient = is_transient_cron_error(error.value_or(""));
        int consecutive = job.state.consecutive_errors;
        if (transient && consecutive <= DEFAULT_MAX_TRANSIENT_RETRIES) {
          int64_t backoff = error_backoff_ms(consecutive);
          job.state.next_run_at_ms = ended_at + backoff;
        } else {
          // 永久错误或超出重试次数：禁用
          job.enabled = false;
          job.state.next_run_at_ms = std::nullopt;
        }
      }
    } else if (status == CronRunStatus::ERROR && job.enabled) {
      // recurring 任务错误：指数退避
      int64_t backoff = error_backoff_ms(job.state.consecutive_errors);
      std::optional<int64_t> normal_next;
      try {
        if (preserve_schedule && job.schedule.kind == CronSchedule::Kind::EVERY) {
          // 保留 anchor 计算
          CronJob tmp = job;
          tmp.state.last_run_at_ms = prev_last_run_at_ms > 0
                                         ? std::optional<int64_t>(prev_last_run_at_ms)
                                         : std::nullopt;
          normal_next = compute_job_next_run_at_ms(tmp, ended_at);
        } else {
          normal_next = compute_job_next_run_at_ms(job, ended_at);
        }
      } catch (const std::exception& e) {
        // schedule 计算异常：记录错误，回退到纯 backoff (对齐 OpenClaw)
        record_schedule_compute_error(job, e.what());
      }
      int64_t backoff_next = ended_at + backoff;
      job.state.next_run_at_ms =
          normal_next.has_value() ? std::max(*normal_next, backoff_next) : backoff_next;
    } else if (job.enabled) {
      // 正常完成：计算下次运行时间
      std::optional<int64_t> natural_next;
      try {
        if (preserve_schedule && job.schedule.kind == CronSchedule::Kind::EVERY) {
          CronJob tmp = job;
          tmp.state.last_run_at_ms = prev_last_run_at_ms > 0
                                         ? std::optional<int64_t>(prev_last_run_at_ms)
                                         : std::nullopt;
          natural_next = compute_job_next_run_at_ms(tmp, ended_at);
        } else {
          natural_next = compute_job_next_run_at_ms(job, ended_at);
        }
      } catch (const std::exception& e) {
        // schedule 计算异常：记录错误，防止 MIN_REFIRE_GAP_MS 热循环 (对齐 OpenClaw)
        record_schedule_compute_error(job, e.what());
      }
      if (job.schedule.kind == CronSchedule::Kind::CRON) {
        // 防止热循环：至少间隔 MIN_REFIRE_GAP_MS
        int64_t min_next = ended_at + MIN_REFIRE_GAP_MS;
        job.state.next_run_at_ms =
            natural_next.has_value() ? std::max(*natural_next, min_next) : min_next;
      } else {
        job.state.next_run_at_ms = natural_next;
      }
    } else {
      job.state.next_run_at_ms = std::nullopt;
    }
  }

  return should_delete;
}

// ============================================================================
// update_job_state
// ============================================================================

void CronService::update_job_state(const std::string& job_id, const CronJobState& state) {
  CronJobPatch patch;
  patch.state = state;
  store_->update(job_id, patch);
}

// ============================================================================
// run_missed_jobs (对齐 OpenClaw runMissedJobs)
// ============================================================================

void CronService::run_missed_jobs(const std::set<std::string>& skip_job_ids) {
  int64_t now = deps_.now_ms();
  int max_immediate = std::max(0, deps_.max_missed_jobs_per_restart > 0
                                      ? deps_.max_missed_jobs_per_restart
                                      : DEFAULT_MAX_MISSED_JOBS_PER_RESTART);
  int64_t stagger_ms = deps_.missed_job_stagger_ms > 0 ? deps_.missed_job_stagger_ms
                                                        : DEFAULT_MISSED_JOB_STAGGER_MS;

  // 收集错过的任务 (对齐 OpenClaw collectRunnableJobs with skipAtIfAlreadyRan + allowCronMissedRunByLastRun)
  std::vector<CronJob> missed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto jobs = store_->list();
    for (auto& job : jobs) {
      if (!job.enabled) continue;
      if (skip_job_ids.count(job.id)) continue;
      if (job.state.running_at_ms.has_value()) continue;

      // 对齐 OpenClaw skipAtIfAlreadyRan
      if (job.schedule.kind == CronSchedule::Kind::AT &&
          job.state.last_run_status.has_value()) {
        auto last_status = *job.state.last_run_status;
        if (last_status == CronRunStatus::OK || last_status == CronRunStatus::SKIPPED) {
          continue;
        }
        // error 状态：检查是否是重试 (nextRunAtMs > lastRunAtMs)
        if (last_status == CronRunStatus::ERROR && job.enabled &&
            job.state.next_run_at_ms.has_value() &&
            job.state.last_run_at_ms.has_value() &&
            *job.state.next_run_at_ms > *job.state.last_run_at_ms) {
          if (now >= *job.state.next_run_at_ms) {
            missed.push_back(job);
          }
          continue;
        }
        continue;
      }

      bool is_due = job.state.next_run_at_ms.has_value() && now >= *job.state.next_run_at_ms;
      if (is_due) {
        missed.push_back(job);
        continue;
      }

      // 对齐 OpenClaw allowCronMissedRunByLastRun：
      // cron 类型任务：检查上一个 slot 是否在 lastRunAtMs 之后（即错过了一次执行）
      if (job.schedule.kind == CronSchedule::Kind::CRON) {
        auto prev_run = compute_job_previous_run_at_ms(job, now);
        if (prev_run.has_value() && job.state.last_run_at_ms.has_value()) {
          if (*prev_run > *job.state.last_run_at_ms) {
            missed.push_back(job);
          }
        }
      }
    }
  }

  if (missed.empty()) return;

  // 按 nextRunAtMs 排序
  std::sort(missed.begin(), missed.end(), [](const CronJob& a, const CronJob& b) {
    return a.state.next_run_at_ms.value_or(0) < b.state.next_run_at_ms.value_or(0);
  });

  // 分割：立即执行 vs 延迟执行
  std::vector<CronJob> immediate(missed.begin(),
                                  missed.begin() + std::min((int)missed.size(), max_immediate));
  std::vector<CronJob> deferred(missed.begin() + std::min((int)missed.size(), max_immediate),
                                 missed.end());

  // 标记立即执行的任务为 running
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& job : immediate) {
      job.state.running_at_ms = now;
      job.state.last_error = std::nullopt;
      CronJobPatch p;
      p.state = job.state;
      store_->update(job.id, p);
    }
  }

  // 执行立即任务
  struct RunResult {
    std::string job_id;
    CronRunStatus status;
    std::optional<std::string> error;
    std::optional<bool> delivered;
    int64_t started_at;
    int64_t ended_at;
  };
  std::vector<RunResult> results;
  for (auto& job : immediate) {
    int64_t started_at = deps_.now_ms();
    auto outcome = execute_job_core(job);
    int64_t ended_at = deps_.now_ms();
    results.push_back({job.id, outcome.status, outcome.error, outcome.delivered,
                       started_at, ended_at});
  }

  // 应用结果 + 处理延迟任务
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : results) {
      auto stored = store_->get(r.job_id);
      if (!stored.has_value()) continue;
      bool should_delete = apply_job_result(*stored, r.status, r.error, r.delivered,
                                            r.started_at, r.ended_at);
      CronJobPatch p;
      p.state = stored->state;
      p.enabled = stored->enabled;
      store_->update(r.job_id, p);
      if (should_delete) {
        store_->remove(r.job_id);
      }
    }

    // 延迟任务：错峰调度
    int64_t base_now = deps_.now_ms();
    int64_t offset = stagger_ms;
    for (auto& job : deferred) {
      auto stored = store_->get(job.id);
      if (!stored.has_value() || !stored->enabled) continue;
      stored->state.next_run_at_ms = base_now + offset;
      offset += stagger_ms;
      CronJobPatch p;
      p.state = stored->state;
      store_->update(job.id, p);
    }

    recompute_next_runs_for_maintenance(deps_.now_ms());
  }
}

// ============================================================================
// scheduler_loop (对齐 OpenClaw onTimer)
// ============================================================================

void CronService::scheduler_loop() {
  while (running_) {
    // 等待到下次唤醒时间
    {
      std::unique_lock<std::mutex> lock(mutex_);
      int64_t now = deps_.now_ms();
      int64_t delay_ms = MAX_TIMER_DELAY_MS;
      if (next_wake_at_ms_.has_value()) {
        int64_t d = *next_wake_at_ms_ - now;
        if (d <= 0) {
          delay_ms = MIN_REFIRE_GAP_MS;  // 防热循环
        } else {
          delay_ms = std::min(d, MAX_TIMER_DELAY_MS);
        }
      }
      cv_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                   [this] { return !running_.load(); });
      if (!running_) break;
    }

    // 对齐 OpenClaw onTimer: state.running 防并发 tick
    // 如果上一个 tick 还在执行（长时间 isolated job），重新 arm watchdog 后继续等待
    // 防止长时间 job 导致调度器静默死亡 (issue #12025)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (tick_running_) {
        // 对齐 OpenClaw: armRunningRecheckTimer — 保持 watchdog 定时器活跃
        rearm_timer();
        continue;
      }
      tick_running_ = true;
      // 启动 watchdog：在 tick 执行期间保持定时器活跃
      rearm_timer();
    }

    // 执行 tick
    try {
      int64_t now = deps_.now_ms();

      // 收集到期任务
      std::vector<CronJob> due_jobs;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto jobs = store_->list();
        for (auto& job : jobs) {
          if (!job.enabled) continue;
          if (job.state.running_at_ms.has_value()) continue;
          if (!job.state.next_run_at_ms.has_value()) continue;
          if (now < *job.state.next_run_at_ms) continue;
          due_jobs.push_back(job);
        }

        if (due_jobs.empty()) {
          recompute_next_runs_for_maintenance(now, /*recompute_expired=*/true);
          rearm_timer();
          tick_running_ = false;
          continue;
        }

        // 标记为 running
        for (auto& job : due_jobs) {
          job.state.running_at_ms = now;
          job.state.last_error = std::nullopt;
          CronJobPatch p;
          p.state = job.state;
          store_->update(job.id, p);
        }
        rearm_timer();
      }

      // 执行任务（在锁外执行，避免阻塞）
      // 对齐 OpenClaw: 支持 maxConcurrentRuns 并发执行
      struct TickResult {
        std::string job_id;
        CronRunStatus status;
        std::optional<std::string> error;
        std::optional<bool> delivered;
        int64_t started_at;
        int64_t ended_at;
      };
      std::vector<TickResult> tick_results(due_jobs.size());

      // 解析并发数 (对齐 OpenClaw resolveRunConcurrency)
      int concurrency = 1;
      if (deps_.cron_config.has_value()) {
        int raw = deps_.cron_config->max_concurrent_runs;
        if (raw > 1) concurrency = raw;
      }
      concurrency = std::min(concurrency, static_cast<int>(due_jobs.size()));

      if (concurrency <= 1) {
        // 串行执行
        for (size_t i = 0; i < due_jobs.size(); ++i) {
          auto& job = due_jobs[i];
          int64_t started_at = deps_.now_ms();
          auto outcome = execute_job_core(job);
          int64_t ended_at = deps_.now_ms();
          tick_results[i] = {job.id, outcome.status, outcome.error, outcome.delivered,
                              started_at, ended_at};
        }
      } else {
        // 并发执行 (对齐 OpenClaw worker pool)
        std::atomic<size_t> cursor{0};
        std::vector<std::thread> workers;
        for (int w = 0; w < concurrency; ++w) {
          workers.emplace_back([&]() {
            for (;;) {
              size_t idx = cursor.fetch_add(1);
              if (idx >= due_jobs.size()) return;
              auto& job = due_jobs[idx];
              int64_t started_at = deps_.now_ms();
              auto outcome = execute_job_core(job);
              int64_t ended_at = deps_.now_ms();
              tick_results[idx] = {job.id, outcome.status, outcome.error, outcome.delivered,
                                    started_at, ended_at};
            }
          });
        }
        for (auto& t : workers) t.join();
      }

      // 应用结果
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& r : tick_results) {
          auto stored = store_->get(r.job_id);
          if (!stored.has_value()) continue;
          bool should_delete = apply_job_result(*stored, r.status, r.error, r.delivered,
                                                r.started_at, r.ended_at);
          CronJobPatch p;
          p.state = stored->state;
          p.enabled = stored->enabled;
          store_->update(r.job_id, p);
          if (should_delete) {
            store_->remove(r.job_id);
          }
        }
        recompute_next_runs_for_maintenance(deps_.now_ms());
        rearm_timer();
      }
    } catch (const std::exception& e) {
      // tick 异常不应终止调度器
    }

    // 对齐 OpenClaw onTimer finally 块：tick 结束后重新 arm 定时器
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tick_running_ = false;
      rearm_timer();  // finally: 确保定时器始终处于活跃状态
    }
  }
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<CronService> createCronService(std::shared_ptr<CronStore> store,
                                                CronServiceDeps deps) {
  return std::make_shared<CronService>(std::move(store), std::move(deps));
}

std::shared_ptr<CronStore> createMemoryCronStore() {
  return std::make_shared<MemoryCronStore>();
}

// ============================================================================
// Helper Functions
// ============================================================================

std::string cron_run_status_to_string(CronRunStatus status) {
  switch (status) {
    case CronRunStatus::OK: return "ok";
    case CronRunStatus::ERROR: return "error";
    case CronRunStatus::SKIPPED: return "skipped";
    default: return "unknown";
  }
}

CronRunStatus cron_run_status_from_string(const std::string& str) {
  if (str == "ok") return CronRunStatus::OK;
  if (str == "error") return CronRunStatus::ERROR;
  if (str == "skipped") return CronRunStatus::SKIPPED;
  return CronRunStatus::OK;
}

}  // namespace cron
}  // namespace hunterclaw
