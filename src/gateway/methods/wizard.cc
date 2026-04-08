// ============================================================================
// wizard.cc - Setup wizard flow management
// 🎯 严格对齐 OpenClaw server-methods/wizard.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <map>
#include <mutex>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t wizard_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Wizard state tracking
static std::mutex s_wizard_mutex;
static std::map<std::string, json> s_wizard_sessions;  // wizardId → state

// wizard.start
core::Result<json> method_wizard_start(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string wizard_id = core::utils::generate_uuid();
    std::string type = params.value("type", "setup");

    json state;
    state["wizardId"] = wizard_id;
    state["type"] = type;
    state["step"] = 0;
    state["status"] = "active";
    state["startedAtMs"] = wizard_now_ms();

    {
        std::lock_guard<std::mutex> lock(s_wizard_mutex);
        s_wizard_sessions[wizard_id] = state;
    }

    json result;
    result["wizardId"] = wizard_id;
    result["step"] = 0;
    result["status"] = "active";
    return core::Result<json>::Ok(result);
}

// wizard.next
core::Result<json> method_wizard_next(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string wizard_id = params.value("wizardId", "");
    if (wizard_id.empty()) {
        return core::Result<json>::Err("wizardId required");
    }

    std::lock_guard<std::mutex> lock(s_wizard_mutex);
    auto it = s_wizard_sessions.find(wizard_id);
    if (it == s_wizard_sessions.end()) {
        return core::Result<json>::Err("unknown wizardId");
    }

    auto& state = it->second;
    int step = state.value("step", 0) + 1;
    state["step"] = step;

    if (params.contains("response")) {
        state["lastResponse"] = params["response"];
    }

    json result;
    result["wizardId"] = wizard_id;
    result["step"] = step;
    result["status"] = "active";
    return core::Result<json>::Ok(result);
}

// wizard.cancel
core::Result<json> method_wizard_cancel(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string wizard_id = params.value("wizardId", "");
    if (wizard_id.empty()) {
        return core::Result<json>::Err("wizardId required");
    }

    std::lock_guard<std::mutex> lock(s_wizard_mutex);
    auto it = s_wizard_sessions.find(wizard_id);
    if (it != s_wizard_sessions.end()) {
        it->second["status"] = "cancelled";
        s_wizard_sessions.erase(it);
    }

    json result;
    result["ok"] = true;
    result["wizardId"] = wizard_id;
    return core::Result<json>::Ok(result);
}

// wizard.status
core::Result<json> method_wizard_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string wizard_id = params.value("wizardId", "");
    if (wizard_id.empty()) {
        return core::Result<json>::Err("wizardId required");
    }

    std::lock_guard<std::mutex> lock(s_wizard_mutex);
    auto it = s_wizard_sessions.find(wizard_id);
    if (it == s_wizard_sessions.end()) {
        json result;
        result["wizardId"] = wizard_id;
        result["status"] = "not_found";
        return core::Result<json>::Ok(result);
    }

    return core::Result<json>::Ok(it->second);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
