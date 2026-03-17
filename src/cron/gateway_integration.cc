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
    // TODO: Implement proper JSON serialization for CronJob
    j["id"] = job.id;
    j["name"] = job.name;
    j["enabled"] = job.enabled;
    j["sessionTarget"] = job.session_target;
    j["wakeMode"] = job.wake_mode;
    j["createdAtMs"] = job.created_at_ms;
    j["updatedAtMs"] = job.updated_at_ms;
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
    
    // TODO: Parse schedule, payload, delivery from JSON
    
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
    
    // TODO: Parse schedule, payload, delivery from JSON
    
    return patch;
}

} // namespace cron
} // namespace hunterclaw
