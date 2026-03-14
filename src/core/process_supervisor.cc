#include "hunterclaw/core/process_supervisor.h"
#include "hunterclaw/core/kill_tree.h"
#include <chrono>
#include <thread>

namespace hunterclaw {

namespace {

// 获取当前时间戳(毫秒)
int64_t GetCurrentTimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

// Result helper functions
template<typename T>
core::Result<T> Ok(T value) {
  return core::Result<T>::Ok(std::move(value));
}

template<typename T>
core::Result<T> Error(const std::string& msg) {
  return core::Result<T>::Err(msg);
}

core::Result<void> OkVoid() {
  return core::Result<void>::Ok();
}

}  // namespace

//==============================================================================
// ManagedRun 实现
//==============================================================================

ManagedRun::ManagedRun(
    const std::string& run_id,
    const std::string& scope_key,
    const SupervisorConfig& config)
    : run_id_(run_id),
      scope_key_(scope_key),
      config_(config),
      state_(RunState::Starting),
      started_at_(GetCurrentTimeMs()),
      last_output_at_(GetCurrentTimeMs()) {}

ManagedRun::~ManagedRun() {
  // 如果进程还在运行,尝试优雅关闭
  if (IsRunning() && subprocess_) {
    Cancel(TerminationReason::ManualCancel);
  }
}

Result<void> ManagedRun::Spawn(const SpawnInput& input) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  if (state_ != RunState::Starting) {
    return Error<void>("Process already spawned");
  }
  
  // 使用 subprocess::popen 创建子进程
  try {
    subprocess_ = std::unique_ptr<core::subprocess::Process>(
        core::subprocess::popen(input.argv, true, true)
    );
    
    if (!subprocess_) {
      UpdateState(RunState::Exited);
      return Error<void>("Failed to spawn process");
    }
  } catch (const std::exception& e) {
    UpdateState(RunState::Exited);
    return Error<void>(std::string("Spawn failed: ") + e.what());
  }
  
  // TODO: 实现信号桥接 (需要 SignalBridge 实现)
  // if (config_.forward_signals && subprocess_->pid() > 0) { ... }
  
  // 更新状态
  UpdateState(RunState::Running);
  
  // 触发启动回调
  if (config_.callbacks.on_start) {
    RunRecord record;
    record.run_id = run_id_;
    record.scope_key = scope_key_;
    record.state = state_;
    record.started_at_ms = started_at_;
    record.pid = subprocess_->pid();
    
    config_.callbacks.on_start(record);
  }
  
  // 启动监控线程(如果配置了超时)
  if (config_.global_timeout_ms > 0 || config_.idle_timeout_ms > 0) {
    std::thread(&ManagedRun::TimeoutWatchdog, this).detach();
  }
  
  // 启动输出读取线程
  std::thread(&ManagedRun::OutputReader, this).detach();
  
  return OkVoid();
}

Result<void> ManagedRun::Cancel(TerminationReason reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  if (state_ == RunState::Exited) {
    return OkVoid();  // 已经退出
  }
  
  if (!subprocess_) {
    return Error<void>("Process not spawned");
  }
  
  // 更新状态
  UpdateState(RunState::Exiting);
  
  // 终止进程 (简化实现)
  if (subprocess_ && subprocess_->is_alive()) {
    int pid = subprocess_->pid();
    if (pid > 0) {
      // TODO: 使用 KillProcessTree 终止进程树
      // 目前只等待进程结束
      subprocess_->wait();
    }
  }
  
  return OkVoid();
}

Result<RunExit> ManagedRun::Wait(int64_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  
  // 如果已经退出,直接返回
  if (exit_) {
    return Result<RunExit>::Ok(*exit_);
  }
  
  // 等待退出
  if (timeout_ms > 0) {
    auto timeout = std::chrono::milliseconds(timeout_ms);
    if (!exit_cv_.wait_for(lock, timeout, [this] { return exit_.has_value(); })) {
      return Error<RunExit>("Wait timeout");
    }
  } else {
    exit_cv_.wait(lock, [this] { return exit_.has_value(); });
  }
  
  return Result<RunExit>::Ok(*exit_);
}

RunState ManagedRun::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::optional<pid_t> ManagedRun::GetPid() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return subprocess_ ? std::optional<pid_t>(subprocess_->pid()) : std::nullopt;
}

std::optional<RunRecord> ManagedRun::GetRecord() const {
  std::lock_guard<std::mutex> lock(mutex_);
  
  RunRecord record;
  record.run_id = run_id_;
  record.scope_key = scope_key_;
  record.state = state_;
  record.started_at_ms = started_at_;
  record.pid = subprocess_ ? std::optional<int>(subprocess_->pid()) : std::nullopt;
  
  // Note: subprocess::Process doesn't expose command/args, skip these fields
  
  if (exit_) {
    // Copy exit info to record fields
    record.exit_code = exit_->exit_code;
    record.signal = exit_->signal;
    record.termination_reason = exit_->reason;
  }
  
  return record;
}

bool ManagedRun::IsRunning() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == RunState::Running || state_ == RunState::Exiting;
}

bool ManagedRun::HasExited() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == RunState::Exited;
}

void ManagedRun::UpdateState(RunState new_state) {
  // 注意: 调用者必须持有锁
  
  RunState old_state = state_;
  state_ = new_state;
  
  // 触发状态变化回调
  if (config_.callbacks.on_state_change) {
    auto record = GetRecord();
    if (record) {
      config_.callbacks.on_state_change(*record, old_state, new_state);
    }
  }
}

void ManagedRun::TriggerExit(const RunExit& exit) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    exit_ = exit;
    UpdateState(RunState::Exited);
  }
  
  // 通知等待线程
  exit_cv_.notify_all();
  
  // 触发退出回调
  if (config_.callbacks.on_exit) {
    auto record = GetRecord();
    if (record) {
      config_.callbacks.on_exit(*record, exit);
    }
  }
}

void ManagedRun::TimeoutWatchdog() {
  while (IsRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    int64_t now = GetCurrentTimeMs();
    
    // 检查全局超时
    if (config_.global_timeout_ms > 0) {
      int64_t elapsed = now - started_at_;
      if (elapsed > config_.global_timeout_ms) {
        Cancel(TerminationReason::OverallTimeout);
        break;
      }
    }
    
    // 检查无输出超时
    if (config_.idle_timeout_ms > 0) {
      int64_t idle = now - last_output_at_;
      if (idle > config_.idle_timeout_ms) {
        Cancel(TerminationReason::NoOutputTimeout);
        break;
      }
    }
  }
}

void ManagedRun::OutputReader() {
  if (!subprocess_) {
    return;
  }
  
  // 等待进程退出
  int exit_code = subprocess_->wait();
  
  // 更新最后输出时间
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_output_at_ = GetCurrentTimeMs();
  }
  
  // 触发退出事件
  RunExit exit;
  exit.exit_code = exit_code;
  // Note: subprocess::Process.wait() doesn't capture stdout/stderr
  // Those are accumulated in real-time via read_stdout()/read_stderr()
  exit.reason = TerminationReason::Exit;
  
  TriggerExit(exit);
}

//==============================================================================
// ProcessSupervisor 实现
//==============================================================================

ProcessSupervisor::ProcessSupervisor(SupervisorRegistry* registry)
    : registry_(registry ? registry : &GetGlobalRegistry()) {}

ProcessSupervisor::~ProcessSupervisor() {
  // 取消所有运行中的进程
  std::vector<std::shared_ptr<ManagedRun>> active;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, run] : runs_) {
      if (run->IsRunning()) {
        active.push_back(run);
      }
    }
  }
  
  for (auto& run : active) {
    run->Cancel(TerminationReason::ManualCancel);
  }
}

Result<std::shared_ptr<ManagedRun>> ProcessSupervisor::Spawn(
    const SpawnInput& input,
    const SupervisorConfig& config) {
  // 生成运行ID (SpawnInput 没有 run_id 字段,总是生成新的)
  std::string run_id = "run_" + std::to_string(GetCurrentTimeMs());
  
  // 创建 ManagedRun
  std::string scope = input.scope_key ? *input.scope_key : "";
  auto run = std::make_shared<ManagedRun>(run_id, scope, config);
  
  // 启动进程
  auto result = run->Spawn(input);
  if (!result.is_ok()) {
    return Error<std::shared_ptr<ManagedRun>>(result.error);
  }
  
  // 添加到管理列表
  {
    std::lock_guard<std::mutex> lock(mutex_);
    runs_[run_id] = run;
  }
  
  // 注册到注册表
  if (config.auto_register && registry_) {
    auto record = run->GetRecord();
    if (record) {
      registry_->Add(*record);
    }
  }
  
  return Ok(run);
}

Result<void> ProcessSupervisor::Cancel(
    const std::string& run_id,
    TerminationReason reason) {
  std::shared_ptr<ManagedRun> run;
  
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runs_.find(run_id);
    if (it == runs_.end()) {
      return Error<void>("Run not found: " + run_id);
    }
    run = it->second;
  }
  
  return run->Cancel(reason);
}

size_t ProcessSupervisor::CancelScope(
    const std::string& scope_key,
    TerminationReason reason) {
  std::vector<std::shared_ptr<ManagedRun>> to_cancel;
  
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, run] : runs_) {
      if (run->GetScopeKey() == scope_key && run->IsRunning()) {
        to_cancel.push_back(run);
      }
    }
  }
  
  for (auto& run : to_cancel) {
    run->Cancel(reason);
  }
  
  return to_cancel.size();
}

std::vector<std::shared_ptr<ManagedRun>> ProcessSupervisor::GetActive(
    const std::optional<std::string>& scope_key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  
  std::vector<std::shared_ptr<ManagedRun>> active;
  for (const auto& [_, run] : runs_) {
    if (run->IsRunning()) {
      if (!scope_key || run->GetScopeKey() == *scope_key) {
        active.push_back(run);
      }
    }
  }
  
  return active;
}

std::shared_ptr<ManagedRun> ProcessSupervisor::Get(
    const std::string& run_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  
  auto it = runs_.find(run_id);
  if (it == runs_.end()) {
    return nullptr;
  }
  
  return it->second;
}

bool ProcessSupervisor::WaitAll(int64_t timeout_ms) {
  auto start = GetCurrentTimeMs();
  
  while (true) {
    // 获取活跃进程
    auto active = GetActive();
    if (active.empty()) {
      return true;
    }
    
    // 检查超时
    if (timeout_ms > 0) {
      int64_t elapsed = GetCurrentTimeMs() - start;
      if (elapsed >= timeout_ms) {
        return false;
      }
    }
    
    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 清理已退出的进程
    CleanupExited();
  }
}

void ProcessSupervisor::CleanupExited() {
  std::lock_guard<std::mutex> lock(mutex_);
  
  auto it = runs_.begin();
  while (it != runs_.end()) {
    if (it->second->HasExited()) {
      it = runs_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace hunterclaw
