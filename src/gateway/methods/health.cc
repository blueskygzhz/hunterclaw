#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/channels/manager.h"  // ✅ For ChannelManager::get_runtime_snapshot()
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Core Methods (Health & Debug) - 完全对齐 OpenClaw
// ============================================================================

// ============================================================================
// health - 完整健康检查
// 🎯 OpenClaw 严格对齐: server-methods/health.ts healthHandlers["health"]
// 对应 OpenClaw: getStatusSummary + refreshHealthSnapshot
// ============================================================================
core::Result<json> method_health(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: probe 参数
    bool wants_probe = params.value("probe", false);

    int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    auto channel_mgr = server->channel_manager();
    auto runtime_snapshot = channel_mgr->get_runtime_snapshot();

    // 🎯 OpenClaw 对齐: channels 健康状态
    json channels_map = json::object();
    json channel_order = json::array();

    for (const auto& [ch_id, ch_state] : runtime_snapshot.channels) {
        std::string ch_id_str(ch_id);
        channel_order.push_back(ch_id_str);

        json ch_info;
        ch_info["configured"] = ch_state.configured.value_or(false);
        ch_info["running"]    = ch_state.running.value_or(false);
        ch_info["connected"]  = ch_state.connected.value_or(false);
        if (ch_state.last_error.has_value() && !ch_state.last_error.value().empty()) {
            ch_info["lastError"] = ch_state.last_error.value();
        }
        if (wants_probe) {
            json probe;
            probe["ok"] = ch_state.connected.value_or(false);
            probe["connected"] = ch_state.connected.value_or(false);
            probe["running"] = ch_state.running.value_or(false);
            ch_info["probe"] = probe;
        }
        channels_map[ch_id_str] = ch_info;
    }

    // 🎯 OpenClaw 对齐: 返回 getStatusSummary 格式
    json result;
    result["ts"] = ts_ms;
    result["ok"] = true;
    result["channels"] = channels_map;
    result["channelOrder"] = channel_order;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// status - 服务器状态摘要
// 🎯 OpenClaw 严格对齐: server-methods/health.ts healthHandlers["status"]
// 对应 OpenClaw: getStatusSummary({ includeSensitive })
// ============================================================================
core::Result<json> method_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    auto runtime = server->runtime_state();
    auto channel_mgr = server->channel_manager();
    auto runtime_snapshot = channel_mgr->get_runtime_snapshot();

    // 🎯 OpenClaw 对齐: status 摘要
    json result;
    result["ts"] = ts_ms;
    result["server"] = "HunterClaw";
    result["version"] = "1.0.0";
    result["sessionsCount"] = (int)runtime->sessions.size();

    // channels 摘要
    json channels_summary = json::object();
    for (const auto& [ch_id, ch_state] : runtime_snapshot.channels) {
        std::string ch_id_str(ch_id);
        json ch;
        ch["running"]   = ch_state.running.value_or(false);
        ch["connected"] = ch_state.connected.value_or(false);
        channels_summary[ch_id_str] = ch;
    }
    result["channels"] = channels_summary;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// connect (fallback) - 握手完成后再收到 connect 返回错误
// 🎯 OpenClaw 严格对齐: server-methods/connect.ts connectHandlers["connect"]
// 对应 OpenClaw: respond(false, undefined, errorShape(INVALID_REQUEST, "connect is only valid as the first request"))
// ============================================================================
core::Result<json> method_connect_fallback(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    return core::Result<json>::Err("connect is only valid as the first request");
}

// ============================================================================
// ping - 调试用健康检查 (HunterClaw 扩展)
// ============================================================================
core::Result<json> method_ping(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    json result;
    result["pong"] = true;
    result["timestamp"] = core::utils::now().time_since_epoch().count();
    result["server"] = "HunterClaw";
    result["version"] = "0.1.0";
    return core::Result<json>::Ok(result);
}

// echo - 调试回显
core::Result<json> method_echo(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    return core::Result<json>::Ok(params);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
