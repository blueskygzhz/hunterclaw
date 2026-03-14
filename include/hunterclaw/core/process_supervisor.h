#pragma once

#include <memory>
#include <string>
#include <functional>
#include <optional>
#include <condition_variable>
#include "hunterclaw/core/supervisor_types.h"
#include "hunterclaw/core/supervisor_registry.h"
#include "hunterclaw/core/subprocess.h"
#include "hunterclaw/core/signal_bridge.h"
#include "hunterclaw/core/result.h"

namespace hunterclaw {

// 引入 core 命名空间的类型
using core::RunRecord;
using core::RunExit;
using core::RunState;
using core::SpawnInput;
using core::TerminationReason;
using core::Result;

/**
 * 进程事件回调
 */
struct ProcessCallbacks {
  // 进程启动回调
  std::function<void(const RunRecord&)> on_start;
  
  // 进程退出回调
  std::function<void(const RunRecord&, const RunExit&)> on_exit;
  
  // 状态变化回调
  std::function<void(const RunRecord&, RunState old_state, RunState new_state)> on_state_change;
  
  // 标准输出回调
  std::function<void(const std::string&)> on_stdout;
  
  // 标准错误回调
  std::function<void(const std::string&)> on_stderr;
};

/**
 * 监督器配置
 */
struct SupervisorConfig {
  // 全局超时(毫秒, 0表示无超时)
  int64_t global_timeout_ms = 0;
  
  // 无输出超时(毫秒, 0表示无超时)
  int64_t idle_timeout_ms = 0;
  
  // 是否自动转发信号
  bool forward_signals = true;
  
  // 是否自动记录到注册表
  bool auto_register = true;
  
  // 进程事件回调
  ProcessCallbacks callbacks;
};

/**
 * ManagedRun
 * 
 * 功能对齐 OpenClaw process/supervisor/managed-run.ts:
 * - 封装单个进程的生命周期管理
 * - 支持 Child/Pty 两种模式
 * - 自动超时检测(全局 + 无输出)
 * - 信号桥接(父进程 → 子进程)
 * - 状态追踪和事件通知
 * 
 * OpenClaw 对应:
 * - export class ManagedRun
 * - spawn(input): Promise<void>
 * - cancel(reason): Promise<void>
 * - wait(): Promise<RunExit>
 */
class ManagedRun {
 public:
  ManagedRun(
      const std::string& run_id,
      const std::string& scope_key,
      const SupervisorConfig& config = {});
  
  ~ManagedRun();

  // 禁止拷贝
  ManagedRun(const ManagedRun&) = delete;
  ManagedRun& operator=(const ManagedRun&) = delete;

  /**
   * 启动进程
   * @param input spawn参数
   * @return 成功返回 Ok(), 失败返回错误信息
   */
  Result<void> Spawn(const SpawnInput& input);

  /**
   * 取消进程
   * @param reason 终止原因
   * @return 成功返回 Ok(), 失败返回错误信息
   */
  Result<void> Cancel(TerminationReason reason);

  /**
   * 等待进程退出
   * @param timeout_ms 超时时间(毫秒, 0表示永久等待)
   * @return 成功返回退出结果, 超时或失败返回错误信息
   */
  Result<RunExit> Wait(int64_t timeout_ms = 0);

  /**
   * 获取运行ID
   */
  std::string GetRunId() const { return run_id_; }

  /**
   * 获取作用域键
   */
  std::string GetScopeKey() const { return scope_key_; }

  /**
   * 获取当前状态
   */
  RunState GetState() const;

  /**
   * 获取进程ID
   */
  std::optional<pid_t> GetPid() const;

  /**
   * 获取运行记录
   */
  std::optional<RunRecord> GetRecord() const;

  /**
   * 检查是否正在运行
   */
  bool IsRunning() const;

  /**
   * 检查是否已退出
   */
  bool HasExited() const;

 private:
  // 更新状态
  void UpdateState(RunState new_state);
  
  // 触发退出事件
  void TriggerExit(const RunExit& exit);
  
  // 超时监控线程
  void TimeoutWatchdog();
  
  // 读取输出线程
  void OutputReader();

  // 运行ID
  std::string run_id_;
  
  // 作用域键
  std::string scope_key_;
  
  // 配置
  SupervisorConfig config_;
  
  // 当前状态
  RunState state_;
  
  // 子进程 (使用 subprocess::Process 而不是Subprocess类)
  std::unique_ptr<core::subprocess::Process> subprocess_;
  
  // 信号桥接
  std::unique_ptr<core::SignalBridgeHandle> signal_bridge_;
  
  // 启动时间
  int64_t started_at_;
  
  // 最后输出时间(用于无输出超时)
  int64_t last_output_at_;
  
  // 退出结果
  std::optional<RunExit> exit_;
  
  // 互斥锁
  mutable std::mutex mutex_;
  
  // 条件变量(用于等待退出)
  std::condition_variable exit_cv_;
};

/**
 * ProcessSupervisor
 * 
 * 功能对齐 OpenClaw process/supervisor/process-supervisor.ts:
 * - 管理多个 ManagedRun
 * - 提供统一的 spawn/cancel 接口
 * - 与 SupervisorRegistry 集成
 * 
 * OpenClaw 对应:
 * - export class ProcessSupervisor
 * - spawn(input): ManagedRun
 * - cancel(runId, reason): Promise<void>
 * - cancelScope(scopeKey, reason): Promise<void>
 */
class ProcessSupervisor {
 public:
  ProcessSupervisor(SupervisorRegistry* registry = nullptr);
  ~ProcessSupervisor();

  // 禁止拷贝
  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

  /**
   * 启动进程
   * @param input spawn参数
   * @param config 监督器配置
   * @return 成功返回 ManagedRun, 失败返回错误信息
   */
  Result<std::shared_ptr<ManagedRun>> Spawn(
      const SpawnInput& input,
      const SupervisorConfig& config = {});

  /**
   * 取消指定进程
   * @param run_id 运行ID
   * @param reason 终止原因
   * @return 成功返回 Ok(), 失败返回错误信息
   */
  Result<void> Cancel(
      const std::string& run_id,
      TerminationReason reason = TerminationReason::ManualCancel);

  /**
   * 取消作用域内所有进程
   * @param scope_key 作用域键
   * @param reason 终止原因
   * @return 成功返回取消的进程数量
   */
  size_t CancelScope(
      const std::string& scope_key,
      TerminationReason reason = TerminationReason::ManualCancel);

  /**
   * 获取运行中的进程
   * @param scope_key 可选的作用域过滤
   * @return 运行中的 ManagedRun 列表
   */
  std::vector<std::shared_ptr<ManagedRun>> GetActive(
      const std::optional<std::string>& scope_key = std::nullopt) const;

  /**
   * 获取指定进程
   * @param run_id 运行ID
   * @return 成功返回 ManagedRun, 失败返回空
   */
  std::shared_ptr<ManagedRun> Get(const std::string& run_id) const;

  /**
   * 等待所有进程退出
   * @param timeout_ms 超时时间(毫秒, 0表示永久等待)
   * @return 是否所有进程都退出(超时返回false)
   */
  bool WaitAll(int64_t timeout_ms = 0);

  /**
   * 获取注册表
   */
  SupervisorRegistry* GetRegistry() const { return registry_; }

 private:
  // 清理已退出的进程
  void CleanupExited();

  // 注册表(可选)
  SupervisorRegistry* registry_;
  
  // 管理的进程(run_id -> ManagedRun)
  std::unordered_map<std::string, std::shared_ptr<ManagedRun>> runs_;
  
  // 互斥锁
  mutable std::mutex mutex_;
};

}  // namespace hunterclaw
