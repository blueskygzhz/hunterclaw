// ============================================================================
// system.cc - System presence and events
// 🎯 严格对齐 OpenClaw server-methods/system.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

static inline std::shared_ptr<spdlog::logger> get_system_logger() {
    auto logger = spdlog::get("default");
    if (!logger) logger = spdlog::get("console");
    return logger;
}

// system-presence
// 🎯 OpenClaw 对齐: system.ts L45-48
core::Result<json> method_system_presence(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    // List all connected clients as presence entries
    json entries = json::array();
    auto state = server->runtime_state();
    if (state) {
        for (const auto& [conn_id, client] : state->clients) {
            json entry;
            entry["connId"] = conn_id;
            entry["role"] = client ? client->connect.role : "operator";
            entries.push_back(entry);
        }
    }

    json result;
    result["entries"] = entries;
    result["ts"] = system_now_ms();
    return core::Result<json>::Ok(result);
}

// system-event
// 🎯 OpenClaw 对齐: system.ts L49-148
core::Result<json> method_system_event(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string text = params.value("text", "");
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    while (!text.empty() && text.back() == ' ') text.pop_back();

    if (text.empty()) {
        return core::Result<json>::Err("text required");
    }

    auto logger = get_system_logger();

    // 🎯 OpenClaw 对齐: Node state change detection (system.ts L100-138)
    bool is_node_line = text.find("Node:") == 0;

    if (is_node_line) {
        std::string reason = params.value("reason", "");
        std::string reason_lower = reason;
        std::transform(reason_lower.begin(), reason_lower.end(), reason_lower.begin(), ::tolower);
        if (reason_lower.find("periodic") == 0 || reason_lower == "heartbeat") {
            // Still update presence but don't enqueue system event
        } else {
            if (logger) logger->info("system-event (node): {}", text);
        }
    } else {
        if (logger) logger->info("system-event: {}", text);
    }

    // Broadcast presence update
    json presence_data;
    presence_data["text"] = text;
    presence_data["ts"] = system_now_ms();
    core::EventFrame ef;
    ef.event = "presence";
    ef.payload = core::JsonValue(presence_data);
    server->broadcast(ef);

    json result;
    result["ok"] = true;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
