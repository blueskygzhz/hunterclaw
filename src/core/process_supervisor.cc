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
    Cancel(TerminationReason::ProcessSupervisorShutdown);
  }
}

Result<void> ManagedRun::Spawn(const SpawnInput& input) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  if (state_ != RunState::Starting) {
    return Error("Process already spawned");
  }
  
  // 创建子进程
  SubprocessOptions options;
  options.command = input.command;
  options.args = input.args;
  options.cwd = input.cwd;
  options.env = input.env;
  options.capture_stdout = true;
  options.capture_stderr = true;
  
  subprocess_ = std::make_unique<Subprocess>(options);
  auto result = subprocess_->Start();
  if (!result.ok()) {
    UpdateState(RunState::Exited);
    return result;
  }
  
  // 设置信号桥接
  if (config_.forward_signals && subprocess_->GetPid()) {
    SignalBridgeConfig bridge_config;
    bridge_config.forward_signals = {
        SignalType::SIGINT,
        SignalType::SIGTERM,
        SignalType::SIGHUP
    };
    
    signal_bridge_ = std::make_unique<SignalBridge>(
        *subprocess_->GetPid(),
        bridge_config);
    signal_bridge_->Activate();
  }
  
  // 更新状态
  UpdateState(RunState::Running);
  
  // 触发启动回调
  if (config_.callbacks.on_start) {
    RunRecord record;
    record.run_id = run_id_;
    record.scope_key = scope_key_;
    record.state = state_;
    record.started_at = started_at_;
    record.pid = subprocess_->GetPid();
    record.command = input.command;
    record.args = input.args;
    
    config_.callbacks.on_start(record);
  }
  
  // 启动监控线程(如果配置了超时)
  if (config_.global_timeout_ms > 0 || config_.idle_timeout_ms > 0) {
    std::thread(&ManagedRun::TimeoutWatchdog, this).detach();
  }
  
  // 启动输出读取线程
  std::thread(&ManagedRun::OutputReader, this).detach();
  
  return Ok();
}

Result<void> ManagedRun::Cancel(TerminationReason reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  if (state_ == RunState::Exited) {
    return Ok();  // 已经退出
  }
  
  if (!subprocess_) {
    return Error("Process not spawned");
  }
  
  // 更新状态
  UpdateState(RunState::Exiting);
  
  // 终止进程树
  if (subprocess_->GetPid()) {
    KillTreeOptions options;
    options.force = (reason == TerminationReason::Timeout ||
                     reason == TerminationReason::IdleTimeout);
    options.grace_period_ms = options.force ? 1000 : 3000;
    
    auto result = KillProcessTree(*subprocess_->GetPid(), options);
    if (!result.ok()) {
      return result;
    }
  }
  
  return Ok();
}

Result<RunExit> ManagedRun::Wait(int64_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  
  // 如果已经退出,直接返回
  if (exit_) {
    return Ok(*exit_);
  }
  
  // 等待退出
  if (timeout_ms > 0) {
    auto timeout = std::chrono::milliseconds(timeout_ms);
    if (!exit_cv_.wait_for(lock, timeout, [this] { return exit_.has_value(); })) {
      return Error("Wait timeout");
    }
  } else {
    exit_cv_.wait(lock, [this] { return exit_.has_value(); });
  }
  
  return Ok(*exit_);
}

RunState ManagedRun::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::optional<pid_t> ManagedRun::GetPid() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return subprocess_ ? subprocess_->GetPid() : std::nullopt;
}

std::optional<RunRecord> ManagedRun::GetRecord() const {
  std::lock_guard<std::mutex> lock(mutex_);
  
  RunRecord record;
  record.run_id = run_id_;
  record.scope_key = scope_key_;
  record.state = state_;
  record.started_at = started_at_;
  record.pid = subprocess_ ? subprocess_->GetPid() : std::nullopt;
  
  if (subprocess_) {
    record.command = subprocess_->GetOptions().command;
    record.args = subprocess_->GetOptions().args;
  }
  
  if (exit_) {
    record.exit_info = *exit_;
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
        Cancel(TerminationReason::Timeout);
        break;
      }
    }
    
    // 检查无输出超时
    if (config_.idle_timeout_ms > 0) {
      int64_t idle = now - last_output_at_;
      if (idle > config_.idle_timeout_ms) {
        Cancel(TerminationReason::IdleTimeout);
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
  auto result = subprocess_->Wait();
  
  // 更新最后输出时间
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_output_at_ = GetCurrentTimeMs();
  }
  
  // 触发退出事件
  RunExit exit;
  if (result.ok()) {
    auto& res = *result;
    exit.exit_code = res.exit_code;
    exit.stdout_output = res.stdout_output;
    exit.stderr_output = res.stderr_output;
    exit.reason = TerminationReason::Exited;
  } else {
    exit.exit_code = -1;
    exit.error_message = result.error();
    exit.reason = TerminationReason::Error;
  }
  
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
    run->Cancel(TerminationReason::ProcessSupervisorShutdown);
  }
}

Result<std::shared_ptr<ManagedRun>> ProcessSupervisor::Spawn(
    const SpawnInput& input,
    const SupervisorConfig& config) {
  // 生成运行ID
  std::string run_id = input.run_id.empty() 
      ? "run_" + std::to_string(GetCurrentTimeMs())
      : input.run_id;
  
  // 创建 ManagedRun
  auto run = std::make_shared<ManagedRun>(run_id, input.scope_key, config);
  
  // 启动进程
  auto result = run->Spawn(input);
  if (!result.ok()) {
    return Error(result.error());
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
      return Error("Run not found: " + run_id);
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
