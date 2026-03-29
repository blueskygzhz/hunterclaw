#pragma once

#include "hunterclaw/cron/service.h"
#include "hunterclaw/cron/types.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/infra/heartbeat_runner.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>

namespace hunterclaw {
namespace cron {

/**
 * Gateway Cron Integration
 * 
 * 提供 WebSocket API 和 Agent Tool 接口
 */
class GatewayCronIntegration {
public:
    explicit GatewayCronIntegration(std::shared_ptr<CronService> service);

    // ========================================================================
    // Heartbeat reply_fn injection
    // Aligned with openclaw: server-cron.ts runHeartbeatOnce callback
    //
    // The gateway must call set_heartbeat_reply_fn() after construction to
    // wire up the actual LLM call (ReplyGenerator::get_reply_from_config).
    // This is the C++ equivalent of the lambda in server-cron.ts that calls
    // runHeartbeatOnce({ cfg, agentId, sessionKey, heartbeat, deps }).
    // ========================================================================
    using HeartbeatReplyFn = std::function<
        infra::HeartbeatReplyResult(const infra::HeartbeatReplyRequest&)>;

    void set_heartbeat_reply_fn(HeartbeatReplyFn fn) {
        heartbeat_reply_fn_ = std::move(fn);
    }

    const HeartbeatReplyFn& heartbeat_reply_fn() const {
        return heartbeat_reply_fn_;
    }
    
    // ========================================================================
    // WebSocket Handlers (对齐 OpenClaw gateway/server-methods/cron.ts)
    // ========================================================================
    
    /**
     * wake - 触发心跳
     * 
     * Request: { "mode": "now" | "next-heartbeat", "text": string }
     * Response: { "success": true }
     */
    nlohmann::json handle_wake(const nlohmann::json& params);
    
    /**
     * cron.status - 获取服务状态
     * 
     * Request: {}
     * Response: { "running": bool, "totalJobs": int, ... }
     */
    nlohmann::json handle_status(const nlohmann::json& params);
    
    /**
     * cron.list - 列出任务
     * 
     * Request: { "includeDisabled"?: bool }
     * Response: { "jobs": Job[] }
     */
    nlohmann::json handle_list(const nlohmann::json& params);
    
    /**
     * cron.add - 添加任务
     * 
     * Request: { "job": JobCreate }
     * Response: { "job": Job }
     */
    nlohmann::json handle_add(const nlohmann::json& params);
    
    /**
     * cron.update - 更新任务
     * 
     * Request: { "id": string, "patch": JobPatch }
     * Response: { "job": Job }
     */
    nlohmann::json handle_update(const nlohmann::json& params);
    
    /**
     * cron.remove - 删除任务
     * 
     * Request: { "id": string }
     * Response: { "success": true }
     */
    nlohmann::json handle_remove(const nlohmann::json& params);
    
    /**
     * cron.run - 手动运行任务
     * 
     * Request: { "id": string, "force"?: bool }
     * Response: { "success": true }
     */
    nlohmann::json handle_run(const nlohmann::json& params);
    
    /**
     * cron.runs - 获取运行日志
     * 
     * Request: { "id": string, "limit"?: int }
     * Response: { "runs": RunLogEntry[] }
     */
    nlohmann::json handle_runs(const nlohmann::json& params);
    
    // ========================================================================
    // Agent Tool Interface (对齐 OpenClaw agents/tools/cron-tool.ts)
    // ========================================================================
    
    /**
     * Cron Agent Tool
     * 
     * 允许 Agent 管理 cron 任务
     * 
     * Parameters:
     * {
     *   "action": "add" | "update" | "remove" | "run" | "list" | "status",
     *   "id"?: string,
     *   "job"?: JobCreate,
     *   "patch"?: JobPatch,
     *   "force"?: bool
     * }
     */
    nlohmann::json execute_agent_tool(const nlohmann::json& args);
    
    // ========================================================================
    // Multi-tenant Context (🏢 对齐 OpenClaw)
    // ========================================================================
    
    /**
     * 设置执行上下文 (多租户支持)
     * 
     * @param agent_id Agent ID (用于隔离 Cron Job)
     * @param session_key Session Key (可选)
     */
    void set_context(const std::string& agent_id, const std::optional<std::string>& session_key = std::nullopt);
    
private:
    std::shared_ptr<CronService> service_;
    HeartbeatReplyFn heartbeat_reply_fn_;  // injected by gateway; calls ReplyGenerator::get_reply_from_config

    // JSON conversion helpers
    static nlohmann::json job_to_json(const CronJob& job);
    static CronJobCreate json_to_job_create(const nlohmann::json& j);
    static CronJobPatch json_to_job_patch(const nlohmann::json& j);
};

} // namespace cron
} // namespace hunterclaw
