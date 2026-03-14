#include "hunterclaw/cron/service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace hunterclaw {
namespace cron {

namespace {
std::string generate_uuid() {
  std::stringstream ss;
  ss << std::chrono::system_clock::now().time_since_epoch().count()
     << "-" << rand();
  return ss.str();
}

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

// ============================================================================
// CronScheduleCalculator Implementation
// 对齐 OpenClaw cron/schedule.ts
// ============================================================================

std::optional<int64_t> CronScheduleCalculator::compute_next_run(
    const CronSchedule& schedule,
    int64_t now_ms,
    std::optional<int64_t> last_run_ms) {
  switch (schedule.kind) {
    case CronSchedule::Kind::AT: {
      // Parse ISO 8601 timestamp
      // Simplified: assume timestamp in milliseconds
      int64_t at_ms = std::stoll(schedule.at);
      return (at_ms > now_ms) ? std::optional<int64_t>(at_ms) : std::nullopt;
    }
    
    case CronSchedule::Kind::EVERY: {
      if (schedule.every_ms <= 0) {
        return std::nullopt;
      }
      
      int64_t anchor = schedule.anchor_ms.value_or(now_ms);
      if (now_ms < anchor) {
        return anchor;
      }
      
      int64_t elapsed = now_ms - anchor;
      int64_t steps = (elapsed + schedule.every_ms - 1) / schedule.every_ms;
      if (steps < 1) steps = 1;
      
      return anchor + steps * schedule.every_ms;
    }
    
    case CronSchedule::Kind::CRON: {
      // Simplified cron expression parsing
      // In production, use a proper cron library (e.g., ccronexpr)
      // For now, return next hour as placeholder
      int64_t next_hour = ((now_ms / 3600000) + 1) * 3600000;
      return next_hour;
    }
  }
  
  return std::nullopt;
}

bool CronScheduleCalculator::is_overdue(int64_t next_run_at_ms, int64_t now_ms, int64_t grace_ms) {
  return now_ms >= (next_run_at_ms + grace_ms);
}

bool CronScheduleCalculator::should_skip(int64_t next_run_at_ms, int64_t now_ms, int64_t skip_threshold_ms) {
  return (now_ms - next_run_at_ms) > skip_threshold_ms;
}

// ============================================================================
// Memory Cron Store Implementation
// 对齐 OpenClaw cron/store.ts
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
    if (it == jobs_.end()) {
      return std::nullopt;
    }
    return *it;
  }
  
  CronJob add(const CronJobCreate& create) override {
    std::lock_guard<std::mutex> lock(mutex_);
    
    CronJob job;
    job.id = generate_uuid();
    job.name = create.name;
    job.session_target = create.session_target;
    job.wake_mode = create.wake_mode;
    job.schedule = create.schedule;
    job.payload = create.payload;
    job.delivery = create.delivery;
    job.failure_alert = create.failure_alert;
    job.enabled = create.enabled;
    job.created_at_ms = now_ms();
    job.updated_at_ms = now_ms();
    job.state = create.state.value_or(CronJobState{});
    
    jobs_.push_back(job);
    return job;
  }
  
  std::optional<CronJob> update(const std::string& job_id, const CronJobPatch& patch) override {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::find_if(jobs_.begin(), jobs_.end(),
                           [&](const CronJob& job) { return job.id == job_id; });
    if (it == jobs_.end()) {
      return std::nullopt;
    }
    
    if (patch.name) it->name = *patch.name;
    if (patch.enabled) it->enabled = *patch.enabled;
    if (patch.schedule) it->schedule = *patch.schedule;
    if (patch.payload) it->payload = *patch.payload;
    if (patch.delivery) it->delivery = *patch.delivery;
    if (patch.failure_alert) it->failure_alert = *patch.failure_alert;
    if (patch.state) {
      // Merge state
      auto& state = it->state;
      auto& new_state = *patch.state;
      if (new_state.next_run_at_ms) state.next_run_at_ms = new_state.next_run_at_ms;
      if (new_state.running_at_ms) state.running_at_ms = new_state.running_at_ms;
      if (new_state.last_run_at_ms) state.last_run_at_ms = new_state.last_run_at_ms;
      if (new_state.last_run_status) state.last_run_status = new_state.last_run_status;
      if (new_state.last_error) state.last_error = new_state.last_error;
      if (new_state.last_duration_ms) state.last_duration_ms = new_state.last_duration_ms;
      state.consecutive_errors = new_state.consecutive_errors;
      state.schedule_error_count = new_state.schedule_error_count;
      state.last_delivered = new_state.last_delivered;
    }
    
    it->updated_at_ms = now_ms();
    return *it;
  }
  
  bool remove(const std::string& job_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(),
                           [&](const CronJob& job) { return job.id == job_id; });
    if (it == jobs_.end()) {
      return false;
    }
    jobs_.erase(it);
    return true;
  }
  
  void save() override {
    // Memory store doesn't need to save
  }
  
 private:
  std::vector<CronJob> jobs_;
  std::mutex mutex_;
};

// ============================================================================
// CronService Implementation
// 对齐 OpenClaw cron/service.ts
// ============================================================================

CronService::CronService(std::shared_ptr<CronStore> store)
    : store_(std::move(store)) {}

CronService::~CronService() {
  stop();
}

void CronService::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_) {
    return;
  }
  
  running_ = true;
  scheduler_thread_ = std::thread(&CronService::scheduler_loop, this);
}

void CronService::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return;
    }
    running_ = false;
  }
  
  if (scheduler_thread_.joinable()) {
    scheduler_thread_.join();
  }
}

CronJob CronService::add_job(const CronJobCreate& create) {
  auto job = store_->add(create);
  
  // Compute next run time
  int64_t now = now_ms();
  auto next_run = CronScheduleCalculator::compute_next_run(job.schedule, now);
  if (next_run) {
    CronJobState state = job.state;
    state.next_run_at_ms = *next_run;
    
    CronJobPatch patch;
    patch.state = state;
    store_->update(job.id, patch);
    
    std::lock_guard<std::mutex> lock(mutex_);
    next_run_times_[job.id] = *next_run;
  }
  
  rearm_timer();
  return job;
}

std::optional<CronJob> CronService::update_job(const std::string& job_id, const CronJobPatch& patch) {
  auto result = store_->update(job_id, patch);
  if (result) {
    rearm_timer();
  }
  return result;
}

bool CronService::delete_job(const std::string& job_id) {
  bool removed = store_->remove(job_id);
  if (removed) {
    std::lock_guard<std::mutex> lock(mutex_);
    next_run_times_.erase(job_id);
    rearm_timer();
  }
  return removed;
}

std::optional<CronJob> CronService::get_job(const std::string& job_id) {
  return store_->get(job_id);
}

std::vector<CronJob> CronService::list_jobs(bool enabled_only) {
  auto jobs = store_->list();
  if (!enabled_only) {
    return jobs;
  }
  
  std::vector<CronJob> result;
  std::copy_if(jobs.begin(), jobs.end(), std::back_inserter(result),
               [](const CronJob& job) { return job.enabled; });
  return result;
}

void CronService::force_run(const std::string& job_id) {
  auto job = store_->get(job_id);
  if (!job) {
    return;
  }
  
  execute_job(*job);
}

void CronService::set_executor(std::function<void(const CronJob&)> executor) {
  std::lock_guard<std::mutex> lock(mutex_);
  executor_ = std::move(executor);
}

void CronService::scheduler_loop() {
  while (running_) {
    int64_t now = now_ms();
    
    // Find jobs that need to run
    std::vector<CronJob> jobs_to_run;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& pair : next_run_times_) {
        if (pair.second <= now) {
          auto job = store_->get(pair.first);
          if (job && job->enabled) {
            jobs_to_run.push_back(*job);
          }
        }
      }
    }
    
    // Execute jobs
    for (const auto& job : jobs_to_run) {
      execute_job(job);
      
      // Compute next run time
      auto next_run = CronScheduleCalculator::compute_next_run(job.schedule, now_ms());
      if (next_run) {
        std::lock_guard<std::mutex> lock(mutex_);
        next_run_times_[job.id] = *next_run;
      }
    }
    
    // Sleep for 1 second
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void CronService::execute_job(const CronJob& job) {
  int64_t start_ms = now_ms();
  
  // Update running state
  CronJobState state = job.state;
  state.running_at_ms = start_ms;
  update_job_state(job.id, state);
  
  try {
    // Call executor
    if (executor_) {
      executor_(job);
    }
    
    // Update success state
    int64_t end_ms = now_ms();
    state.running_at_ms = std::nullopt;
    state.last_run_at_ms = start_ms;
    state.last_run_status = CronRunStatus::OK;
    state.last_duration_ms = end_ms - start_ms;
    state.consecutive_errors = 0;
    state.last_error = std::nullopt;
    
  } catch (const std::exception& e) {
    // Update error state
    int64_t end_ms = now_ms();
    state.running_at_ms = std::nullopt;
    state.last_run_at_ms = start_ms;
    state.last_run_status = CronRunStatus::ERROR;
    state.last_duration_ms = end_ms - start_ms;
    state.consecutive_errors++;
    state.last_error = e.what();
  }
  
  update_job_state(job.id, state);
}

void CronService::update_job_state(const std::string& job_id, const CronJobState& state) {
  CronJobPatch patch;
  patch.state = state;
  store_->update(job_id, patch);
}

void CronService::rearm_timer() {
  // Wake up scheduler thread
  // In a real implementation, use condition variable
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<CronService> createCronService(std::shared_ptr<CronStore> store) {
  return std::make_shared<CronService>(std::move(store));
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
