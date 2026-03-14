#pragma once

#include <json/json.h>
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace hunterclaw {
namespace tools {

/**
 * @brief Cron Tool - 定时任务管理工具
 * 
 * 对齐 OpenClaw 的 Cron 功能,支持:
 * - cron_list: 列出所有定时任务
 * - cron_create: 创建定时任务
 * - cron_delete: 删除定时任务
 * 
 * 对应 OpenClaw 文件:
 * - src/cron/service.ts
 * - src/cron/types.ts
 */
class CronTool {
 public:
  CronTool();
  ~CronTool();

  // 执行 Cron 动作
  nlohmann::json execute(const std::string& action,
                         const nlohmann::json& params);

 private:
  // ==================== cron_list ====================
  /**
   * @brief 列出所有定时任务
   * 
   * 参数:
   * - include_disabled (bool, optional): 包含已禁用的任务 (默认 false)
   * - sort_by (string, optional): 排序方式 ("nextRunAtMs", "updatedAtMs", "name")
   * - limit (int, optional): 返回数量限制
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "jobs": [
   *     {
   *       "id": "cron-123",
   *       "name": "Daily Backup",
   *       "schedule": "0 2 * * *",
   *       "enabled": true,
   *       "next_run_at_ms": 1234567890,
   *       "last_run_at_ms": 1234567800,
   *       "last_run_status": "success"
   *     }
   *   ],
   *   "count": 1
   * }
   * 
   * OpenClaw 对齐:
   * - src/cron/service.ts (list, listPage)
   * - src/cron/service/ops.ts (CronJobsSortBy)
   */
  nlohmann::json cron_list(const nlohmann::json& params);

  // ==================== cron_create ====================
  /**
   * @brief 创建定时任务
   * 
   * 参数:
   * - name (string, required): 任务名称
   * - schedule (string, required): Cron 表达式 (如 "0 2 * * *")
   * - task (string, required): 任务内容 (Agent prompt)
   * - enabled (bool, optional): 是否启用 (默认 true)
   * - session_target (string, optional): 会话目标 ("main", "isolated")
   * - wake_mode (string, optional): 唤醒模式 ("now", "next-heartbeat")
   * - delivery (object, optional): 投递配置
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "job": {
   *     "id": "cron-123",
   *     "name": "Daily Backup",
   *     "schedule": "0 2 * * *",
   *     "enabled": true,
   *     "next_run_at_ms": 1234567890
   *   }
   * }
   * 
   * OpenClaw 对齐:
   * - src/cron/service.ts (add)
   * - src/cron/types.ts (CronJobCreate)
   */
  nlohmann::json cron_create(const nlohmann::json& params);

  // ==================== cron_delete ====================
  /**
   * @brief 删除定时任务
   * 
   * 参数:
   * - id (string, required): 任务 ID
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "deleted_id": "cron-123"
   * }
   * 
   * OpenClaw 对齐:
   * - src/cron/service.ts (remove)
   */
  nlohmann::json cron_delete(const nlohmann::json& params);

  // ==================== 辅助方法 ====================
  
  // Cron 任务结构 (对齐 OpenClaw CronJob)
  struct CronJob {
    std::string id;
    std::string name;
    std::string schedule;
    std::string task;
    bool enabled;
    int64_t next_run_at_ms;
    std::optional<int64_t> last_run_at_ms;
    std::optional<std::string> last_run_status;
    int64_t created_at_ms;
    int64_t updated_at_ms;
  };
  
  // 从存储加载任务
  std::vector<CronJob> load_jobs();
  
  // 保存任务到存储
  bool save_job(const CronJob& job);
  
  // 删除任务从存储
  bool delete_job(const std::string& id);
  
  // 验证 Cron 表达式
  bool validate_cron_schedule(const std::string& schedule);
  
  // 计算下次运行时间 (对齐 OpenClaw cronstrue)
  int64_t calculate_next_run_time(const std::string& schedule);
  
  // 执行 Cron 任务
  nlohmann::json run_job(const std::string& id, bool force = false);
  
  // 排序任务 (对齐 OpenClaw CronJobsSortBy)
  std::vector<CronJob> sort_jobs(const std::vector<CronJob>& jobs,
                                  const std::string& sort_by);
  
  // 过滤任务 (enabled/disabled)
  std::vector<CronJob> filter_jobs(const std::vector<CronJob>& jobs,
                                    bool include_disabled);
  
  // 私有成员
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tools
} // namespace hunterclaw
