// ============================================================================
// push.cc - Push notification testing
// 🎯 严格对齐 OpenClaw server-methods/push.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// push.test
core::Result<json> method_push_test(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string node_id = params.value("nodeId", "");
    if (node_id.empty()) {
        return core::Result<json>::Err("nodeId required");
    }

    // In production, would send APNs test push
    json result;
    result["ok"] = false;
    result["error"] = "push notifications not configured";
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
