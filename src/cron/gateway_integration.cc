#include "hunterclaw/cron/gateway_integration.h"
#include "hunterclaw/cron/schedule.h"
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace cron {

namespace {
    auto log() {
        static auto logger = spdlog::get("cron_gateway");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("cron_gateway");
        }
        return logger;
    }
}

// ============================================================================
// Constructor
// ============================================================================

GatewayCronIntegration::GatewayCronIntegration(std::shared_ptr<CronService> service)
    : service_(std::move(service)) {
    if (!service_) {
        throw std::invalid_argument("CronService cannot be null");
    }
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

nlohmann::json GatewayCronIntegration::handle_wake(const nlohmann::json& params) {
    log()->debug("handle_wake: {}", params.dump());
    
    // TODO: Implement wake functionality when needed
    log()->warn("Wake functionality not yet implemented in CronService");
    
    return {
        {"success", true},
        {"message", "Wake not implemented"}
    };
}

nlohmann::json GatewayCronIntegration::handle_status(const nlohmann::json& params) {
    log()->debug("handle_status: {}", params.dump());
    
    // Get job statistics
    auto jobs = service_->list_jobs(false);  // Get all jobs
    int total = jobs.size();
    int enabled = 0;
    for (const auto& job : jobs) {
        if (job.enabled) enabled++;
    }
    
    nlohmann::json result = {
        {"running", true},  // Assume running if service exists
        {"totalJobs", total},
        {"enabledJobs", enabled},
        {"disabledJobs", total - enabled}
    };
    
    return result;
}

nlohmann::json GatewayCronIntegration::handle_list(const nlohmann::json& params) {
    log()->debug("handle_list: {}", params.dump());
    
    bool include_disabled = false;
    if (params.contains("includeDisabled")) {
        include_disabled = params["includeDisabled"].get<bool>();
    }
    
    auto jobs = service_->list_jobs(!include_disabled);  // enabled_only = !include_disabled
    
    nlohmann::json jobs_json = nlohmann::json::array();
    for (const auto& job : jobs) {
        jobs_json.push_back(job_to_json(job));
    }
    
    return {
        {"jobs", jobs_json}
    };
}

nlohmann::json GatewayCronIntegration::handle_add(const nlohmann::json& params) {
    log()->debug("handle_add: {}", params.dump());
    
    if (!params.contains("job")) {
        return {
            {"error", "Missing 'job' parameter"}
        };
    }
    
    try {
        auto job_create = json_to_job_create(params["job"]);
        auto job = service_->add_job(job_create);
        
        return {
            {"job", job_to_json(job)}
        };
    } catch (const std::exception& e) {
        log()->error("Failed to parse job: {}", e.what());
        return {
            {"error", std::string("Invalid job format: ") + e.what()}
        };
    }
}

nlohmann::json GatewayCronIntegration::handle_update(const nlohmann::json& params) {
    log()->debug("handle_update: {}", params.dump());
    
    if (!params.contains("id")) {
        return {{"error", "Missing 'id' parameter"}};
    }
    
    if (!params.contains("patch")) {
        return {{"error", "Missing 'patch' parameter"}};
    }
    
    try {
        std::string id = params["id"].get<std::string>();
        auto patch = json_to_job_patch(params["patch"]);
        auto result = service_->update_job(id, patch);
        
        if (!result.has_value()) {
            return {
                {"error", "Job not found or update failed"}
            };
        }
        
        return {
            {"job", job_to_json(*result)}
        };
    } catch (const std::exception& e) {
        log()->error("Failed to parse patch: {}", e.what());
        return {
            {"error", std::string("Invalid patch format: ") + e.what()}
        };
    }
}

nlohmann::json GatewayCronIntegration::handle_remove(const nlohmann::json& params) {
    log()->debug("handle_remove: {}", params.dump());
    
    if (!params.contains("id")) {
        return {{"error", "Missing 'id' parameter"}};
    }
    
    std::string id = params["id"].get<std::string>();
    bool success = service_->delete_job(id);
    
    if (!success) {
        return {
            {"error", "Job not found or deletion failed"}
        };
    }
    
    return {
        {"success", true}
    };
}

nlohmann::json GatewayCronIntegration::handle_run(const nlohmann::json& params) {
    log()->debug("handle_run: {}", params.dump());
    
    if (!params.contains("id")) {
        return {{"error", "Missing 'id' parameter"}};
    }
    
    std::string id = params["id"].get<std::string>();
    // Note: force parameter is ignored for now
    
    try {
        service_->force_run(id);
        return {
            {"success", true}
        };
    } catch (const std::exception& e) {
        return {
            {"error", std::string("Failed to run job: ") + e.what()}
        };
    }
}

nlohmann::json GatewayCronIntegration::handle_runs(const nlohmann::json& params) {
    log()->debug("handle_runs: {}", params.dump());
    
    if (!params.contains("id")) {
        return {{"error", "Missing 'id' parameter"}};
    }
    
    std::string id = params["id"].get<std::string>();
    // Note: Run logs not implemented yet in CronService
    
    log()->warn("Run logs not yet implemented in CronService");
    
    nlohmann::json logs_json = nlohmann::json::array();
    
    return {
        {"runs", logs_json}
    };
}

// ============================================================================
// Agent Tool Interface
// ============================================================================

nlohmann::json GatewayCronIntegration::execute_agent_tool(const nlohmann::json& args) {
    log()->debug("execute_agent_tool: {}", args.dump());
    
    if (!args.contains("action")) {
        return {
            {"error", "Missing 'action' parameter"}
        };
    }
    
    std::string action = args["action"].get<std::string>();
    
    if (action == "status") {
        return handle_status(args);
    } else if (action == "list") {
        return handle_list(args);
    } else if (action == "add") {
        if (!args.contains("job")) {
            return {{"error", "Missing 'job' parameter for 'add' action"}};
        }
        return handle_add(args);
    } else if (action == "update") {
        if (!args.contains("id") || !args.contains("patch")) {
            return {{"error", "Missing 'id' or 'patch' parameter for 'update' action"}};
        }
        return handle_update(args);
    } else if (action == "remove") {
        if (!args.contains("id")) {
            return {{"error", "Missing 'id' parameter for 'remove' action"}};
        }
        return handle_remove(args);
    } else if (action == "run") {
        if (!args.contains("id")) {
            return {{"error", "Missing 'id' parameter for 'run' action"}};
        }
        return handle_run(args);
    } else {
        return {
            {"error", "Unknown action: " + action}
        };
    }
}

// ============================================================================
// Multi-tenant Context (🏢 对齐 OpenClaw)
// ============================================================================

void GatewayCronIntegration::set_context(
    const std::string& agent_id,
    const std::optional<std::string>& session_key
) {
    // TODO: ExecutionContext not yet implemented in CronService
    // CronService::ExecutionContext ctx;
    // ctx.agent_id = agent_id;
    // ctx.session_key = session_key;
    // service_->set_context(ctx);
    
    log()->debug("[Cron] Context set to agent_id={} (not yet implemented)", agent_id);
}

// ============================================================================
// JSON Conversion Helpers
// ============================================================================

nlohmann::json GatewayCronIntegration::job_to_json(const CronJob& job) {
    nlohmann::json j;
    j["id"] = job.id;
    j["name"] = job.name;
    if (job.description.has_value()) j["description"] = *job.description;
    j["enabled"] = job.enabled;
    j["sessionTarget"] = job.session_target;
    j["wakeMode"] = job.wake_mode;
    j["createdAtMs"] = job.created_at_ms;
    j["updatedAtMs"] = job.updated_at_ms;
    if (job.agent_id.has_value()) j["agentId"] = *job.agent_id;
    if (job.session_key.has_value()) j["sessionKey"] = *job.session_key;
    j["deleteAfterRun"] = job.delete_after_run;

    // schedule
    nlohmann::json schedule;
    switch (job.schedule.kind) {
        case CronSchedule::Kind::AT:
            schedule["kind"] = "at";
            schedule["at"] = job.schedule.at;
            break;
        case CronSchedule::Kind::EVERY:
            schedule["kind"] = "every";
            schedule["everyMs"] = job.schedule.every_ms;
            if (job.schedule.anchor_ms.has_value()) schedule["anchorMs"] = *job.schedule.anchor_ms;
            break;
        case CronSchedule::Kind::CRON:
            schedule["kind"] = "cron";
            schedule["expr"] = job.schedule.expr;
            if (job.schedule.tz.has_value()) schedule["tz"] = *job.schedule.tz;
            if (job.schedule.stagger_ms > 0) schedule["staggerMs"] = job.schedule.stagger_ms;
            break;
    }
    j["schedule"] = schedule;

    // payload
    nlohmann::json payload;
    if (job.payload.kind == CronPayload::Kind::SYSTEM_EVENT) {
        payload["kind"] = "systemEvent";
        payload["text"] = job.payload.text;
    } else {
        payload["kind"] = "agentTurn";
        payload["message"] = job.payload.message;
        if (job.payload.model.has_value()) payload["model"] = *job.payload.model;
        if (job.payload.thinking.has_value()) payload["thinking"] = *job.payload.thinking;
        if (job.payload.timeout_seconds > 0) payload["timeoutSeconds"] = job.payload.timeout_seconds;
        if (job.payload.allow_unsafe_external_content) payload["allowUnsafeExternalContent"] = true;
        if (job.payload.light_context) payload["lightContext"] = true;
        if (!job.payload.fallbacks.empty()) {
            nlohmann::json fallbacks = nlohmann::json::array();
            for (const auto& f : job.payload.fallbacks) fallbacks.push_back(f);
            payload["fallbacks"] = fallbacks;
        }
    }
    j["payload"] = payload;

    // delivery
    nlohmann::json delivery;
    switch (job.delivery.mode) {
        case CronDeliveryMode::NONE:     delivery["mode"] = "none"; break;
        case CronDeliveryMode::ANNOUNCE: delivery["mode"] = "announce"; break;
        case CronDeliveryMode::WEBHOOK:  delivery["mode"] = "webhook"; break;
    }
    if (job.delivery.channel.has_value()) delivery["channel"] = *job.delivery.channel;
    if (job.delivery.to.has_value()) delivery["to"] = *job.delivery.to;
    if (job.delivery.account_id.has_value()) delivery["accountId"] = *job.delivery.account_id;
    if (job.delivery.best_effort) delivery["bestEffort"] = true;
    if (job.delivery.failure_destination.has_value()) {
        nlohmann::json fd;
        const auto& fdd = *job.delivery.failure_destination;
        if (fdd.channel.has_value()) fd["channel"] = *fdd.channel;
        if (fdd.to.has_value()) fd["to"] = *fdd.to;
        if (fdd.account_id.has_value()) fd["accountId"] = *fdd.account_id;
        if (fdd.mode.has_value()) fd["mode"] = *fdd.mode;
        delivery["failureDestination"] = fd;
    }
    j["delivery"] = delivery;

    // failureAlert
    if (job.failure_alert_disabled) {
        j["failureAlert"] = false;
    } else if (job.failure_alert.has_value()) {
        nlohmann::json fa;
        fa["after"] = job.failure_alert->after;
        if (job.failure_alert->channel.has_value()) fa["channel"] = *job.failure_alert->channel;
        if (job.failure_alert->to.has_value()) fa["to"] = *job.failure_alert->to;
        if (job.failure_alert->cooldown_ms > 0) fa["cooldownMs"] = job.failure_alert->cooldown_ms;
        fa["mode"] = job.failure_alert->mode;
        if (job.failure_alert->account_id.has_value()) fa["accountId"] = *job.failure_alert->account_id;
        j["failureAlert"] = fa;
    }

    // state
    nlohmann::json state;
    if (job.state.next_run_at_ms.has_value()) state["nextRunAtMs"] = *job.state.next_run_at_ms;
    if (job.state.running_at_ms.has_value()) state["runningAtMs"] = *job.state.running_at_ms;
    if (job.state.last_run_at_ms.has_value()) state["lastRunAtMs"] = *job.state.last_run_at_ms;
    if (job.state.last_run_status.has_value()) {
        state["lastRunStatus"] = cron_run_status_to_string(*job.state.last_run_status);
    }
    if (job.state.last_error.has_value()) state["lastError"] = *job.state.last_error;
    if (job.state.last_duration_ms.has_value()) state["lastDurationMs"] = *job.state.last_duration_ms;
    state["consecutiveErrors"] = job.state.consecutive_errors;
    if (job.state.last_failure_alert_at_ms.has_value()) {
        state["lastFailureAlertAtMs"] = *job.state.last_failure_alert_at_ms;
    }
    if (job.state.last_delivery_status.has_value()) {
        state["lastDeliveryStatus"] = *job.state.last_delivery_status;
    }
    if (job.state.last_delivery_error.has_value()) {
        state["lastDeliveryError"] = *job.state.last_delivery_error;
    }
    state["lastDelivered"] = job.state.last_delivered;
    j["state"] = state;

    return j;
}

CronJobCreate GatewayCronIntegration::json_to_job_create(const nlohmann::json& j) {
    CronJobCreate job_create;
    
    job_create.name = j.at("name").get<std::string>();
    
    if (j.contains("enabled")) {
        job_create.enabled = j["enabled"].get<bool>();
    }
    
    if (j.contains("sessionTarget")) {
        job_create.session_target = j["sessionTarget"].get<std::string>();
    }
    
    if (j.contains("wakeMode")) {
        job_create.wake_mode = j["wakeMode"].get<std::string>();
    }
    
    // ✅ 对齐 OpenClaw: 解析 schedule, payload, delivery
    if (j.contains("schedule")) {
        const auto& s = j["schedule"];
        std::string kind = s.value("kind", "cron");
        if (kind == "at") {
            job_create.schedule.kind = CronSchedule::Kind::AT;
            job_create.schedule.at = s.value("at", "");
        } else if (kind == "every") {
            job_create.schedule.kind = CronSchedule::Kind::EVERY;
            job_create.schedule.every_ms = s.value("everyMs", 0LL);
        } else {
            job_create.schedule.kind = CronSchedule::Kind::CRON;
            job_create.schedule.expr = s.value("expr", "");
            if (s.contains("tz")) job_create.schedule.tz = s["tz"].get<std::string>();
        }
    }
    if (j.contains("payload")) {
        const auto& p = j["payload"];
        std::string kind = p.value("kind", "agentTurn");
        if (kind == "systemEvent") {
            job_create.payload.kind = CronPayload::Kind::SYSTEM_EVENT;
            job_create.payload.text = p.value("text", "");
        } else {
            job_create.payload.kind = CronPayload::Kind::AGENT_TURN;
            job_create.payload.message = p.value("message", "");
            if (p.contains("model")) job_create.payload.model = p["model"].get<std::string>();
            if (p.contains("thinking")) job_create.payload.thinking = p["thinking"].get<std::string>();
            if (p.contains("timeoutSeconds")) job_create.payload.timeout_seconds = p["timeoutSeconds"].get<int>();
            if (p.contains("allowUnsafeExternalContent")) job_create.payload.allow_unsafe_external_content = p["allowUnsafeExternalContent"].get<bool>();
            if (p.contains("lightContext")) job_create.payload.light_context = p["lightContext"].get<bool>();
            if (p.contains("fallbacks") && p["fallbacks"].is_array()) {
                for (const auto& f : p["fallbacks"]) {
                    job_create.payload.fallbacks.push_back(f.get<std::string>());
                }
            }
        }
    }
    if (j.contains("delivery")) {
        const auto& d = j["delivery"];
        std::string mode = d.value("mode", "none");
        if (mode == "announce") job_create.delivery.mode = CronDeliveryMode::ANNOUNCE;
        else if (mode == "webhook") job_create.delivery.mode = CronDeliveryMode::WEBHOOK;
        else job_create.delivery.mode = CronDeliveryMode::NONE;
        if (d.contains("channel")) job_create.delivery.channel = d["channel"].get<std::string>();
        if (d.contains("to")) job_create.delivery.to = d["to"].get<std::string>();
        if (d.contains("accountId")) job_create.delivery.account_id = d["accountId"].get<std::string>();
        if (d.contains("bestEffort")) job_create.delivery.best_effort = d["bestEffort"].get<bool>();
    }
    if (j.contains("failureAlert")) {
        if (j["failureAlert"].is_boolean() && !j["failureAlert"].get<bool>()) {
            job_create.failure_alert_disabled = true;
        } else if (j["failureAlert"].is_object()) {
            const auto& fa = j["failureAlert"];
            CronFailureAlert alert;
            if (fa.contains("after")) alert.after = fa["after"].get<int>();
            if (fa.contains("channel")) alert.channel = fa["channel"].get<std::string>();
            if (fa.contains("to")) alert.to = fa["to"].get<std::string>();
            if (fa.contains("cooldownMs")) alert.cooldown_ms = fa["cooldownMs"].get<int64_t>();
            if (fa.contains("mode")) alert.mode = fa["mode"].get<std::string>();
            if (fa.contains("accountId")) alert.account_id = fa["accountId"].get<std::string>();
            job_create.failure_alert = alert;
        }
    }
    if (j.contains("deleteAfterRun")) {
        job_create.delete_after_run = j["deleteAfterRun"].get<bool>();
    }
    if (j.contains("description")) {
        job_create.description = j["description"].get<std::string>();
    }
    if (j.contains("agentId")) {
        job_create.agent_id = j["agentId"].get<std::string>();
    }
    if (j.contains("sessionKey")) {
        job_create.session_key = j["sessionKey"].get<std::string>();
    }
    return job_create;
}

CronJobPatch GatewayCronIntegration::json_to_job_patch(const nlohmann::json& j) {
    CronJobPatch patch;
    
    if (j.contains("name")) {
        patch.name = j["name"].get<std::string>();
    }
    
    if (j.contains("enabled")) {
        patch.enabled = j["enabled"].get<bool>();
    }
    
    // ✅ 对齐 OpenClaw: 解析 schedule, payload, delivery
    if (j.contains("schedule")) {
        const auto& s = j["schedule"];
        CronSchedule sched;
        std::string kind = s.value("kind", "cron");
        if (kind == "at") {
            sched.kind = CronSchedule::Kind::AT;
            sched.at = s.value("at", "");
        } else if (kind == "every") {
            sched.kind = CronSchedule::Kind::EVERY;
            sched.every_ms = s.value("everyMs", 0LL);
        } else {
            sched.kind = CronSchedule::Kind::CRON;
            sched.expr = s.value("expr", "");
        }
        patch.schedule = sched;
    }
    if (j.contains("payload")) {
        const auto& p = j["payload"];
        std::string kind = p.value("kind", "agentTurn");
        CronPayload payload;
        if (kind == "systemEvent") {
            payload.kind = CronPayload::Kind::SYSTEM_EVENT;
            payload.text = p.value("text", "");
        } else {
            payload.kind = CronPayload::Kind::AGENT_TURN;
            payload.message = p.value("message", "");
            if (p.contains("model")) payload.model = p["model"].get<std::string>();
            if (p.contains("thinking")) payload.thinking = p["thinking"].get<std::string>();
            if (p.contains("timeoutSeconds")) payload.timeout_seconds = p["timeoutSeconds"].get<int>();
            if (p.contains("allowUnsafeExternalContent")) payload.allow_unsafe_external_content = p["allowUnsafeExternalContent"].get<bool>();
            if (p.contains("lightContext")) payload.light_context = p["lightContext"].get<bool>();
            if (p.contains("fallbacks") && p["fallbacks"].is_array()) {
                for (const auto& f : p["fallbacks"]) {
                    payload.fallbacks.push_back(f.get<std::string>());
                }
            }
        }
        patch.payload = payload;
    }
    if (j.contains("delivery")) {
        const auto& d = j["delivery"];
        CronDelivery delivery;
        std::string mode = d.value("mode", "none");
        if (mode == "announce") delivery.mode = CronDeliveryMode::ANNOUNCE;
        else if (mode == "webhook") delivery.mode = CronDeliveryMode::WEBHOOK;
        if (d.contains("channel")) delivery.channel = d["channel"].get<std::string>();
        if (d.contains("to")) delivery.to = d["to"].get<std::string>();
        if (d.contains("accountId")) delivery.account_id = d["accountId"].get<std::string>();
        if (d.contains("bestEffort")) delivery.best_effort = d["bestEffort"].get<bool>();
        patch.delivery = delivery;
    }
    if (j.contains("failureAlert")) {
        if (j["failureAlert"].is_boolean() && !j["failureAlert"].get<bool>()) {
            patch.failure_alert_disabled = true;
        } else if (j["failureAlert"].is_object()) {
            const auto& fa = j["failureAlert"];
            CronFailureAlert alert;
            if (fa.contains("after")) alert.after = fa["after"].get<int>();
            if (fa.contains("channel")) alert.channel = fa["channel"].get<std::string>();
            if (fa.contains("to")) alert.to = fa["to"].get<std::string>();
            if (fa.contains("cooldownMs")) alert.cooldown_ms = fa["cooldownMs"].get<int64_t>();
            if (fa.contains("mode")) alert.mode = fa["mode"].get<std::string>();
            if (fa.contains("accountId")) alert.account_id = fa["accountId"].get<std::string>();
            patch.failure_alert = alert;
        }
    }
    if (j.contains("deleteAfterRun")) {
        patch.delete_after_run = j["deleteAfterRun"].get<bool>();
    }
    if (j.contains("description")) {
        patch.description = j["description"].get<std::string>();
    }
    if (j.contains("agentId")) {
        patch.agent_id = j["agentId"].get<std::string>();
    }
    if (j.contains("sessionKey")) {
        patch.session_key = j["sessionKey"].get<std::string>();
    }
    return patch;
}

} // namespace cron
} // namespace hunterclaw
