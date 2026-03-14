#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Agent Methods
// ============================================================================

// 14. agent.cancel - Cancel a running agent (NEW)
core::Result<json> method_agent_cancel(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("run_id")) {
        return core::Result<json>::Err("Missing required parameter: run_id");
    }

    std::string run_id = params["run_id"];

    // Get abort controller from state
    auto state = server->runtime_state();
    auto it = state->abort_controllers.find(run_id);

    if (it == state->abort_controllers.end()) {
        return core::Result<json>::Err("Run not found or already completed: " + run_id);
    }

    // Signal abort
    it->second->store(true);

    // Broadcast agent.cancelled event
    core::EventFrame cancelled_event;
    cancelled_event.event = "agent.cancelled";
    cancelled_event.payload = json::object();
    cancelled_event.payload["run_id"] = run_id;
    cancelled_event.payload["timestamp"] = core::utils::now().time_since_epoch().count();
    server->broadcast(cancelled_event);

    json result;
    result["run_id"] = run_id;
    result["status"] = "cancelled";
    result["cancelled_at"] = core::utils::now().time_since_epoch().count();

    return core::Result<json>::Ok(result);
}

// 15. agent.status - Get agent run status (NEW)
core::Result<json> method_agent_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("run_id")) {
        return core::Result<json>::Err("Missing required parameter: run_id");
    }

    std::string run_id = params["run_id"];

    // Check if run exists
    auto state = server->runtime_state();
    auto it = state->abort_controllers.find(run_id);

    json result;
    result["run_id"] = run_id;

    if (it == state->abort_controllers.end()) {
        // Run completed or never existed
        result["status"] = "completed";
        result["exists"] = false;
    } else {
        // Run is active
        bool is_aborted = it->second->load();
        result["status"] = is_aborted ? "cancelling" : "running";
        result["exists"] = true;
    }

    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
