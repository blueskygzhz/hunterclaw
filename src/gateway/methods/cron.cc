#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Cron Methods - 完全对齐 OpenClaw
// ============================================================================

// wake - 触发心跳唤醒 (OpenClaw 用于触发 cron 调度器)
core::Result<json> method_wake(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // TODO: 实现心跳触发逻辑
    // auto cron_svc = server->cron_service();
    // if (cron_svc) {
    //     cron_svc->wake();
    // }
    
    json result;
    result["ok"] = true;
    result["woke"] = true;
    result["timestamp"] = core::utils::now().time_since_epoch().count();
    return core::Result<json>::Ok(result);
}

// cron.status - 获取 cron 服务状态 (对齐 OpenClaw cron.status)
core::Result<json> method_cron_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // TODO: 从 cron 服务获取真实状态
    // auto cron_svc = server->cron_service();
    
    json result;
    result["running"] = false;  // Cron 服务是否运行
    result["jobsTotal"] = 0;    // 总任务数
    result["jobsEnabled"] = 0;  // 已启用任务数
    // result["nextWakeAtMs"] = 0;  // 下次唤醒时间（可选）
    
    return core::Result<json>::Ok(result);
}

// cron.list - 列出 cron 任务 (对齐 OpenClaw cron.list)
core::Result<json> method_cron_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 参数
    bool include_disabled = params.value("includeDisabled", false);
    int limit = params.value("limit", 50);
    int offset = params.value("offset", 0);
    std::string query = params.value("query", "");
    std::string enabled_filter = params.value("enabled", "all");  // all | enabled | disabled
    std::string sort_by = params.value("sortBy", "nextRunAtMs");   // nextRunAtMs | updatedAtMs | name
    std::string sort_dir = params.value("sortDir", "asc");         // asc | desc
    
    // 限制最大数量
    if (limit > 200) limit = 200;
    
    // TODO: 从 cron 服务获取任务列表
    json jobs = json::array();
    
    // CronJob 结构 (对齐 OpenClaw):
    // {
    //   "id": "job-uuid",
    //   "name": "Job Name",
    //   "description": "Job Description",
    //   "enabled": true,
    //   "agentId": "agent-id",
    //   "sessionKey": "session-key",
    //   "createdAtMs": 1234567890,
    //   "updatedAtMs": 1234567890,
    //   "schedule": {
    //     "kind": "at" | "every" | "cron",
    //     "timestampMs": 0,     // for "at"
    //     "intervalMs": 0,      // for "every"
    //     "cron": "0 0 * * *"   // for "cron"
    //   },
    //   "sessionTarget": "main" | "isolated",
    //   "wakeMode": "next-heartbeat" | "now",
    //   "payload": {
    //     "kind": "systemEvent" | "agentTurn",
    //     "event": {...},
    //     "messages": [...]
    //   },
    //   "delivery": {
    //     "mode": "none" | "announce" | "webhook",
    //     "channel": "...",
    //     "to": "...",
    //     "webhookUrl": "..."
    //   },
    //   "state": {
    //     "nextRunAtMs": 0,
    //     "lastRunAtMs": 0,
    //     "lastRunStatus": "ok" | "error" | "skipped",
    //     "lastError": "...",
    //     "lastDurationMs": 0,
    //     "consecutiveErrors": 0
    //   }
    // }
    
    json result;
    result["jobs"] = jobs;
    result["total"] = 0;
    result["limit"] = limit;
    result["offset"] = offset;
    
    return core::Result<json>::Ok(result);
}

// cron.add - 添加 cron 任务 (对齐 OpenClaw cron.add)
core::Result<json> method_cron_add(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 必需参数验证
    if (!params.contains("name") || !params.contains("schedule") || !params.contains("payload")) {
        return core::Result<json>::Err("Missing required parameters: name, schedule, payload");
    }
    
    std::string name = params["name"];
    std::string agent_id = params.value("agentId", "default");
    std::string session_key = params.value("sessionKey", "");
    std::string description = params.value("description", "");
    bool enabled = params.value("enabled", true);
    std::string session_target = params.value("sessionTarget", "main");
    std::string wake_mode = params.value("wakeMode", "next-heartbeat");
    
    // 解析 schedule
    json schedule = params["schedule"];
    std::string schedule_kind = schedule["kind"];
    
    // 验证 schedule 类型
    if (schedule_kind != "at" && schedule_kind != "every" && schedule_kind != "cron") {
        return core::Result<json>::Err("Invalid schedule.kind: must be 'at', 'every', or 'cron'");
    }
    
    // 解析 payload
    json payload = params["payload"];
    std::string payload_kind = payload["kind"];
    
    if (payload_kind != "systemEvent" && payload_kind != "agentTurn") {
        return core::Result<json>::Err("Invalid payload.kind: must be 'systemEvent' or 'agentTurn'");
    }
    
    // TODO: 创建 cron 任务
    // auto cron_svc = server->cron_service();
    // auto job = cron_svc->add_job(...);
    
    // 模拟返回
    json result;
    result["id"] = core::utils::generate_uuid();
    result["name"] = name;
    result["enabled"] = enabled;
    result["createdAtMs"] = core::utils::now().time_since_epoch().count();
    result["updatedAtMs"] = result["createdAtMs"];
    result["schedule"] = schedule;
    result["payload"] = payload;
    result["sessionTarget"] = session_target;
    result["wakeMode"] = wake_mode;
    
    // State
    result["state"] = {
        {"nextRunAtMs", 0},
        {"lastRunAtMs", 0},
        {"lastRunStatus", "ok"},
        {"consecutiveErrors", 0}
    };
    
    return core::Result<json>::Ok(result);
}

// cron.update - 更新 cron 任务 (对齐 OpenClaw cron.update)
core::Result<json> method_cron_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ID 参数
    std::string job_id = params.value("id", params.value("jobId", ""));
    if (job_id.empty()) {
        return core::Result<json>::Err("Missing required parameter: id or jobId");
    }
    
    // patch 对象
    if (!params.contains("patch")) {
        return core::Result<json>::Err("Missing required parameter: patch");
    }
    
    json patch = params["patch"];
    
    // TODO: 更新任务
    // auto cron_svc = server->cron_service();
    // auto job = cron_svc->update_job(job_id, patch);
    
    // 支持的 patch 字段:
    // - name, description, enabled
    // - schedule, payload, delivery
    // - sessionTarget, wakeMode, failureAlert
    
    json result;
    result["id"] = job_id;
    result["updated"] = true;
    result["updatedAtMs"] = core::utils::now().time_since_epoch().count();
    
    // 返回更新后的完整 job 对象
    // result = updated_job;
    
    return core::Result<json>::Ok(result);
}

// cron.remove - 删除 cron 任务 (对齐 OpenClaw cron.remove)
core::Result<json> method_cron_remove(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    std::string job_id = params.value("id", params.value("jobId", ""));
    if (job_id.empty()) {
        return core::Result<json>::Err("Missing required parameter: id or jobId");
    }
    
    // TODO: 删除任务
    // auto cron_svc = server->cron_service();
    // bool removed = cron_svc->remove_job(job_id);
    
    json result;
    result["removed"] = true;  // or false if not found
    
    return core::Result<json>::Ok(result);
}

// cron.run - 手动运行 cron 任务 (对齐 OpenClaw cron.run)
core::Result<json> method_cron_run(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    std::string job_id = params.value("id", params.value("jobId", ""));
    if (job_id.empty()) {
        return core::Result<json>::Err("Missing required parameter: id or jobId");
    }
    
    std::string mode = params.value("mode", "force");  // "due" | "force"
    
    // TODO: 手动触发任务
    // auto cron_svc = server->cron_service();
    // auto run_result = cron_svc->run_job(job_id, mode);
    
    // mode = "due": 仅当任务到期时运行
    // mode = "force": 强制立即运行
    
    json result;
    result["ok"] = true;
    result["enqueued"] = true;  // 是否已入队
    result["ran"] = true;       // 是否已运行
    // result["reason"] = "...";  // 如果未运行，说明原因
    result["runId"] = core::utils::generate_uuid();
    
    return core::Result<json>::Ok(result);
}

// cron.runs - 获取 cron 任务运行历史 (对齐 OpenClaw cron.runs)
core::Result<json> method_cron_runs(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    std::string scope = params.value("scope", "all");  // "job" | "all"
    std::string job_id = params.value("id", params.value("jobId", ""));
    int limit = params.value("limit", 50);
    int offset = params.value("offset", 0);
    std::string status = params.value("status", "all");  // all | ok | error | skipped
    std::string delivery_status = params.value("deliveryStatus", "");
    std::string query = params.value("query", "");
    std::string sort_dir = params.value("sortDir", "desc");
    
    // 限制
    if (limit > 200) limit = 200;
    
    // scope = "job" 时必须提供 job_id
    if (scope == "job" && job_id.empty()) {
        return core::Result<json>::Err("job_id required when scope=job");
    }
    
    // TODO: 从运行日志读取
    json entries = json::array();
    
    // CronRunLogEntry 结构 (对齐 OpenClaw):
    // {
    //   "runId": "run-uuid",
    //   "jobId": "job-uuid",
    //   "jobName": "Job Name",
    //   "startedAtMs": 1234567890,
    //   "completedAtMs": 1234567890,
    //   "durationMs": 100,
    //   "status": "ok" | "error" | "skipped",
    //   "error": "...",
    //   "delivery": {
    //     "attempted": true,
    //     "status": "delivered" | "not-delivered" | "unknown",
    //     "error": "..."
    //   }
    // }
    
    json result;
    result["entries"] = entries;
    result["total"] = 0;
    result["limit"] = limit;
    result["offset"] = offset;
    
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
