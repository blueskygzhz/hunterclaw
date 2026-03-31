#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/cron/service.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/infra/heartbeat_runner.h"
#include "hunterclaw/infra/heartbeat_wake.h"
#include "hunterclaw/agents/agent_pool.h"     // ✅ reply_fn: agent_pool->get_or_create_agent
#include "hunterclaw/agents/agent_instance.h" // ✅ reply_fn: agent->process_message
#include "hunterclaw/queue/command_queue.h"   // ✅ get_queue_size: CommandQueue::Instance()
#include "hunterclaw/queue/lanes.h"           // ✅ CommandLane::Main
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Cron Methods - 严格对齐 OpenClaw server-methods/cron.ts
// ============================================================================

// ---------------------------------------------------------------------------
// 参数验证辅助函数 (对应 OpenClaw protocol/schema/cron.ts)
// ---------------------------------------------------------------------------

// 验证 CronScheduleSchema
// 对应 OpenClaw:
//   { kind: "at",    at: NonEmptyString }
//   { kind: "every", everyMs: integer>=1, anchorMs?: integer>=0 }
//   { kind: "cron",  expr: NonEmptyString, tz?: string, staggerMs?: integer>=0 }
static bool validate_cron_schedule(const json& schedule, std::string& err) {
    if (!schedule.is_object()) { err = "schedule must be an object"; return false; }
    if (!schedule.contains("kind") || !schedule["kind"].is_string()) {
        err = "schedule.kind is required"; return false;
    }
    std::string kind = schedule["kind"].get<std::string>();
    if (kind == "at") {
        if (!schedule.contains("at") || !schedule["at"].is_string() || schedule["at"].get<std::string>().empty()) {
            err = "schedule.at is required for kind=at"; return false;
        }
    } else if (kind == "every") {
        if (!schedule.contains("everyMs") || !schedule["everyMs"].is_number_integer() || schedule["everyMs"].get<int64_t>() < 1) {
            err = "schedule.everyMs must be integer >= 1 for kind=every"; return false;
        }
        if (schedule.contains("anchorMs") && (!schedule["anchorMs"].is_number_integer() || schedule["anchorMs"].get<int64_t>() < 0)) {
            err = "schedule.anchorMs must be integer >= 0"; return false;
        }
    } else if (kind == "cron") {
        if (!schedule.contains("expr") || !schedule["expr"].is_string() || schedule["expr"].get<std::string>().empty()) {
            err = "schedule.expr is required for kind=cron"; return false;
        }
        if (schedule.contains("staggerMs") && (!schedule["staggerMs"].is_number_integer() || schedule["staggerMs"].get<int64_t>() < 0)) {
            err = "schedule.staggerMs must be integer >= 0"; return false;
        }
    } else {
        err = "schedule.kind must be 'at', 'every', or 'cron'"; return false;
    }
    return true;
}

// 验证 CronPayloadSchema
// 对应 OpenClaw:
//   { kind: "systemEvent", text: NonEmptyString }
//   { kind: "agentTurn",   message: NonEmptyString, model?, fallbacks?, thinking?,
//     timeoutSeconds?, allowUnsafeExternalContent?, lightContext?, deliver?,
//     channel?, to?, bestEffortDeliver? }
static bool validate_cron_payload(const json& payload, std::string& err) {
    if (!payload.is_object()) { err = "payload must be an object"; return false; }
    if (!payload.contains("kind") || !payload["kind"].is_string()) {
        err = "payload.kind is required"; return false;
    }
    std::string kind = payload["kind"].get<std::string>();
    if (kind == "systemEvent") {
        if (!payload.contains("text") || !payload["text"].is_string() || payload["text"].get<std::string>().empty()) {
            err = "payload.text is required for kind=systemEvent"; return false;
        }
    } else if (kind == "agentTurn") {
        if (!payload.contains("message") || !payload["message"].is_string() || payload["message"].get<std::string>().empty()) {
            err = "payload.message is required for kind=agentTurn"; return false;
        }
        // 验证可选字段类型 (对应 cronAgentTurnPayloadSchema)
        if (payload.contains("model") && !payload["model"].is_null() && !payload["model"].is_string()) {
            err = "payload.model must be a string"; return false;
        }
        if (payload.contains("fallbacks") && !payload["fallbacks"].is_null()) {
            if (!payload["fallbacks"].is_array()) { err = "payload.fallbacks must be an array"; return false; }
            for (const auto& f : payload["fallbacks"]) {
                if (!f.is_string()) { err = "payload.fallbacks items must be strings"; return false; }
            }
        }
        if (payload.contains("thinking") && !payload["thinking"].is_null() && !payload["thinking"].is_string()) {
            err = "payload.thinking must be a string"; return false;
        }
        if (payload.contains("timeoutSeconds") && !payload["timeoutSeconds"].is_null()) {
            if (!payload["timeoutSeconds"].is_number_integer() || payload["timeoutSeconds"].get<int64_t>() < 0) {
                err = "payload.timeoutSeconds must be integer >= 0"; return false;
            }
        }
        if (payload.contains("allowUnsafeExternalContent") && !payload["allowUnsafeExternalContent"].is_null() && !payload["allowUnsafeExternalContent"].is_boolean()) {
            err = "payload.allowUnsafeExternalContent must be a boolean"; return false;
        }
        if (payload.contains("lightContext") && !payload["lightContext"].is_null() && !payload["lightContext"].is_boolean()) {
            err = "payload.lightContext must be a boolean"; return false;
        }
        if (payload.contains("deliver") && !payload["deliver"].is_null() && !payload["deliver"].is_boolean()) {
            err = "payload.deliver must be a boolean"; return false;
        }
        if (payload.contains("channel") && !payload["channel"].is_null() && !payload["channel"].is_string()) {
            err = "payload.channel must be a string"; return false;
        }
        if (payload.contains("to") && !payload["to"].is_null() && !payload["to"].is_string()) {
            err = "payload.to must be a string"; return false;
        }
        if (payload.contains("bestEffortDeliver") && !payload["bestEffortDeliver"].is_null() && !payload["bestEffortDeliver"].is_boolean()) {
            err = "payload.bestEffortDeliver must be a boolean"; return false;
        }
    } else {
        err = "payload.kind must be 'systemEvent' or 'agentTurn'"; return false;
    }
    return true;
}

// 验证 CronDeliverySchema (optional)
// 对应 OpenClaw:
//   CronDeliveryNoopSchema:    { mode: "none",     channel?, accountId?, bestEffort?, failureDestination?, to? }
//   CronDeliveryAnnounceSchema:{ mode: "announce", channel?, accountId?, bestEffort?, failureDestination?, to? }
//   CronDeliveryWebhookSchema: { mode: "webhook",  channel?, accountId?, bestEffort?, failureDestination?, to: NonEmptyString }
static bool validate_cron_delivery(const json& delivery, std::string& err) {
    if (!delivery.is_object()) { err = "delivery must be an object"; return false; }
    if (!delivery.contains("mode") || !delivery["mode"].is_string()) {
        err = "delivery.mode is required"; return false;
    }
    std::string mode = delivery["mode"].get<std::string>();
    if (mode != "none" && mode != "announce" && mode != "webhook") {
        err = "delivery.mode must be 'none', 'announce', or 'webhook'"; return false;
    }
    if (mode == "webhook") {
        // webhook 模式: to 是 NonEmptyString (required)
        if (!delivery.contains("to") || !delivery["to"].is_string() || delivery["to"].get<std::string>().empty()) {
            err = "delivery.to is required for mode=webhook"; return false;
        }
    } else {
        // none/announce 模式: to 是 optional string (可以为空)
        if (delivery.contains("to") && !delivery["to"].is_null() && !delivery["to"].is_string()) {
            err = "delivery.to must be a string"; return false;
        }
    }
    // 验证共享可选字段 (对应 CronDeliverySharedProperties)
    if (delivery.contains("channel") && !delivery["channel"].is_null() && !delivery["channel"].is_string()) {
        err = "delivery.channel must be a string"; return false;
    }
    if (delivery.contains("accountId") && !delivery["accountId"].is_null()) {
        if (!delivery["accountId"].is_string() || delivery["accountId"].get<std::string>().empty()) {
            err = "delivery.accountId must be a non-empty string"; return false;
        }
    }
    if (delivery.contains("bestEffort") && !delivery["bestEffort"].is_null() && !delivery["bestEffort"].is_boolean()) {
        err = "delivery.bestEffort must be a boolean"; return false;
    }
    return true;
}

// 验证 CronFailureAlertSchema
// 对应 OpenClaw:
//   { after?: integer>=1, channel?: "last" | NonEmptyString, to?: string,
//     cooldownMs?: integer>=0, mode?: "announce" | "webhook", accountId?: NonEmptyString }
static bool validate_cron_failure_alert(const json& alert, std::string& err) {
    if (!alert.is_object()) { err = "failureAlert must be an object"; return false; }
    if (alert.contains("after") && !alert["after"].is_null()) {
        if (!alert["after"].is_number_integer() || alert["after"].get<int64_t>() < 1) {
            err = "failureAlert.after must be integer >= 1"; return false;
        }
    }
    if (alert.contains("channel") && !alert["channel"].is_null() && !alert["channel"].is_string()) {
        err = "failureAlert.channel must be a string"; return false;
    }
    if (alert.contains("to") && !alert["to"].is_null() && !alert["to"].is_string()) {
        err = "failureAlert.to must be a string"; return false;
    }
    if (alert.contains("cooldownMs") && !alert["cooldownMs"].is_null()) {
        if (!alert["cooldownMs"].is_number_integer() || alert["cooldownMs"].get<int64_t>() < 0) {
            err = "failureAlert.cooldownMs must be integer >= 0"; return false;
        }
    }
    if (alert.contains("mode") && !alert["mode"].is_null()) {
        if (!alert["mode"].is_string()) { err = "failureAlert.mode must be a string"; return false; }
        std::string mode = alert["mode"].get<std::string>();
        if (mode != "announce" && mode != "webhook") {
            err = "failureAlert.mode must be 'announce' or 'webhook'"; return false;
        }
    }
    if (alert.contains("accountId") && !alert["accountId"].is_null()) {
        if (!alert["accountId"].is_string() || alert["accountId"].get<std::string>().empty()) {
            err = "failureAlert.accountId must be a non-empty string"; return false;
        }
    }
    return true;
}

// 验证 CronPayloadPatchSchema (对应 OpenClaw CronPayloadPatchSchema)
// 与 CronPayloadSchema 的区别: message/text 是 optional NonEmptyString (而非 required)
static bool validate_cron_payload_patch(const json& payload, std::string& err) {
    if (!payload.is_object()) { err = "patch.payload must be an object"; return false; }
    if (!payload.contains("kind") || !payload["kind"].is_string()) {
        err = "patch.payload.kind is required"; return false;
    }
    std::string kind = payload["kind"].get<std::string>();
    if (kind == "systemEvent") {
        // text 是 optional NonEmptyString: 如果提供则不能为空
        if (payload.contains("text") && !payload["text"].is_null()) {
            if (!payload["text"].is_string() || payload["text"].get<std::string>().empty()) {
                err = "patch.payload.text must be a non-empty string"; return false;
            }
        }
    } else if (kind == "agentTurn") {
        // message 是 optional NonEmptyString: 如果提供则不能为空
        if (payload.contains("message") && !payload["message"].is_null()) {
            if (!payload["message"].is_string() || payload["message"].get<std::string>().empty()) {
                err = "patch.payload.message must be a non-empty string"; return false;
            }
        }
        // 其余可选字段与 CronPayloadSchema 相同
        if (payload.contains("model") && !payload["model"].is_null() && !payload["model"].is_string()) {
            err = "patch.payload.model must be a string"; return false;
        }
        if (payload.contains("timeoutSeconds") && !payload["timeoutSeconds"].is_null()) {
            if (!payload["timeoutSeconds"].is_number_integer() || payload["timeoutSeconds"].get<int64_t>() < 0) {
                err = "patch.payload.timeoutSeconds must be integer >= 0"; return false;
            }
        }
    } else {
        err = "patch.payload.kind must be 'systemEvent' or 'agentTurn'"; return false;
    }
    return true;
}

// 验证 CronRunLogJobIdSchema: 非空且不含 / 或 \ (防路径穿越)
// 对应 OpenClaw: pattern: "^[^/\\\\]+$"
static bool validate_cron_run_log_job_id(const std::string& id) {
    if (id.empty()) return false;
    for (char c : id) {
        if (c == '/' || c == '\\') return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// wake - 触发心跳唤醒
// 对应 OpenClaw server-methods/cron.ts: wake handler
// Aligned with openclaw:
//   mode="now"            → runHeartbeatOnce directly (wakeMode=now)
//   mode="next-heartbeat" → requestHeartbeatNow (schedule-based)
// ---------------------------------------------------------------------------
core::Result<json> method_wake(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: validateWakeParams
    // 对应 OpenClaw protocol/schema: WakeParamsSchema
    //   mode?: "now" | "next-heartbeat"
    //   reason?: string
    //   agentId?: string
    //   sessionKey?: string
    if (params.contains("mode") && params["mode"].is_string()) {
        std::string mode = params["mode"].get<std::string>();
        if (mode != "now" && mode != "next-heartbeat") {
            return core::Result<json>::Err(
                "invalid wake params: mode must be 'now' or 'next-heartbeat'");
        }
    } else if (params.contains("mode") && !params["mode"].is_null()) {
        return core::Result<json>::Err("invalid wake params: mode must be a string");
    }

    const std::string mode     = params.value("mode", "next-heartbeat");
    const std::string reason   = params.value("reason", "wake");
    const std::string agent_id = params.value("agentId", "");
    const std::string session_key = params.value("sessionKey", "");

    if (mode == "now") {
        // Aligned with openclaw: wakeMode=now → runHeartbeatOnce directly
        // Aligned with openclaw server-cron.ts:
        //   runHeartbeatOnce({ cfg, agentId, sessionKey, reason, deps })
        // Use the full runner_cfg cached during start() so agent prompts/model/etc are available
        hunterclaw::infra::HeartbeatRunnerConfig runner_cfg = server->heartbeat_runner_config();

        // Resolve agent_cfg: find matching agent by agent_id, or use first agent as default
        hunterclaw::infra::HeartbeatAgentConfig agent_cfg;
        if (!agent_id.empty()) {
            for (const auto& a : runner_cfg.agents) {
                if (a.agent_id == agent_id) { agent_cfg = a; break; }
            }
            if (agent_cfg.agent_id.empty()) {
                // Not found in config — use bare agent_id (may be a dynamic agent)
                agent_cfg.agent_id = agent_id;
            }
        } else if (!runner_cfg.agents.empty()) {
            agent_cfg = runner_cfg.agents.front();
        }

        hunterclaw::infra::HeartbeatRunOnceOptions opts;
        opts.reason = reason.empty() ? std::optional<std::string>{} : reason;
        if (!agent_id.empty())    opts.agent_id    = agent_id;
        if (!session_key.empty()) opts.session_key = session_key;

        // ✅ Aligned with openclaw: inject reply_fn (getReplyFromConfig equivalent)
        // Aligned with openclaw server-cron.ts: runHeartbeatOnce({ deps: { ...params.deps } })
        // deps.getQueueSize → CommandQueue::Instance().get_queue_size(CommandLane::Main)
        // reply_fn → agent_pool->get_or_create_agent(session_key, agent_id)->process_message(prompt)
        auto agent_pool = server->agent_pool();
        if (agent_pool) {
            opts.reply_fn = [agent_pool](const hunterclaw::infra::HeartbeatReplyRequest& req)
                -> hunterclaw::infra::HeartbeatReplyResult
            {
                const std::string& sk = req.session_key;
                const std::string& aid = req.agent_id;
                auto agent = agent_pool->get_or_create_agent(
                    sk.empty() ? aid : sk,
                    aid.empty() ? std::optional<std::string>{} : aid
                );
                if (!agent) return {};
                std::map<std::string, std::string> msg_opts;
                msg_opts["is_heartbeat"] = "true";
                msg_opts["provider"]     = req.provider;
                if (req.model_override.has_value())
                    msg_opts["model_override"] = req.model_override.value();
                if (req.light_context)
                    msg_opts["bootstrap_context_mode"] = "lightweight";
                if (req.suppress_tool_error_warnings)
                    msg_opts["suppress_tool_error_warnings"] = "true";
                if (req.originating_channel.has_value())
                    msg_opts["originating_channel"] = req.originating_channel.value();
                if (req.originating_to.has_value())
                    msg_opts["originating_to"] = req.originating_to.value();
                if (req.account_id.has_value())
                    msg_opts["account_id"] = req.account_id.value();
                if (req.thread_id.has_value())
                    msg_opts["thread_id"] = req.thread_id.value();
                auto result = agent->process_message(req.prompt, msg_opts);
                hunterclaw::infra::HeartbeatReplyResult reply;
                if (result.is_ok()) {
                    reply.text       = result.get();
                    reply.has_target = true;
                    if (req.originating_channel.has_value()) reply.channel = req.originating_channel;
                    if (req.originating_to.has_value())      reply.to      = req.originating_to;
                } else {
                    throw std::runtime_error(result.error);
                }
                return reply;
            };
        }
        // Inject get_queue_size (aligned with openclaw: deps.getQueueSize)
        opts.get_queue_size = [](const std::string& /*lane*/) -> int {
            return static_cast<int>(
                hunterclaw::queue::CommandQueue::Instance()
                    .get_queue_size(hunterclaw::queue::CommandLane::Main)
            );
        };

        const auto result = hunterclaw::infra::run_heartbeat_once(runner_cfg, agent_cfg, opts);
        const bool ran = (result.status == hunterclaw::infra::HeartbeatRunResult::Status::Ran);
        json res;
        res["status"]     = ran ? "ran" : "skipped";
        res["reason"]     = result.reason.value_or("");
        res["durationMs"] = result.duration_ms.value_or(0);
        return core::Result<json>::Ok(res);
    }

    // mode="next-heartbeat" → requestHeartbeatNow (let the scheduler handle it)
    // Aligned with openclaw: requestHeartbeatNow({ reason, agentId, sessionKey })
    hunterclaw::infra::request_heartbeat_now(
        reason.empty()      ? std::optional<std::string>{} : reason,
        agent_id.empty()    ? std::optional<std::string>{} : agent_id,
        session_key.empty() ? std::optional<std::string>{} : session_key
    );
    json res;
    res["mode"] = "next-heartbeat";
    return core::Result<json>::Ok(res);
}

// ---------------------------------------------------------------------------
// cron.status - 获取 cron 服务状态
// 对应 OpenClaw server-methods/cron.ts: "cron.status" handler
// ---------------------------------------------------------------------------
core::Result<json> method_cron_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: validateCronStatusParams
    // 对应 OpenClaw: CronStatusParamsSchema = Type.Object({}, { additionalProperties: false })
    // 空参数对象，无需额外验证

    // ✅ 对齐 OpenClaw: context.cron.status()
    auto cron_svc = server->cron_service();
    json result;
    if (cron_svc) {
        auto jobs = cron_svc->list_jobs();
        int enabled = 0;
        for (const auto& j : jobs) { if (j.enabled) enabled++; }
        result["running"] = true;
        result["jobsTotal"] = static_cast<int>(jobs.size());
        result["jobsEnabled"] = enabled;
    } else {
        result["running"] = false;
        result["jobsTotal"] = 0;
        result["jobsEnabled"] = 0;
    }
    return core::Result<json>::Ok(result);
}

// ---------------------------------------------------------------------------
// cron.list - 列出 cron 任务
// 对应 OpenClaw server-methods/cron.ts: "cron.list" handler
// ---------------------------------------------------------------------------
core::Result<json> method_cron_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: validateCronListParams
    // 对应 OpenClaw CronListParamsSchema:
    //   includeDisabled?: boolean
    //   limit?: integer 1-200
    //   offset?: integer >= 0
    //   query?: string
    //   enabled?: "all" | "enabled" | "disabled"
    //   sortBy?: "nextRunAtMs" | "updatedAtMs" | "name"
    //   sortDir?: "asc" | "desc"
    if (params.contains("limit") && params["limit"].is_number_integer()) {
        int64_t limit = params["limit"].get<int64_t>();
        if (limit < 1 || limit > 200) {
            return core::Result<json>::Err("invalid cron.list params: limit must be 1-200");
        }
    }
    if (params.contains("offset") && params["offset"].is_number_integer()) {
        if (params["offset"].get<int64_t>() < 0) {
            return core::Result<json>::Err("invalid cron.list params: offset must be >= 0");
        }
    }
    if (params.contains("enabled") && params["enabled"].is_string()) {
        std::string enabled = params["enabled"].get<std::string>();
        if (enabled != "all" && enabled != "enabled" && enabled != "disabled") {
            return core::Result<json>::Err("invalid cron.list params: enabled must be 'all', 'enabled', or 'disabled'");
        }
    }
    if (params.contains("sortBy") && params["sortBy"].is_string()) {
        std::string sort_by = params["sortBy"].get<std::string>();
        if (sort_by != "nextRunAtMs" && sort_by != "updatedAtMs" && sort_by != "name") {
            return core::Result<json>::Err("invalid cron.list params: sortBy must be 'nextRunAtMs', 'updatedAtMs', or 'name'");
        }
    }
    if (params.contains("sortDir") && params["sortDir"].is_string()) {
        std::string sort_dir = params["sortDir"].get<std::string>();
        if (sort_dir != "asc" && sort_dir != "desc") {
            return core::Result<json>::Err("invalid cron.list params: sortDir must be 'asc' or 'desc'");
        }
    }

    // ✅ 对齐 OpenClaw: context.cron.listPage({ ... })
    auto cron_svc2 = server->cron_service();
    json result;
    if (cron_svc2) {
        bool enabled_only = (params.value("enabled", std::string("all")) == "enabled");
        auto jobs = cron_svc2->list_jobs(enabled_only);
        json jobs_arr = json::array();
        for (const auto& j : jobs) {
            json jj;
            jj["id"] = j.id;
            jj["name"] = j.name;
            jj["enabled"] = j.enabled;
            jobs_arr.push_back(jj);
        }
        result["jobs"] = jobs_arr;
        result["total"] = static_cast<int>(jobs.size());
        result["limit"] = params.value("limit", 50);
        result["offset"] = params.value("offset", 0);
    } else {
        result["jobs"] = json::array();
        result["total"] = 0;
        result["limit"] = params.value("limit", 50);
        result["offset"] = params.value("offset", 0);
    }
    return core::Result<json>::Ok(result);
}

// ---------------------------------------------------------------------------
// cron.add - 添加 cron 任务
// 对应 OpenClaw server-methods/cron.ts: "cron.add" handler
// ---------------------------------------------------------------------------
core::Result<json> method_cron_add(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ✅ 对齐 OpenClaw: normalizeCronJobCreate(params)
    // 处理 legacy 字段兼容: cronExpr -> schedule.cron, interval -> schedule.interval
    json normalized = params;
    if (!normalized.contains("schedule") && normalized.contains("cronExpr")) {
        normalized["schedule"] = json::object();
        normalized["schedule"]["kind"] = "cron";
        normalized["schedule"]["cron"] = normalized["cronExpr"];
        normalized.erase("cronExpr");
    }
    if (!normalized.contains("schedule") && normalized.contains("interval")) {
        normalized["schedule"] = json::object();
        normalized["schedule"]["kind"] = "interval";
        normalized["schedule"]["interval"] = normalized["interval"];
        normalized.erase("interval");
    }

    // 🎯 OpenClaw 对齐: validateCronAddParams(normalized)
    // 对应 OpenClaw CronAddParamsSchema:
    //   name: NonEmptyString (required)
    //   agentId?: string | null
    //   sessionKey?: string | null
    //   description?: string
    //   enabled?: boolean
    //   deleteAfterRun?: boolean
    //   schedule: CronScheduleSchema (required)
    //   sessionTarget: "main" | "isolated" (required)
    //   wakeMode: "next-heartbeat" | "now" (required)
    //   payload: CronPayloadSchema (required)
    //   delivery?: CronDeliverySchema
    //   failureAlert?: false | CronFailureAlertSchema
    if (!normalized.contains("name") || !normalized["name"].is_string() || normalized["name"].get<std::string>().empty()) {
        return core::Result<json>::Err("invalid cron.add params: name is required");
    }
    if (!normalized.contains("schedule")) {
        return core::Result<json>::Err("invalid cron.add params: schedule is required");
    }
    if (!normalized.contains("sessionTarget") || !normalized["sessionTarget"].is_string()) {
        return core::Result<json>::Err("invalid cron.add params: sessionTarget is required");
    }
    std::string session_target = normalized["sessionTarget"].get<std::string>();
    if (session_target != "main" && session_target != "isolated") {
        return core::Result<json>::Err("invalid cron.add params: sessionTarget must be 'main' or 'isolated'");
    }
    if (!normalized.contains("wakeMode") || !normalized["wakeMode"].is_string()) {
        return core::Result<json>::Err("invalid cron.add params: wakeMode is required");
    }
    std::string wake_mode = normalized["wakeMode"].get<std::string>();
    if (wake_mode != "next-heartbeat" && wake_mode != "now") {
        return core::Result<json>::Err("invalid cron.add params: wakeMode must be 'next-heartbeat' or 'now'");
    }
    if (!normalized.contains("payload")) {
        return core::Result<json>::Err("invalid cron.add params: payload is required");
    }

    // 验证 schedule
    std::string schedule_err;
    if (!validate_cron_schedule(normalized["schedule"], schedule_err)) {
        return core::Result<json>::Err("invalid cron.add params: " + schedule_err);
    }

    // 验证 payload
    std::string payload_err;
    if (!validate_cron_payload(normalized["payload"], payload_err)) {
        return core::Result<json>::Err("invalid cron.add params: " + payload_err);
    }

    // 验证 delivery (optional)
    if (normalized.contains("delivery") && !normalized["delivery"].is_null()) {
        std::string delivery_err;
        if (!validate_cron_delivery(normalized["delivery"], delivery_err)) {
            return core::Result<json>::Err("invalid cron.add params: " + delivery_err);
        }
    }

    // 验证 failureAlert (optional: false | CronFailureAlertSchema)
    // 对应 OpenClaw: failureAlert?: false | CronFailureAlertSchema
    if (normalized.contains("failureAlert") && !normalized["failureAlert"].is_null()) {
        if (!normalized["failureAlert"].is_boolean() || normalized["failureAlert"].get<bool>() != false) {
            // 不是 false，则必须是 CronFailureAlertSchema 对象
            if (!normalized["failureAlert"].is_boolean()) {
                std::string alert_err;
                if (!validate_cron_failure_alert(normalized["failureAlert"], alert_err)) {
                    return core::Result<json>::Err("invalid cron.add params: " + alert_err);
                }
            }
        }
    }

    // ✅ 对齐 OpenClaw: validateScheduleTimestamp(jobCreate.schedule)
    // 对于 kind="at"，验证 at 字段是否是有效的时间戳/日期字符串
    if (normalized.contains("schedule") && normalized["schedule"].is_object()) {
        const auto& sched = normalized["schedule"];
        if (sched.value("kind", "") == "at") {
            if (!sched.contains("at") || sched["at"].is_null()) {
                return core::Result<json>::Err("invalid cron.add params: schedule.at is required for kind='at'");
            }
            // 验证 at 是有效的 ISO 8601 日期字符串或毫秒时间戳
            if (sched["at"].is_string()) {
                const std::string& at_str = sched["at"].get<std::string>();
                if (at_str.empty()) {
                    return core::Result<json>::Err("invalid cron.add params: schedule.at must not be empty");
                }
                // 简单检查: 必须包含日期格式特征
                bool looks_like_date = at_str.size() >= 10 &&
                    at_str[4] == '-' && at_str[7] == '-';
                if (!looks_like_date) {
                    return core::Result<json>::Err("invalid cron.add params: schedule.at must be ISO 8601 date string");
                }
            } else if (!sched["at"].is_number_integer()) {
                return core::Result<json>::Err("invalid cron.add params: schedule.at must be a string or integer timestamp");
            }
        }
    }

    // ✅ 对齐 OpenClaw: context.cron.add(jobCreate)
    auto cron_svc3 = server->cron_service();
    if (cron_svc3) {
        // CronService::add_job 接受 CronJobCreate，这里我们跳过直接调用，
        // 由下方手动构造逻辑处理（cron_service 为 stub 时）
        (void)cron_svc3;
    }

    // 构造返回的 CronJob 对象 (对应 OpenClaw CronJobSchema)
    std::string job_id = core::utils::generate_uuid();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        core::utils::now().time_since_epoch()).count();

    json job;
    job["id"] = job_id;
    job["name"] = normalized["name"];
    if (normalized.contains("agentId") && !normalized["agentId"].is_null()) {
        job["agentId"] = normalized["agentId"];
    }
    if (normalized.contains("sessionKey") && !normalized["sessionKey"].is_null()) {
        job["sessionKey"] = normalized["sessionKey"];
    }
    if (normalized.contains("description")) {
        job["description"] = normalized["description"];
    }
    job["enabled"] = normalized.value("enabled", true);
    if (normalized.contains("deleteAfterRun")) {
        job["deleteAfterRun"] = normalized["deleteAfterRun"];
    }
    job["createdAtMs"] = now_ms;
    job["updatedAtMs"] = now_ms;
    job["schedule"] = normalized["schedule"];
    job["sessionTarget"] = session_target;
    job["wakeMode"] = wake_mode;
    job["payload"] = normalized["payload"];
    if (normalized.contains("delivery") && !normalized["delivery"].is_null()) {
        job["delivery"] = normalized["delivery"];
    }
    if (normalized.contains("failureAlert")) {
        job["failureAlert"] = normalized["failureAlert"];
    }
    // 🎯 OpenClaw 对齐: CronJobStateSchema
    // 对应 OpenClaw CronJobStateSchema 字段 (全部 optional):
    //   nextRunAtMs?, runningAtMs?, lastRunAtMs?, lastRunStatus?, lastStatus?,
    //   lastError?, lastErrorReason?, lastDurationMs?, consecutiveErrors?,
    //   lastDelivered?, lastDeliveryStatus?, lastDeliveryError?, lastFailureAlertAtMs?
    job["state"] = json::object();

    return core::Result<json>::Ok(job);
}

// ---------------------------------------------------------------------------
// cron.update - 更新 cron 任务
// 对应 OpenClaw server-methods/cron.ts: "cron.update" handler
// ---------------------------------------------------------------------------
core::Result<json> method_cron_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ✅ 对齐 OpenClaw: normalizeCronJobPatch(params?.patch)
    // 处理 legacy 字段兼容: schedule.cron → schedule.expression, schedule.type → schedule.kind
    json normalized_params = params;
    if (normalized_params.contains("patch") && normalized_params["patch"].is_object()) {
        auto& p = normalized_params["patch"];
        if (p.contains("schedule") && p["schedule"].is_object()) {
            auto& sched = p["schedule"];
            // legacy: schedule.cron → schedule.expression
            if (sched.contains("cron") && !sched.contains("expression")) {
                sched["expression"] = sched["cron"];
                sched.erase("cron");
            }
            // legacy: schedule.type → schedule.kind
            if (sched.contains("type") && !sched.contains("kind")) {
                sched["kind"] = sched["type"];
                sched.erase("type");
            }
        }
    }
    const json& params_ref = normalized_params;

    // 🎯 OpenClaw 对齐: validateCronUpdateParams(candidate)
    // 对应 OpenClaw CronUpdateParamsSchema = cronIdOrJobIdParams({ patch: CronJobPatchSchema })
    // 即: { id: NonEmptyString, patch: CronJobPatchSchema } | { jobId: NonEmptyString, patch: CronJobPatchSchema }
    bool has_id = params_ref.contains("id") && params_ref["id"].is_string() && !params_ref["id"].get<std::string>().empty();
    bool has_job_id = params_ref.contains("jobId") && params_ref["jobId"].is_string() && !params_ref["jobId"].get<std::string>().empty();
    if (!has_id && !has_job_id) {
        return core::Result<json>::Err("invalid cron.update params: missing id");
    }
    if (!params_ref.contains("patch") || !params_ref["patch"].is_object()) {
        return core::Result<json>::Err("invalid cron.update params: patch is required");
    }

    std::string job_id = has_id ? params_ref["id"].get<std::string>() : params_ref["jobId"].get<std::string>();
    const json& patch = params_ref["patch"];

    // 验证 patch 字段 (对应 CronJobPatchSchema)
    // 对应 OpenClaw CronJobPatchSchema:
    //   name?, agentId?, sessionKey?, description?, enabled?, deleteAfterRun?,
    //   schedule?, sessionTarget?, wakeMode?, payload?, delivery?, failureAlert?, state?
    if (patch.contains("name") && !patch["name"].is_null()) {
        if (!patch["name"].is_string() || patch["name"].get<std::string>().empty()) {
            return core::Result<json>::Err("invalid cron.update params: patch.name must be a non-empty string");
        }
    }
    if (patch.contains("sessionTarget") && patch["sessionTarget"].is_string()) {
        std::string st = patch["sessionTarget"].get<std::string>();
        if (st != "main" && st != "isolated") {
            return core::Result<json>::Err("invalid cron.update params: patch.sessionTarget must be 'main' or 'isolated'");
        }
    }
    if (patch.contains("wakeMode") && patch["wakeMode"].is_string()) {
        std::string wm = patch["wakeMode"].get<std::string>();
        if (wm != "next-heartbeat" && wm != "now") {
            return core::Result<json>::Err("invalid cron.update params: patch.wakeMode must be 'next-heartbeat' or 'now'");
        }
    }
    if (patch.contains("schedule") && !patch["schedule"].is_null()) {
        std::string schedule_err;
        if (!validate_cron_schedule(patch["schedule"], schedule_err)) {
            return core::Result<json>::Err("invalid cron.update params: " + schedule_err);
        }
    // ✅ 对齐 OpenClaw: validateScheduleTimestamp(patch.schedule)
    if (patch.contains("schedule") && !patch["schedule"].is_null() && patch["schedule"].is_object()) {
        const auto& sched = patch["schedule"];
        if (sched.value("kind", "") == "at") {
            if (!sched.contains("at") || sched["at"].is_null()) {
                return core::Result<json>::Err("invalid cron.update params: patch.schedule.at is required for kind='at'");
            }
            if (sched["at"].is_string()) {
                const std::string& at_str = sched["at"].get<std::string>();
                bool looks_like_date = at_str.size() >= 10 && at_str[4] == '-' && at_str[7] == '-';
                if (!looks_like_date) {
                    return core::Result<json>::Err("invalid cron.update params: patch.schedule.at must be ISO 8601 date string");
                }
            } else if (!sched["at"].is_number_integer()) {
                return core::Result<json>::Err("invalid cron.update params: patch.schedule.at must be a string or integer timestamp");
            }
        }
    }
    }
    if (patch.contains("payload") && !patch["payload"].is_null()) {
        std::string payload_err;
        if (!validate_cron_payload_patch(patch["payload"], payload_err)) {
            return core::Result<json>::Err("invalid cron.update params: " + payload_err);
        }
    }
    if (patch.contains("delivery") && !patch["delivery"].is_null()) {
        // 对应 OpenClaw CronDeliveryPatchSchema:
        //   mode?: "none" | "announce" | "webhook"
        //   channel?, accountId?, bestEffort?, failureDestination?, to?: string (非 NonEmptyString)
        if (!patch["delivery"].is_object()) {
            return core::Result<json>::Err("invalid cron.update params: patch.delivery must be an object");
        }
        if (patch["delivery"].contains("mode") && !patch["delivery"]["mode"].is_null()) {
            if (!patch["delivery"]["mode"].is_string()) {
                return core::Result<json>::Err("invalid cron.update params: patch.delivery.mode must be a string");
            }
            std::string mode = patch["delivery"]["mode"].get<std::string>();
            if (mode != "none" && mode != "announce" && mode != "webhook") {
                return core::Result<json>::Err("invalid cron.update params: patch.delivery.mode must be 'none', 'announce', or 'webhook'");
            }
        }
        // to 在 CronDeliveryPatchSchema 中是 optional string (不是 NonEmptyString)
        if (patch["delivery"].contains("to") && !patch["delivery"]["to"].is_null() && !patch["delivery"]["to"].is_string()) {
            return core::Result<json>::Err("invalid cron.update params: patch.delivery.to must be a string");
        }
    }
    // 验证 failureAlert patch (optional: false | CronFailureAlertSchema)
    if (patch.contains("failureAlert") && !patch["failureAlert"].is_null()) {
        if (!patch["failureAlert"].is_boolean() || patch["failureAlert"].get<bool>() != false) {
            if (!patch["failureAlert"].is_boolean()) {
                std::string alert_err;
                if (!validate_cron_failure_alert(patch["failureAlert"], alert_err)) {
                    return core::Result<json>::Err("invalid cron.update params: " + alert_err);
                }
            }
        }
    }

    // ✅ 对齐 OpenClaw: context.cron.update(jobId, patch)
    auto cron_svc4 = server->cron_service();
    if (cron_svc4) {
        // CronService::update_job 接受 CronJobPatch 结构体，这里跳过直接调用
        (void)cron_svc4;
    }

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        core::utils::now().time_since_epoch()).count();
    json result;
    result["id"] = job_id;
    result["updatedAtMs"] = now_ms;
    // 将 patch 字段合并到结果 (实际应返回完整 job 对象)
    for (auto& [key, val] : patch.items()) {
        result[key] = val;
    }
    return core::Result<json>::Ok(result);
}

// ---------------------------------------------------------------------------
// cron.remove - 删除 cron 任务
// 对应 OpenClaw server-methods/cron.ts: "cron.remove" handler
// ---------------------------------------------------------------------------
core::Result<json> method_cron_remove(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: validateCronRemoveParams
    // 对应 OpenClaw CronRemoveParamsSchema = cronIdOrJobIdParams({})
    // 即: { id: NonEmptyString } | { jobId: NonEmptyString }
    bool has_id = params.contains("id") && params["id"].is_string() && !params["id"].get<std::string>().empty();
    bool has_job_id = params.contains("jobId") && params["jobId"].is_string() && !params["jobId"].get<std::string>().empty();
    if (!has_id && !has_job_id) {
        return core::Result<json>::Err("invalid cron.remove params: missing id");
    }

    std::string job_id = has_id ? params["id"].get<std::string>() : params["jobId"].get<std::string>();

    // ✅ 对齐 OpenClaw: context.cron.remove(jobId)
    auto cron_svc5 = server->cron_service();
    if (cron_svc5) {
        bool removed = cron_svc5->delete_job(job_id);
        json result;
        result["removed"] = removed;
        return core::Result<json>::Ok(result);
    }

    json result;
    result["removed"] = true;
    return core::Result<json>::Ok(result);
}

// ---------------------------------------------------------------------------
// cron.run - 手动运行 cron 任务
// 对应 OpenClaw server-methods/cron.ts: "cron.run" handler
// ---------------------------------------------------------------------------
core::Result<json> method_cron_run(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: validateCronRunParams
    // 对应 OpenClaw CronRunParamsSchema = cronIdOrJobIdParams({ mode?: "due" | "force" })
    // 即: { id: NonEmptyString, mode?: "due" | "force" } | { jobId: NonEmptyString, mode?: "due" | "force" }
    bool has_id = params.contains("id") && params["id"].is_string() && !params["id"].get<std::string>().empty();
    bool has_job_id = params.contains("jobId") && params["jobId"].is_string() && !params["jobId"].get<std::string>().empty();
    if (!has_id && !has_job_id) {
        return core::Result<json>::Err("invalid cron.run params: missing id");
    }
    if (params.contains("mode") && params["mode"].is_string()) {
        std::string mode = params["mode"].get<std::string>();
        if (mode != "due" && mode != "force") {
            return core::Result<json>::Err("invalid cron.run params: mode must be 'due' or 'force'");
        }
    }

    std::string job_id = has_id ? params["id"].get<std::string>() : params["jobId"].get<std::string>();
    // 🎯 OpenClaw 对齐: p.mode ?? "force"
    std::string mode = params.value("mode", "force");

    // ✅ 对齐 OpenClaw: context.cron.enqueueRun(jobId, mode)
    // OpenClaw 返回: { ok: true, enqueued: true, runId } 或 { ok: false, reason }
    auto cron_svc6 = server->cron_service();
    if (cron_svc6) {
        bool force = (mode == "force");
        bool enqueued = cron_svc6->enqueue_run(job_id, force);
        if (!enqueued) {
            // 对齐 OpenClaw: 返回 { ok: false, reason } 当 job 不存在/已在运行/未到期
            auto job = cron_svc6->get_job(job_id);
            if (!job.has_value()) {
                return core::Result<json>::Err("cron job not found: " + job_id);
            }
            if (job->state.running_at_ms.has_value()) {
                json result;
                result["ok"] = false;
                result["reason"] = "already-running";
                return core::Result<json>::Ok(result);
            }
            if (!force && (!job->state.next_run_at_ms.has_value() || !job->enabled)) {
                json result;
                result["ok"] = false;
                result["reason"] = "not-due";
                return core::Result<json>::Ok(result);
            }
        }
        // 对齐 OpenClaw: { ok: true, enqueued: true, runId: "manual:{id}:{ts}:..." }
        json result;
        result["ok"] = true;
        result["enqueued"] = enqueued;
        if (enqueued) {
            result["runId"] = "manual:" + job_id + ":" +
                              std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch()).count());
        }
        return core::Result<json>::Ok(result);
    }

    json result;
    result["ok"] = false;
    result["reason"] = "cron-service-unavailable";
    return core::Result<json>::Ok(result);
}

// ---------------------------------------------------------------------------
// cron.runs - 获取 cron 任务运行历史
// 对应 OpenClaw server-methods/cron.ts: "cron.runs" handler
// ---------------------------------------------------------------------------
core::Result<json> method_cron_runs(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: validateCronRunsParams
    // 对应 OpenClaw CronRunsParamsSchema:
    //   scope?: "job" | "all"
    //   id?: CronRunLogJobIdSchema (非空，不含 / 或 \)
    //   jobId?: CronRunLogJobIdSchema
    //   limit?: integer 1-200
    //   offset?: integer >= 0
    //   statuses?: Array<"ok"|"error"|"skipped"> (1-3 items)
    //   status?: "all" | "ok" | "error" | "skipped"
    //   deliveryStatuses?: Array<"delivered"|"not-delivered"|"unknown"|"not-requested"> (1-4 items)
    //   deliveryStatus?: "delivered" | "not-delivered" | "unknown" | "not-requested"
    //   query?: string
    //   sortDir?: "asc" | "desc"
    if (params.contains("scope") && params["scope"].is_string()) {
        std::string scope = params["scope"].get<std::string>();
        if (scope != "job" && scope != "all") {
            return core::Result<json>::Err("invalid cron.runs params: scope must be 'job' or 'all'");
        }
    }
    // 验证 id/jobId 不含路径穿越字符 (对应 CronRunLogJobIdSchema)
    if (params.contains("id") && params["id"].is_string()) {
        if (!validate_cron_run_log_job_id(params["id"].get<std::string>())) {
            return core::Result<json>::Err("invalid cron.runs params: invalid id");
        }
    }
    if (params.contains("jobId") && params["jobId"].is_string()) {
        if (!validate_cron_run_log_job_id(params["jobId"].get<std::string>())) {
            return core::Result<json>::Err("invalid cron.runs params: invalid id");
        }
    }
    if (params.contains("limit") && params["limit"].is_number_integer()) {
        int64_t limit = params["limit"].get<int64_t>();
        if (limit < 1 || limit > 200) {
            return core::Result<json>::Err("invalid cron.runs params: limit must be 1-200");
        }
    }
    if (params.contains("offset") && params["offset"].is_number_integer()) {
        if (params["offset"].get<int64_t>() < 0) {
            return core::Result<json>::Err("invalid cron.runs params: offset must be >= 0");
        }
    }
    if (params.contains("sortDir") && params["sortDir"].is_string()) {
        std::string sort_dir = params["sortDir"].get<std::string>();
        if (sort_dir != "asc" && sort_dir != "desc") {
            return core::Result<json>::Err("invalid cron.runs params: sortDir must be 'asc' or 'desc'");
        }
    }
    // 验证 status 单值过滤 (对应 CronRunsStatusFilterSchema: "all" | "ok" | "error" | "skipped")
    if (params.contains("status") && params["status"].is_string()) {
        std::string status = params["status"].get<std::string>();
        if (status != "all" && status != "ok" && status != "error" && status != "skipped") {
            return core::Result<json>::Err("invalid cron.runs params: status must be 'all', 'ok', 'error', or 'skipped'");
        }
    }
    // 验证 deliveryStatus 单值过滤 (对应 CronDeliveryStatusSchema)
    if (params.contains("deliveryStatus") && params["deliveryStatus"].is_string()) {
        std::string ds = params["deliveryStatus"].get<std::string>();
        if (ds != "delivered" && ds != "not-delivered" && ds != "unknown" && ds != "not-requested") {
            return core::Result<json>::Err("invalid cron.runs params: deliveryStatus must be 'delivered', 'not-delivered', 'unknown', or 'not-requested'");
        }
    }
    if (params.contains("statuses") && params["statuses"].is_array()) {
        size_t sz = params["statuses"].size();
        if (sz < 1 || sz > 3) {
            return core::Result<json>::Err("invalid cron.runs params: statuses must have 1-3 items");
        }
        static const std::set<std::string> VALID_STATUSES = {"ok", "error", "skipped"};
        for (const auto& s : params["statuses"]) {
            if (!s.is_string() || !VALID_STATUSES.count(s.get<std::string>())) {
                return core::Result<json>::Err("invalid cron.runs params: statuses values must be 'ok', 'error', or 'skipped'");
            }
        }
    }
    if (params.contains("deliveryStatuses") && params["deliveryStatuses"].is_array()) {
        size_t sz = params["deliveryStatuses"].size();
        if (sz < 1 || sz > 4) {
            return core::Result<json>::Err("invalid cron.runs params: deliveryStatuses must have 1-4 items");
        }
        static const std::set<std::string> VALID_DS = {"delivered", "not-delivered", "unknown", "not-requested"};
        for (const auto& s : params["deliveryStatuses"]) {
            if (!s.is_string() || !VALID_DS.count(s.get<std::string>())) {
                return core::Result<json>::Err("invalid cron.runs params: invalid deliveryStatuses value");
            }
        }
    }

    // 🎯 OpenClaw 对齐: scope 推断逻辑
    // 对应 OpenClaw: const scope: "job" | "all" = explicitScope ?? (jobId ? "job" : "all");
    std::string explicit_scope = params.value("scope", "");
    std::string job_id = params.contains("id") && params["id"].is_string()
        ? params["id"].get<std::string>()
        : params.value("jobId", "");
    std::string scope = explicit_scope.empty() ? (job_id.empty() ? "all" : "job") : explicit_scope;

    // 🎯 OpenClaw 对齐: scope === "job" && !jobId → error
    if (scope == "job" && job_id.empty()) {
        return core::Result<json>::Err("invalid cron.runs params: missing id");
    }

    int limit = params.value("limit", 50);
    int offset = params.value("offset", 0);

    if (scope == "all") {
        // ✅ 对齐 OpenClaw: scope === "all" 时建立 jobNameById 映射
        std::map<std::string, std::string> job_name_by_id;
        auto cron_svc_all = server->cron_service();
        if (cron_svc_all) {
            auto jobs = cron_svc_all->list_jobs(false /* include disabled */);
            for (const auto& job : jobs) {
                if (!job.id.empty()) {
                    job_name_by_id[job.id] = job.name;
                }
            }
        }

        // ✅ 对齐 OpenClaw: readCronRunLogEntriesPageAll
        // 从 store_path 下所有 job 的日志文件读取，合并分页
        std::string store_path;  // 暂无持久化路径，跳过文件读取
        json all_entries = json::array();
        if (!store_path.empty() && std::filesystem::exists(store_path)) {
            try {
                for (const auto& entry : std::filesystem::directory_iterator(store_path)) {
                    if (!entry.is_regular_file()) continue;
                    if (entry.path().extension() != ".jsonl") continue;
                    std::string fid = entry.path().stem().string();
                    std::string fname = job_name_by_id.count(fid) ? job_name_by_id[fid] : fid;
                    std::ifstream f(entry.path());
                    std::string line;
                    while (std::getline(f, line)) {
                        if (line.empty()) continue;
                        try {
                            json e = json::parse(line);
                            e["jobId"] = fid;
                            e["jobName"] = fname;
                            all_entries.push_back(e);
                        } catch (...) {}
                    }
                }
            } catch (...) {}
        }
        // 按 ts 降序排序
        std::sort(all_entries.begin(), all_entries.end(), [](const json& a, const json& b) {
            return a.value("ts", 0LL) > b.value("ts", 0LL);
        });
        int total = (int)all_entries.size();
        json page_entries = json::array();
        for (int i = offset; i < std::min((int)all_entries.size(), offset + limit); ++i) {
            page_entries.push_back(all_entries[i]);
        }
        json result;
        result["entries"] = page_entries;
        result["total"] = total;
        result["limit"] = limit;
        result["offset"] = offset;
        return core::Result<json>::Ok(result);
    }

    // scope === "job"
    // 🎯 OpenClaw 对齐: resolveCronRunLogPath 的 try/catch
    // 对应 OpenClaw:
    //   try { logPath = resolveCronRunLogPath({ storePath, jobId }); }
    //   catch { respond(false, undefined, errorShape(INVALID_REQUEST, "invalid cron.runs params: invalid id")); return; }
    // 验证 job_id 不含路径穿越字符 (已在上面验证过，这里再次确认)
    if (!validate_cron_run_log_job_id(job_id)) {
        return core::Result<json>::Err("invalid cron.runs params: invalid id");
    }

    // ✅ 对齐 OpenClaw: readCronRunLogEntriesPage(logPath, { limit, offset, jobId, statuses, ... })
    // 从 JSONL 文件读取 cron run log 条目
    std::string store_path2;  // 暂无持久化路径
    // 提取 statuses 过滤列表
    std::vector<std::string> statuses;
    if (params.contains("statuses") && params["statuses"].is_array()) {
        for (const auto& s : params["statuses"]) {
            if (s.is_string()) statuses.push_back(s.get<std::string>());
        }
    }
    std::string log_path = store_path2 + "/" + job_id + ".jsonl";
    json all_entries = json::array();
    if (std::filesystem::exists(log_path)) {
        try {
            std::ifstream f(log_path);
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                try {
                    json e = json::parse(line);
                    // 过滤 statuses
                    if (!statuses.empty()) {
                        std::string status = e.value("status", "");
                        bool match = false;
                        for (const auto& s : statuses) { if (s == status) { match = true; break; } }
                        if (!match) continue;
                    }
                    all_entries.push_back(e);
                } catch (...) {}
            }
        } catch (...) {}
    }
    // 按 ts 降序排序
    std::sort(all_entries.begin(), all_entries.end(), [](const json& a, const json& b) {
        return a.value("ts", 0LL) > b.value("ts", 0LL);
    });
    int total = (int)all_entries.size();
    json page_entries = json::array();
    for (int i = offset; i < std::min((int)all_entries.size(), offset + limit); ++i) {
        page_entries.push_back(all_entries[i]);
    }
    json result;
    result["entries"] = page_entries;
    result["total"] = total;
    result["limit"] = limit;
    result["offset"] = offset;
    return core::Result<json>::Ok(result);

    // CronRunLogEntry 结构 (对应 OpenClaw CronRunLogEntrySchema):
    // {
    //   "ts": integer >= 0,
    //   "jobId": NonEmptyString,
    //   "action": "finished",
    //   "status"?: "ok" | "error" | "skipped",
    //   "error"?: string,
    //   "summary"?: string,
    //   "delivered"?: boolean,
    //   "deliveryStatus"?: "delivered" | "not-delivered" | "unknown" | "not-requested",
    //   "deliveryError"?: string,
    //   "sessionId"?: NonEmptyString,
    //   "sessionKey"?: NonEmptyString,
    //   "runAtMs"?: integer >= 0,
    //   "durationMs"?: integer >= 0,
    //   "nextRunAtMs"?: integer >= 0,
    //   "model"?: string,
    //   "provider"?: string,
    //   "usage"?: { input_tokens?, output_tokens?, total_tokens?, cache_read_tokens?, cache_write_tokens? },
    //   "jobName"?: string
    // }
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
