#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <mutex>  // ✅ 添加缺失的 mutex 头文件

namespace hunterclaw {
namespace process {

/**
 * Run State - 运行状态
 * 对齐 OpenClaw process/supervisor/types.ts RunState
 */
enum class RunState {
  STARTING,  // 启动中
  RUNNING,   // 运行中
  EXITING,   // 退出中
  EXITED     // 已退出
};

/**
 * Termination Reason - 终止原因
 * 对齐 OpenClaw process/supervisor/types.ts TerminationReason
 */
enum class TerminationReason {
  MANUAL_CANCEL,       // 手动取消
  OVERALL_TIMEOUT,     // 总体超时
  NO_OUTPUT_TIMEOUT,   // 无输出超时
  SPAWN_ERROR,         // 启动错误
  SIGNAL,              // 信号终止
  EXIT                 // 正常退出
};

/**
 * Run Record - 运行记录
 * 对齐 OpenClaw process/supervisor/types.ts RunRecord
 */
struct RunRecord {
  std::string run_id;
  std::string session_id;
  std::string backend_id;
  std::optional<std::string> scope_key;
  std::optional<int> pid;
  std::optional<int> process_group_id;
  int64_t started_at_ms;
  int64_t last_output_at_ms;
  int64_t created_at_ms;
  int64_t updated_at_ms;
  RunState state;
  std::optional<TerminationReason> termination_reason;
  std::optional<int> exit_code;
  std::optional<std::string> exit_signal;
};

/**
 * Run Exit - 退出信息
 * 对齐 OpenClaw process/supervisor/types.ts RunExit
 */
struct RunExit {
  TerminationReason reason;
  int exit_code;
  std::string exit_signal;
  int64_t duration_ms;
  std::string stdout_output;
  std::string stderr_output;
  bool timed_out;
  bool no_output_timed_out;
};

/**
 * Managed Run Stdin - 标准输入管理
 * 对齐 OpenClaw process/supervisor/types.ts ManagedRunStdin
 */
class ManagedRunStdin {
 public:
  virtual ~ManagedRunStdin() = default;
  
  virtual void write(const std::string& data) = 0;
  virtual void end() = 0;
  virtual void destroy() = 0;
  virtual bool is_destroyed() const = 0;
};

/**
 * Managed Run - 托管运行
 * 对齐 OpenClaw process/supervisor/types.ts ManagedRun
 */
class ManagedRun {
 public:
  virtual ~ManagedRun() = default;
  
  virtual std::string get_run_id() const = 0;
  virtual std::optional<int> get_pid() const = 0;
  virtual int64_t get_started_at_ms() const = 0;
  virtual ManagedRunStdin* get_stdin() = 0;
  virtual RunExit wait() = 0;  // 阻塞等待进程结束
  virtual void cancel(TerminationReason reason = TerminationReason::MANUAL_CANCEL) = 0;
};

/**
 * Spawn Mode - 启动模式
 * 对齐 OpenClaw process/supervisor/types.ts SpawnMode
 */
enum class SpawnMode {
  CHILD,  // 子进程模式
  PTY     // PTY 伪终端模式
};

/**
 * Spawn Input - 启动参数
 * 对齐 OpenClaw process/supervisor/types.ts SpawnInput
 */
struct SpawnInput {
  // Base
  std::optional<std::string> run_id;
  std::string session_id;
  std::string backend_id;
  std::optional<std::string> scope_key;
  bool replace_existing_scope = false;
  std::optional<std::string> cwd;
  std::map<std::string, std::string> env;
  std::optional<int64_t> timeout_ms;
  std::optional<int64_t> no_output_timeout_ms;
  bool capture_output = true;
  std::function<void(const std::string&)> on_stdout;
  std::function<void(const std::string&)> on_stderr;
  
  // Mode-specific
  SpawnMode mode;
  
  // Child mode
  std::vector<std::string> argv;  // for mode == CHILD
  bool windows_verbatim_arguments = false;
  std::optional<std::string> input;
  std::string stdin_mode = "inherit";  // "inherit", "pipe-open", "pipe-closed"
  
  // PTY mode
  std::string pty_command;  // for mode == PTY
};

/**
 * Run Registry - 运行记录注册表
 * 对齐 OpenClaw process/supervisor/registry.ts RunRegistry
 */
class RunRegistry {
 public:
  explicit RunRegistry(int max_exited_records = 2000);
  ~RunRegistry();
  
  void add(const RunRecord& record);
  std::optional<RunRecord> get(const std::string& run_id) const;
  std::vector<RunRecord> list() const;
  std::vector<RunRecord> list_by_scope(const std::string& scope_key) const;
  
  std::optional<RunRecord> update_state(
      const std::string& run_id,
      RunState state,
      const std::map<std::string, std::string>& patch = {});
  
  void touch_output(const std::string& run_id);
  
  struct FinalizeResult {
    RunRecord record;
    bool first_finalize;
  };
  
  std::optional<FinalizeResult> finalize(
      const std::string& run_id,
      TerminationReason reason,
      int exit_code,
      const std::string& exit_signal);
  
  void remove(const std::string& run_id);

 private:
  void prune_exited_records();
  
  std::map<std::string, RunRecord> records_;
  int max_exited_records_;
  mutable std::mutex mutex_;
};

/**
 * Process Supervisor - 进程监督器
 * 对齐 OpenClaw process/supervisor/supervisor.ts ProcessSupervisor
 * 
 * 核心功能:
 * 1. spawn() - 启动托管进程
 * 2. cancel() - 取消单个进程
 * 3. cancelScope() - 取消 scope 内所有进程
 * 4. reconcileOrphans() - 协调孤儿进程
 * 5. getRecord() - 获取运行记录
 * 
 * 超时管理:
 * - Overall timeout (总体超时)
 * - No-output timeout (无输出超时)
 * 
 * 状态转换:
 * STARTING → RUNNING → EXITING → EXITED
 */
class ProcessSupervisor {
 public:
  ProcessSupervisor();
  ~ProcessSupervisor();
  
  /**
   * spawn - 启动托管进程
   * 对齐 OpenClaw ProcessSupervisor.spawn()
   * 
   * 功能:
   * 1. 分配 run_id
   * 2. 替换现有 scope (如果指定)
   * 3. 创建运行记录
   * 4. 启动子进程或 PTY
   * 5. 设置超时定时器
   * 6. 捕获 stdout/stderr
   * 7. 返回 ManagedRun
   */
  std::shared_ptr<ManagedRun> spawn(const SpawnInput& input);
  
  /**
   * cancel - 取消单个进程
   * 对齐 OpenClaw ProcessSupervisor.cancel()
   */
  void cancel(const std::string& run_id, 
              TerminationReason reason = TerminationReason::MANUAL_CANCEL);
  
  /**
   * cancelScope - 取消 scope 内所有进程
   * 对齐 OpenClaw ProcessSupervisor.cancelScope()
   */
  void cancel_scope(const std::string& scope_key,
                    TerminationReason reason = TerminationReason::MANUAL_CANCEL);
  
  /**
   * reconcileOrphans - 协调孤儿进程
   * 对齐 OpenClaw ProcessSupervisor.reconcileOrphans()
   * 
   * 注意: 当前实现为 no-op,因为使用内存所有权模型
   */
  void reconcile_orphans();
  
  /**
   * getRecord - 获取运行记录
   * 对齐 OpenClaw ProcessSupervisor.getRecord()
   */
  std::optional<RunRecord> get_record(const std::string& run_id) const;

 private:
  // 内部 ManagedRun 实现
  class ManagedRunImpl;
  
  struct ActiveRun {
    std::shared_ptr<ManagedRun> run;
    std::optional<std::string> scope_key;
  };
  
  int64_t clamp_timeout(std::optional<int64_t> value);
  bool is_timeout_reason(TerminationReason reason) const;
  
  std::shared_ptr<RunRegistry> registry_;
  std::map<std::string, ActiveRun> active_runs_;
  mutable std::mutex mutex_;
};

/**
 * createProcessSupervisor - 创建进程监督器 (工厂函数)
 * 对齐 OpenClaw createProcessSupervisor()
 */
std::shared_ptr<ProcessSupervisor> createProcessSupervisor();

/**
 * getProcessSupervisor - 获取全局进程监督器单例
 * 对齐 OpenClaw getProcessSupervisor()
 */
std::shared_ptr<ProcessSupervisor> getProcessSupervisor();

// 辅助函数
std::string run_state_to_string(RunState state);
std::string termination_reason_to_string(TerminationReason reason);
RunState run_state_from_string(const std::string& str);
TerminationReason termination_reason_from_string(const std::string& str);

}  // namespace process
}  // namespace hunterclaw
