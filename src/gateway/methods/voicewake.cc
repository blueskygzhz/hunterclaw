// ============================================================================
// voicewake.cc - Voice wake word management
// 🎯 严格对齐 OpenClaw server-methods/voicewake.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <mutex>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static std::mutex s_voicewake_mutex;
static bool s_voicewake_enabled = false;
static std::string s_voicewake_phrase = "";

// voicewake.get
core::Result<json> method_voicewake_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    std::lock_guard<std::mutex> lock(s_voicewake_mutex);
    json result;
    result["enabled"] = s_voicewake_enabled;
    result["phrase"] = s_voicewake_phrase.empty() ? json(nullptr) : json(s_voicewake_phrase);
    return core::Result<json>::Ok(result);
}

// voicewake.set
core::Result<json> method_voicewake_set(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    {
        std::lock_guard<std::mutex> lock(s_voicewake_mutex);
        if (params.contains("enabled") && params["enabled"].is_boolean()) {
            s_voicewake_enabled = params["enabled"].get<bool>();
        }
        if (params.contains("phrase") && params["phrase"].is_string()) {
            s_voicewake_phrase = params["phrase"].get<std::string>();
        }
    }

    // Broadcast change
    json event;
    event["enabled"] = s_voicewake_enabled;
    event["phrase"] = s_voicewake_phrase.empty() ? json(nullptr) : json(s_voicewake_phrase);
    server->broadcast("voicewake.changed", event);

    json result;
    result["ok"] = true;
    result["enabled"] = s_voicewake_enabled;
    result["phrase"] = s_voicewake_phrase.empty() ? json(nullptr) : json(s_voicewake_phrase);
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
