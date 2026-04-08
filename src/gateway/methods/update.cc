// ============================================================================
// update.cc - Self-update mechanism
// 🎯 严格对齐 OpenClaw server-methods/update.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// update.run
core::Result<json> method_update_run(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    // In production, would trigger self-update mechanism
    json result;
    result["ok"] = false;
    result["error"] = "self-update not available in C++ build";
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
