// ============================================================================
// tts.cc - Text-to-speech management
// 🎯 严格对齐 OpenClaw server-methods/tts.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <mutex>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static std::mutex s_tts_mutex;
static bool s_tts_enabled = false;
static std::string s_tts_provider = "";

// tts.status
core::Result<json> method_tts_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    std::lock_guard<std::mutex> lock(s_tts_mutex);
    json result;
    result["enabled"] = s_tts_enabled;
    result["provider"] = s_tts_provider.empty() ? json(nullptr) : json(s_tts_provider);
    return core::Result<json>::Ok(result);
}

// tts.enable
core::Result<json> method_tts_enable(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    std::lock_guard<std::mutex> lock(s_tts_mutex);
    s_tts_enabled = true;

    json result;
    result["ok"] = true;
    result["enabled"] = true;
    return core::Result<json>::Ok(result);
}

// tts.disable
core::Result<json> method_tts_disable(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    std::lock_guard<std::mutex> lock(s_tts_mutex);
    s_tts_enabled = false;

    json result;
    result["ok"] = true;
    result["enabled"] = false;
    return core::Result<json>::Ok(result);
}

// tts.convert
core::Result<json> method_tts_convert(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string text = params.value("text", "");
    if (text.empty()) {
        return core::Result<json>::Err("text required for TTS conversion");
    }

    // TTS conversion would delegate to provider
    json result;
    result["ok"] = false;
    result["error"] = "TTS provider not configured";
    return core::Result<json>::Ok(result);
}

// tts.setProvider
core::Result<json> method_tts_set_provider(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string provider = params.value("provider", "");
    {
        std::lock_guard<std::mutex> lock(s_tts_mutex);
        s_tts_provider = provider;
    }

    json result;
    result["ok"] = true;
    result["provider"] = provider;
    return core::Result<json>::Ok(result);
}

// tts.providers
core::Result<json> method_tts_providers(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    json result;
    result["providers"] = json::array();
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
