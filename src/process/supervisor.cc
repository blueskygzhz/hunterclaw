#include "hunterclaw/process/supervisor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <mutex>  // ✅ 添加缺失的 mutex 头文件

namespace hunterclaw {
namespace process {

namespace {
// 生成 UUID (简化版)
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
// RunRegistry Implementation
// 对齐 OpenClaw process/supervisor/registry.ts
// ============================================================================

RunRegistry::RunRegistry(int max_exited_records)
    : max_exited_records_(max_exited_records) {}

RunRegistry::~RunRegistry() = default;

void RunRegistry::add(const RunRecord& record) {
  std::lock_guard<std::mutex> lock(mutex_);
  records_[record.run_id] = record;
}

std::optional<RunRecord> RunRegistry::get(const std::string& run_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(run_id);
  if (it == records_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<RunRecord> RunRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RunRecord> result;
  result.reserve(records_.size());
  for (const auto& pair : records_) {
    result.push_back(pair.second);
  }
  return result;
}

std::vector<RunRecord> RunRegistry::list_by_scope(const std::string& scope_key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RunRecord> result;
  for (const auto& pair : records_) {
    if (pair.second.scope_key && *pair.second.scope_key == scope_key) {
      result.push_back(pair.second);
    }
  }
  return result;
}

std::optional<RunRecord> RunRegistry::update_state(
    const std::string& run_id,
    RunState state,
    const std::map<std::string, std::string>& patch) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(run_id);
  if (it == records_.end()) {
    return std::nullopt;
  }
  
  it->second.state = state;
  it->second.updated_at_ms = now_ms();
  
  // Apply patches
  if (patch.count("pid")) {
    it->second.pid = std::stoi(patch.at("pid"));
  }
  if (patch.count("termination_reason")) {
    it->second.termination_reason = 
        termination_reason_from_string(patch.at("termination_reason"));
  }
  
  return it->second;
}

void RunRegistry::touch_output(const std::string& run_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(run_id);
  if (it != records_.end()) {
    it->second.last_output_at_ms = now_ms();
  }
}

std::optional<RunRegistry::FinalizeResult> RunRegistry::finalize(
    const std::string& run_id,
    TerminationReason reason,
    int exit_code,
    const std::string& exit_signal) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(run_id);
  if (it == records_.end()) {
    return std::nullopt;
  }
  
  bool first_finalize = (it->second.state != RunState::EXITED);
  it->second.state = RunState::EXITED;
  it->second.termination_reason = reason;
  it->second.exit_code = exit_code;
  it->second.exit_signal = exit_signal;
  it->second.updated_at_ms = now_ms();
  
  prune_exited_records();
  
  return FinalizeResult{it->second, first_finalize};
}

void RunRegistry::remove(const std::string& run_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  records_.erase(run_id);
}

void RunRegistry::prune_exited_records() {
  // Count exited records
  int exited_count = 0;
  for (const auto& pair : records_) {
    if (pair.second.state == RunState::EXITED) {
      exited_count++;
    }
  }
  
  if (exited_count <= max_exited_records_) {
    return;
  }
  
  // Remove oldest exited records
  std::vector<std::string> to_remove;
  for (const auto& pair : records_) {
    if (pair.second.state == RunState::EXITED) {
      to_remove.push_back(pair.first);
      if (to_remove.size() >= (size_t)(exited_count - max_exited_records_)) {
        break;
      }
    }
  }
  
  for (const auto& run_id : to_remove) {
    records_.erase(run_id);
  }
}

// ============================================================================
// ManagedRunImpl - Internal Implementation
// 对齐 OpenClaw process/supervisor/supervisor.ts ManagedRun
// ============================================================================

class ProcessSupervisor::ManagedRunImpl : public ManagedRun {
 public:
  ManagedRunImpl(const std::string& run_id,
                 int pid,
                 int64_t started_at_ms,
                 std::function<RunExit()> wait_fn,
                 std::function<void(TerminationReason)> cancel_fn)
      : run_id_(run_id),
        pid_(pid),
        started_at_ms_(started_at_ms),
        wait_fn_(std::move(wait_fn)),
        cancel_fn_(std::move(cancel_fn)) {}
  
  std::string get_run_id() const override { return run_id_; }
  std::optional<int> get_pid() const override { return pid_; }
  int64_t get_started_at_ms() const override { return started_at_ms_; }
  ManagedRunStdin* get_stdin() override { return nullptr; }  // Simplified
  
  RunExit wait() override {
    return wait_fn_();
  }
  
  void cancel(TerminationReason reason) override {
    cancel_fn_(reason);
  }
  
 private:
  std::string run_id_;
  int pid_;
  int64_t started_at_ms_;
  std::function<RunExit()> wait_fn_;
  std::function<void(TerminationReason)> cancel_fn_;
};

// ============================================================================
// ProcessSupervisor Implementation
// 对齐 OpenClaw process/supervisor/supervisor.ts createProcessSupervisor
// ============================================================================

ProcessSupervisor::ProcessSupervisor()
    : registry_(std::make_shared<RunRegistry>(2000)) {}

ProcessSupervisor::~ProcessSupervisor() = default;

int64_t ProcessSupervisor::clamp_timeout(std::optional<int64_t> value) {
  if (!value || *value <= 0) {
    return 0;
  }
  return std::max<int64_t>(1, *value);
}

bool ProcessSupervisor::is_timeout_reason(TerminationReason reason) const {
  return reason == TerminationReason::OVERALL_TIMEOUT ||
         reason == TerminationReason::NO_OUTPUT_TIMEOUT;
}

std::shared_ptr<ManagedRun> ProcessSupervisor::spawn(const SpawnInput& input) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  // Generate or use provided run_id
  std::string run_id = input.run_id.value_or(generate_uuid());
  
  // Replace existing scope if requested
  if (input.replace_existing_scope && input.scope_key) {
    cancel_scope(*input.scope_key, TerminationReason::MANUAL_CANCEL);
  }
  
  // Create run record
  int64_t started_at_ms = now_ms();
  RunRecord record;
  record.run_id = run_id;
  record.session_id = input.session_id;
  record.backend_id = input.backend_id;
  record.scope_key = input.scope_key;
  record.state = RunState::STARTING;
  record.started_at_ms = started_at_ms;
  record.last_output_at_ms = started_at_ms;
  record.created_at_ms = started_at_ms;
  record.updated_at_ms = started_at_ms;
  registry_->add(record);
  
  // Fork and exec process (simplified - child mode only)
  pid_t pid = fork();
  if (pid < 0) {
    // Fork failed
    registry_->finalize(run_id, TerminationReason::SPAWN_ERROR, -1, "");
    throw std::runtime_error("fork() failed");
  }
  
  if (pid == 0) {
    // Child process
    if (input.mode == SpawnMode::CHILD && !input.argv.empty()) {
      // Convert argv to C-style
      std::vector<char*> c_argv;
      for (const auto& arg : input.argv) {
        c_argv.push_back(const_cast<char*>(arg.c_str()));
      }
      c_argv.push_back(nullptr);
      
      // Change directory if specified
      if (input.cwd) {
        chdir(input.cwd->c_str());
      }
      
      // Set environment
      for (const auto& pair : input.env) {
        setenv(pair.first.c_str(), pair.second.c_str(), 1);
      }
      
      // Execute
      execvp(c_argv[0], c_argv.data());
      _exit(127);  // execvp failed
    } else if (input.mode == SpawnMode::PTY) {
      // PTY mode - execute pty_command via shell
      const char* shell = "/bin/bash";
      execl(shell, shell, "-c", input.pty_command.c_str(), nullptr);
      _exit(127);
    }
    _exit(1);
  }
  
  // Parent process
  std::map<std::string, std::string> patch;
  patch["pid"] = std::to_string(pid);
  registry_->update_state(run_id, RunState::RUNNING, patch);
  
  // Setup wait function
  auto wait_fn = [this, run_id, pid, started_at_ms, 
                  capture_output = input.capture_output]() -> RunExit {
    int status;
    waitpid(pid, &status, 0);
    
    RunExit exit;
    exit.duration_ms = now_ms() - started_at_ms;
    
    if (WIFEXITED(status)) {
      exit.reason = TerminationReason::EXIT;
      exit.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      exit.reason = TerminationReason::SIGNAL;
      exit.exit_code = -1;
      exit.exit_signal = std::to_string(WTERMSIG(status));
    } else {
      exit.reason = TerminationReason::EXIT;
      exit.exit_code = -1;
    }
    
    exit.timed_out = false;
    exit.no_output_timed_out = false;
    
    registry_->finalize(run_id, exit.reason, exit.exit_code, exit.exit_signal);
    
    // Remove from active runs
    std::lock_guard<std::mutex> lock(mutex_);
    active_runs_.erase(run_id);
    
    return exit;
  };
  
  // Setup cancel function
  auto cancel_fn = [pid](TerminationReason reason) {
    kill(pid, SIGKILL);
  };
  
  auto managed_run = std::make_shared<ManagedRunImpl>(
      run_id, pid, started_at_ms, wait_fn, cancel_fn);
  
  // Add to active runs
  ActiveRun active;
  active.run = managed_run;
  active.scope_key = input.scope_key;
  active_runs_[run_id] = active;
  
  return managed_run;
}

void ProcessSupervisor::cancel(const std::string& run_id, TerminationReason reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_runs_.find(run_id);
  if (it == active_runs_.end()) {
    return;
  }
  
  std::map<std::string, std::string> patch;
  patch["termination_reason"] = termination_reason_to_string(reason);
  registry_->update_state(run_id, RunState::EXITING, patch);
  
  it->second.run->cancel(reason);
}

void ProcessSupervisor::cancel_scope(const std::string& scope_key, 
                                      TerminationReason reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (scope_key.empty()) {
    return;
  }
  
  std::vector<std::string> to_cancel;
  for (const auto& pair : active_runs_) {
    if (pair.second.scope_key && *pair.second.scope_key == scope_key) {
      to_cancel.push_back(pair.first);
    }
  }
  
  for (const auto& run_id : to_cancel) {
    cancel(run_id, reason);
  }
}

void ProcessSupervisor::reconcile_orphans() {
  // Deliberate no-op: this supervisor uses in-memory ownership only.
  // Active runs are not recovered after process restart in the current model.
}

std::optional<RunRecord> ProcessSupervisor::get_record(const std::string& run_id) const {
  return registry_->get(run_id);
}

// ============================================================================
// Global Factory Functions
// 对齐 OpenClaw process/supervisor/index.ts
// ============================================================================

std::shared_ptr<ProcessSupervisor> createProcessSupervisor() {
  return std::make_shared<ProcessSupervisor>();
}

static std::shared_ptr<ProcessSupervisor> g_supervisor_singleton;

std::shared_ptr<ProcessSupervisor> getProcessSupervisor() {
  if (!g_supervisor_singleton) {
    g_supervisor_singleton = createProcessSupervisor();
  }
  return g_supervisor_singleton;
}

// ============================================================================
// Helper Functions
// ============================================================================

std::string run_state_to_string(RunState state) {
  switch (state) {
    case RunState::STARTING: return "starting";
    case RunState::RUNNING: return "running";
    case RunState::EXITING: return "exiting";
    case RunState::EXITED: return "exited";
    default: return "unknown";
  }
}

std::string termination_reason_to_string(TerminationReason reason) {
  switch (reason) {
    case TerminationReason::MANUAL_CANCEL: return "manual-cancel";
    case TerminationReason::OVERALL_TIMEOUT: return "overall-timeout";
    case TerminationReason::NO_OUTPUT_TIMEOUT: return "no-output-timeout";
    case TerminationReason::SPAWN_ERROR: return "spawn-error";
    case TerminationReason::SIGNAL: return "signal";
    case TerminationReason::EXIT: return "exit";
    default: return "unknown";
  }
}

RunState run_state_from_string(const std::string& str) {
  if (str == "starting") return RunState::STARTING;
  if (str == "running") return RunState::RUNNING;
  if (str == "exiting") return RunState::EXITING;
  if (str == "exited") return RunState::EXITED;
  return RunState::EXITED;
}

TerminationReason termination_reason_from_string(const std::string& str) {
  if (str == "manual-cancel") return TerminationReason::MANUAL_CANCEL;
  if (str == "overall-timeout") return TerminationReason::OVERALL_TIMEOUT;
  if (str == "no-output-timeout") return TerminationReason::NO_OUTPUT_TIMEOUT;
  if (str == "spawn-error") return TerminationReason::SPAWN_ERROR;
  if (str == "signal") return TerminationReason::SIGNAL;
  if (str == "exit") return TerminationReason::EXIT;
  return TerminationReason::EXIT;
}

}  // namespace process
}  // namespace hunterclaw
