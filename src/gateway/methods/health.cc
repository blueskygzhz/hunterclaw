#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Core Methods (Health & Debug)
// ============================================================================

// 1. ping - Health check
core::Result<json> method_ping(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server;
    (void)method_guard;
    (void)client_ctx;
    (void)params;
    
    json result;
    result["pong"] = true;
    result["timestamp"] = core::utils::now().time_since_epoch().count();
    result["server"] = "HunterClaw";
    result["version"] = "0.1.0";
    return core::Result<json>::Ok(result);
}

// 2. echo - Echo back params (debug)
core::Result<json> method_echo(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server;
    (void)method_guard;
    (void)client_ctx;
    
    return core::Result<json>::Ok(params);
}

// 3. status - Get server status
core::Result<json> method_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;
    (void)params;
    
    auto runtime = server->runtime_state();
    
    json result;
    result["server"] = "HunterClaw";
    result["version"] = "0.1.0";
    result["uptime_ms"] = 0;  // TODO: Add start_time to GatewayRuntimeState
    
    result["sessions_count"] = runtime->sessions.size();
    result["channels_count"] = 0;  // TODO: Fix ChannelManager::list_channels() access
    result["memory_mb"] = 0;  // TODO: Implement memory tracking
    
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
