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
    
    // Parse params
    WakeMode mode = WakeMode::Now;
    std::string text = "Manual wake";
    
    if (params.contains("mode")) {
        std::string mode_str = params["mode"].get<std::string>();
        if (mode_str == "next-heartbeat") {
            mode = WakeMode::NextHeartbeat;
        } else if (mode_str == "now") {
            mode = WakeMode::Now;
        } else {
            log()->warn("Unknown wake mode: {}, defaulting to 'now'", mode_str);
        }
    }
    
    if (params.contains("text")) {
        text = params["text"].get<std::string>();
    }
    
    // Trigger wake
    service_->wake(mode, text);
    
    return {
        {"success", true}
    };
}

nlohmann::json GatewayCronIntegration::handle_status(const nlohmann::json& params) {
    log()->debug("handle_status: {}", params.dump());
    
    auto status = service_->status();
    
    nlohmann::json result = {
        {"running", status.running},
        {"totalJobs", status.total_jobs},
        {"enabledJobs", status.enabled_jobs},
        {"disabledJobs", status.disabled_jobs}
    };
    
    if (status.next_wake_at_ms.has_value()) {
        result["nextWakeAtMs"] = *status.next_wake_at_ms;
    }
    
    if (status.next_job_id.has_value()) {
        result["nextJobId"] = *status.next_job_id;
    }
    
    return result;
}

nlohmann::json GatewayCronIntegration::handle_list(const nlohmann::json& params) {
    log()->debug("handle_list: {}", params.dump());
    
    bool include_disabled = false;
    if (params.contains("includeDisabled")) {
        include_disabled = params["includeDisabled"].get<bool>();
    }
    
    auto jobs = service_->list(include_disabled);
    
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
        auto result = service_->add(job_create);
        
        if (!result.ok) {
            return {
                {"error", result.error.value_or("Failed to add job")}
            };
        }
        
        return {
            {"job", job_to_json(*result.value)}
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
        auto result = service_->update(id, patch);
        
        if (!result.ok) {
            return {
                {"error", result.error.value_or("Failed to update job")}
            };
        }
        
        return {
            {"job", job_to_json(*result.value)}
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
    auto result = service_->remove(id);
    
    if (!result.ok) {
        return {
            {"error", result.error.value_or("Failed to remove job")}
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
    bool force = false;
    
    if (params.contains("force")) {
        force = params["force"].get<bool>();
    }
    
    auto result = service_->run(id, force);
    
    if (!result.ok) {
        return {
            {"error", result.error.value_or("Failed to run job")}
        };
    }
    
    return {
        {"success", true}
    };
}

nlohmann::json GatewayCronIntegration::handle_runs(const nlohmann::json& params) {
    log()->debug("handle_runs: {}", params.dump());
    
    if (!params.contains("id")) {
        return {{"error", "Missing 'id' parameter"}};
    }
    
    std::string id = params["id"].get<std::string>();
    int limit = 50;
    
    if (params.contains("limit")) {
        limit = params["limit"].get<int>();
    }
    
    auto logs = service_->get_run_logs(id, limit);
    
    nlohmann::json logs_json = nlohmann::json::array();
    for (const auto& entry : logs) {
        logs_json.push_back(entry.to_json());
    }
    
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

nlohmann::json GatewayCronIntegration::job_to_json(const Job& job) {
    nlohmann::json j;
    cron::to_json(j, job);
    return j;
}

JobCreate GatewayCronIntegration::json_to_job_create(const nlohmann::json& j) {
    JobCreate job_create;
    
    job_create.name = j.at("name").get<std::string>();
    
    if (j.contains("enabled")) {
        job_create.enabled = j["enabled"].get<bool>();
    }
    
    if (j.contains("schedule")) {
        cron::from_json(j["schedule"], job_create.schedule);
    }
    
    if (j.contains("sessionTarget")) {
        std::string target_str = j["sessionTarget"].get<std::string>();
        if (target_str == "main") {
            job_create.session_target = SessionTarget::Main;
        } else if (target_str == "isolated") {
            job_create.session_target = SessionTarget::Isolated;
        }
    }
    
    if (j.contains("wakeMode")) {
        std::string wake_str = j["wakeMode"].get<std::string>();
        if (wake_str == "now") {
            job_create.wake_mode = WakeMode::Now;
        } else if (wake_str == "next-heartbeat") {
            job_create.wake_mode = WakeMode::NextHeartbeat;
        }
    }
    
    if (j.contains("payload")) {
        cron::from_json(j["payload"], job_create.payload);
    }
    
    if (j.contains("delivery")) {
        Delivery delivery;
        cron::from_json(j["delivery"], delivery);
        job_create.delivery = delivery;
    }
    
    if (j.contains("agentId")) {
        job_create.agent_id = j["agentId"].get<std::string>();
    }
    
    if (j.contains("sessionKey")) {
        job_create.session_key = j["sessionKey"].get<std::string>();
    }
    
    if (j.contains("lane")) {
        job_create.lane = j["lane"].get<std::string>();
    }
    
    return job_create;
}

JobPatch GatewayCronIntegration::json_to_job_patch(const nlohmann::json& j) {
    JobPatch patch;
    
    if (j.contains("name")) {
        patch.name = j["name"].get<std::string>();
    }
    
    if (j.contains("enabled")) {
        patch.enabled = j["enabled"].get<bool>();
    }
    
    if (j.contains("schedule")) {
        Schedule schedule;
        cron::from_json(j["schedule"], schedule);
        patch.schedule = schedule;
    }
    
    if (j.contains("sessionTarget")) {
        std::string target_str = j["sessionTarget"].get<std::string>();
        if (target_str == "main") {
            patch.session_target = SessionTarget::Main;
        } else if (target_str == "isolated") {
            patch.session_target = SessionTarget::Isolated;
        }
    }
    
    if (j.contains("wakeMode")) {
        std::string wake_str = j["wakeMode"].get<std::string>();
        if (wake_str == "now") {
            patch.wake_mode = WakeMode::Now;
        } else if (wake_str == "next-heartbeat") {
            patch.wake_mode = WakeMode::NextHeartbeat;
        }
    }
    
    if (j.contains("payload")) {
        Payload payload;
        cron::from_json(j["payload"], payload);
        patch.payload = payload;
    }
    
    if (j.contains("delivery")) {
        Delivery delivery;
        cron::from_json(j["delivery"], delivery);
        patch.delivery = delivery;
    }
    
    return patch;
}

} // namespace cron
} // namespace hunterclaw
