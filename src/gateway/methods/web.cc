// ============================================================================
// web.cc - Web login flow
// 🎯 严格对齐 OpenClaw server-methods/web.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// web.login.start
core::Result<json> method_web_login_start(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    std::string session_id = core::utils::generate_uuid();

    json result;
    result["sessionId"] = session_id;
    result["url"] = "https://auth.hunterclaw.dev/login?session=" + session_id;
    return core::Result<json>::Ok(result);
}

// web.login.wait
core::Result<json> method_web_login_wait(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string session_id = params.value("sessionId", "");
    if (session_id.empty()) {
        return core::Result<json>::Err("sessionId required");
    }

    // In production, would poll for auth completion
    json result;
    result["status"] = "pending";
    result["sessionId"] = session_id;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
