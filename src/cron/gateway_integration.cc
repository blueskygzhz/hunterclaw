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
    // ✅ 对齐 OpenClaw: 完整的 CronJob JSON 序列化
    j["id"] = job.id;
    j["name"] = job.name;
    j["enabled"] = job.enabled;
    j["sessionTarget"] = job.session_target;
    j["wakeMode"] = job.wake_mode;
    j["createdAtMs"] = job.created_at_ms;
    j["updatedAtMs"] = job.updated_at_ms;
    // schedule
    nlohmann::json schedule;
    switch (job.schedule.kind) {
        case CronSchedule::Kind::AT:   schedule["kind"] = "at"; schedule["at"] = job.schedule.at; break;
        case CronSchedule::Kind::EVERY: schedule["kind"] = "every"; schedule["everyMs"] = job.schedule.every_ms; break;
        case CronSchedule::Kind::CRON:  schedule["kind"] = "cron"; schedule["expr"] = job.schedule.expr; break;
    }
    j["schedule"] = schedule;
    // payload
    nlohmann::json payload;
    payload["kind"] = (job.payload.kind == CronPayload::Kind::AGENT_TURN) ? "agent-turn" : "system-event";
    payload["text"] = job.payload.text;
    j["payload"] = payload;
    // delivery
    nlohmann::json delivery;
    switch (job.delivery.mode) {
        case CronDeliveryMode::NONE:     delivery["mode"] = "none"; break;
        case CronDeliveryMode::ANNOUNCE: delivery["mode"] = "announce"; break;
        case CronDeliveryMode::WEBHOOK:  delivery["mode"] = "webhook"; break;
    }
    if (job.delivery.channel) delivery["channel"] = *job.delivery.channel;
    if (job.delivery.to) delivery["to"] = *job.delivery.to;
    j["delivery"] = delivery;
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
        std::string kind = p.value("kind", "agent-turn");
        job_create.payload.kind = (kind == "system-event") ? CronPayload::Kind::SYSTEM_EVENT : CronPayload::Kind::AGENT_TURN;
        job_create.payload.text = p.value("text", "");
    }
    if (j.contains("delivery")) {
        const auto& d = j["delivery"];
        std::string mode = d.value("mode", "none");
        if (mode == "announce") job_create.delivery.mode = CronDeliveryMode::ANNOUNCE;
        else if (mode == "webhook") job_create.delivery.mode = CronDeliveryMode::WEBHOOK;
        else job_create.delivery.mode = CronDeliveryMode::NONE;
        if (d.contains("channel")) job_create.delivery.channel = d["channel"].get<std::string>();
        if (d.contains("to")) job_create.delivery.to = d["to"].get<std::string>();
    }
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
        CronPayload payload;
        std::string kind = p.value("kind", "agent-turn");
        payload.kind = (kind == "system-event") ? CronPayload::Kind::SYSTEM_EVENT : CronPayload::Kind::AGENT_TURN;
        payload.text = p.value("text", "");
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
        patch.delivery = delivery;
    }
}

} // namespace cron
} // namespace hunterclaw
