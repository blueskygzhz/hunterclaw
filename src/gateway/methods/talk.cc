// ============================================================================
// talk.cc - Voice/talk mode management
// 🎯 严格对齐 OpenClaw server-methods/talk.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static bool s_talk_enabled = false;

static inline int64_t talk_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// talk.config
core::Result<json> method_talk_config(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    json result;
    result["enabled"] = s_talk_enabled;
    return core::Result<json>::Ok(result);
}

// talk.mode
core::Result<json> method_talk_mode(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string mode = params.value("mode", "");
    if (mode.empty()) {
        return core::Result<json>::Err("mode required");
    }

    // Broadcast mode change
    json event;
    event["mode"] = mode;
    event["ts"] = talk_now_ms();
    server->broadcast("talk.mode", event);

    json result;
    result["ok"] = true;
    result["mode"] = mode;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
