#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include "hunterclaw/core/supervisor_types.h"
#include "hunterclaw/core/result.h"

namespace hunterclaw {

/**
 * RunRecord 存储配置
 */
struct RegistryConfig {
  // 最大保留记录数
  size_t max_records = 2000;
  
  // 是否启用自动修剪
  bool auto_prune = true;
  
  // 修剪触发阈值(超过此数量触发修剪)
  size_t prune_threshold = 2500;
};

/**
 * 运行记录查询选项
 */
struct QueryOptions {
  // 按 scope_key 过滤
  std::optional<std::string> scope_key;
  
  // 按 run_id 过滤
  std::optional<std::string> run_id;
  
  // 按状态过滤
  std::optional<core::RunState> state;
  
  // 是否只返回活跃记录(Starting/Running/Exiting)
  bool active_only = false;
  
  // 最大返回数量
  std::optional<size_t> limit;
  
  // 按时间排序(true=升序, false=降序)
  bool ascending = false;
};

/**
 * SupervisorRegistry
 * 
 * 功能对齐 OpenClaw process/supervisor/registry.ts:
 * - 存储所有 ManagedRun 的运行记录
 * - 支持按 scope_key 查询
 * - 自动修剪旧记录(保留 2000 条)
 * - 线程安全的 CRUD 操作
 * 
 * OpenClaw 对应:
 * - export class SupervisorRegistry
 * - private records: Map<string, RunRecord>
 * - getAll(scopeKey?): RunRecord[]
 * - getActive(scopeKey?): RunRecord[]
 * - prune(): void
 */
class SupervisorRegistry {
 public:
  explicit SupervisorRegistry(const RegistryConfig& config = {});
  ~SupervisorRegistry() = default;

  // 禁止拷贝
  SupervisorRegistry(const SupervisorRegistry&) = delete;
  SupervisorRegistry& operator=(const SupervisorRegistry&) = delete;

  /**
   * 添加运行记录
   * @param record 运行记录
   * @return 成功返回 Ok(), 失败返回错误信息
   */
  core::Result<void> Add(const core::RunRecord& record);

  /**
   * 更新运行记录
   * @param run_id 运行ID
   * @param updater 更新函数
   * @return 成功返回更新后的记录, 失败返回错误信息
   */
  core::Result<core::RunRecord> Update(
      const std::string& run_id,
      std::function<void(core::RunRecord&)> updater);

  /**
   * 删除运行记录
   * @param run_id 运行ID
   * @return 成功返回 Ok(), 失败返回错误信息
   */
  core::Result<void> Remove(const std::string& run_id);

  /**
   * 获取运行记录
   * @param run_id 运行ID
   * @return 成功返回记录, 失败返回空
   */
  std::optional<core::RunRecord> Get(const std::string& run_id) const;

  /**
   * 查询运行记录
   * @param options 查询选项
   * @return 符合条件的记录列表
   */
  std::vector<core::RunRecord> Query(const QueryOptions& options = {}) const;

  /**
   * 获取所有记录
   * @return 所有记录列表
   */
  std::vector<core::RunRecord> GetAll() const;

  /**
   * 获取活跃记录(Starting/Running/Exiting)
   * @return 活跃记录列表
   */
  std::vector<core::RunRecord> GetActive() const;

  /**
   * 按 scope_key 获取记录
   * @param scope_key 作用域键
   * @return 符合条件的记录列表
   */
  std::vector<core::RunRecord> GetByScope(const std::string& scope_key) const;

  /**
   * 按 scope_key 获取活跃记录
   * @param scope_key 作用域键
   * @return 符合条件的活跃记录列表
   */
  std::vector<core::RunRecord> GetActiveByScope(const std::string& scope_key) const;

  /**
   * 修剪旧记录
   * 保留最近的 max_records 条记录
   * @return 删除的记录数量
   */
  size_t Prune();

  /**
   * 清空所有记录
   */
  void Clear();

  /**
   * 获取记录总数
   */
  size_t Size() const;

  /**
   * 获取活跃记录数量
   */
  size_t ActiveCount() const;

  /**
   * 获取配置
   */
  const RegistryConfig& GetConfig() const { return config_; }

 private:
  // 检查是否需要自动修剪
  void CheckAutoPrune();

  // 判断记录是否活跃
  static bool IsActive(const core::RunRecord& record);

  // 记录存储(run_id -> RunRecord)
  std::unordered_map<std::string, core::RunRecord> records_;
  
  // 配置
  RegistryConfig config_;
  
  // 互斥锁
  mutable std::mutex mutex_;
};

/**
 * 全局注册表单例
 */
SupervisorRegistry& GetGlobalRegistry();

}  // namespace hunterclaw
