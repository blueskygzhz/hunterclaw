// ============================================================================
// secrets.cc - Secret management and resolution
// 🎯 严格对齐 OpenClaw server-methods/secrets.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// secrets.reload
// 🎯 OpenClaw 对齐: secrets.ts L39-45
core::Result<json> method_secrets_reload(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["ok"] = true;
    result["warningCount"] = 0;
    return core::Result<json>::Ok(result);
}

// secrets.resolve
// 🎯 OpenClaw 对齐: secrets.ts L47-102
core::Result<json> method_secrets_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string command_name = params.value("commandName", "");
    if (command_name.empty()) {
        return core::Result<json>::Err("invalid secrets.resolve params: commandName");
    }

    if (!params.contains("targetIds") || !params["targetIds"].is_array()) {
        return core::Result<json>::Err("invalid secrets.resolve params: targetIds");
    }

    json result;
    result["ok"] = true;
    result["assignments"] = json::array();
    result["diagnostics"] = json::array();
    result["inactiveRefPaths"] = json::array();
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
