// ============================================================================
// plugin_approval.cc - Plugin Approval Flow
// 🎯 严格对齐 OpenClaw server-methods/plugin-approval.ts (192 行)
//
// 核心功能:
//   - plugin.approval.list: 列出待审批的插件请求
//   - plugin.approval.request: 创建插件审批请求
//   - plugin.approval.resolve: 审批/拒绝插件请求
//   - plugin.approval.waitDecision: 等待审批决定
//
// 复用 exec_approval_manager 基础设施，通过 "plugin:" 前缀区分 exec/plugin
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/gateway/exec_approval_manager.h"
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static const int DEFAULT_PLUGIN_APPROVAL_TIMEOUT_MS = 120000;
static const int MAX_PLUGIN_APPROVAL_TIMEOUT_MS = 600000;

static inline int64_t plugin_approval_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// plugin.approval.list (对齐 plugin-approval.ts:31-41)
core::Result<json> method_plugin_approval_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> /*method_guard*/,
    const ClientContext& /*client_ctx*/,
    const json& /*params*/
) {
    auto mgr = server->exec_approval_manager();
    json result = json::array();
    // 列出以 "plugin:" 为前缀的 pending approvals
    auto pending = mgr->list_pending_records();
    for (const auto& record : pending) {
        if (record.id.substr(0, 7) != "plugin:") continue;
        json entry;
        entry["id"] = record.id;
        entry["request"] = record.request.to_json();
        entry["createdAtMs"] = record.created_at_ms;
        entry["expiresAtMs"] = record.expires_at_ms;
        result.push_back(entry);
    }
    return core::Result<json>::Ok(result);
}

// plugin.approval.request (对齐 plugin-approval.ts:43-139)
core::Result<json> method_plugin_approval_request(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> /*method_guard*/,
    const ClientContext& client_ctx,
    const json& params
) {
    // Validate required params
    if (!params.contains("title") || !params["title"].is_string() ||
        !params.contains("description") || !params["description"].is_string()) {
        return core::Result<json>::Err("title and description required");
    }

    // 构建 request payload (对齐 plugin-approval.ts:82-95)
    json request;
    request["pluginId"] = params.value("pluginId", json(nullptr));
    request["title"] = params["title"];
    request["description"] = params["description"];
    request["severity"] = params.value("severity", json(nullptr));
    request["toolName"] = params.value("toolName", json(nullptr));
    request["toolCallId"] = params.value("toolCallId", json(nullptr));
    request["agentId"] = params.value("agentId", json(nullptr));
    request["sessionKey"] = params.value("sessionKey", json(nullptr));
    request["turnSourceChannel"] = params.value("turnSourceChannel", json(nullptr));

    int timeout_ms = std::min(
        params.value("timeoutMs", DEFAULT_PLUGIN_APPROVAL_TIMEOUT_MS),
        MAX_PLUGIN_APPROVAL_TIMEOUT_MS);
    bool two_phase = params.value("twoPhase", false);

    // 创建审批记录 (对齐 plugin-approval.ts:99)
    auto mgr = server->exec_approval_manager();
    std::string id = "plugin:" + core::utils::generate_uuid();
    ExecApprovalRequestPayload payload;
    payload.command = request.dump();
    auto record = mgr->request(payload, timeout_ms);

    // 广播事件
    json event;
    event["id"] = record.id;
    event["request"] = request;
    event["createdAtMs"] = record.created_at_ms;
    event["expiresAtMs"] = record.expires_at_ms;
    core::EventFrame ef;
    ef.event = "plugin.approval.requested";
    ef.payload = core::JsonValue(event);
    server->broadcast(ef);

    if (two_phase) {
        // 两阶段: 先返回 pending
        json result;
        result["id"] = record.id;
        result["status"] = "pending";
        return core::Result<json>::Ok(result);
    }

    // 等待审批决定
    auto decision = mgr->wait_for_decision(record.id, timeout_ms);
    json result;
    result["id"] = record.id;
    result["decision"] = decision.has_value() ? decision->decision : "timeout";
    result["resolved"] = decision.has_value();
    return core::Result<json>::Ok(result);
}

// plugin.approval.resolve (对齐 plugin-approval.ts:150-189)
core::Result<json> method_plugin_approval_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> /*method_guard*/,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("id") || !params["id"].is_string() ||
        !params.contains("decision") || !params["decision"].is_string()) {
        return core::Result<json>::Err("id and decision required");
    }

    std::string id = params["id"].get<std::string>();
    std::string decision = params["decision"].get<std::string>();

    if (decision != "approve" && decision != "deny") {
        return core::Result<json>::Err("invalid decision (must be approve or deny)");
    }

    auto mgr = server->exec_approval_manager();
    bool ok = mgr->resolve(id, decision == "approve");

    if (ok) {
        // 广播 resolved 事件
        json event;
        event["id"] = id;
        event["decision"] = decision;
        event["resolvedBy"] = client_ctx.conn_id;
        event["ts"] = plugin_approval_now_ms();
        core::EventFrame ef;
        ef.event = "plugin.approval.resolved";
        ef.payload = core::JsonValue(event);
        server->broadcast(ef);
    }

    json result;
    result["ok"] = ok;
    result["id"] = id;
    result["decision"] = decision;
    return core::Result<json>::Ok(result);
}

// plugin.approval.waitDecision (对齐 plugin-approval.ts:142-148)
core::Result<json> method_plugin_approval_wait_decision(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> /*method_guard*/,
    const ClientContext& /*client_ctx*/,
    const json& params
) {
    if (!params.contains("id") || !params["id"].is_string()) {
        return core::Result<json>::Err("id required");
    }
    std::string id = params["id"].get<std::string>();
    auto mgr = server->exec_approval_manager();
    auto decision = mgr->wait_for_decision(id, DEFAULT_PLUGIN_APPROVAL_TIMEOUT_MS);

    json result;
    result["id"] = id;
    result["resolved"] = decision.has_value();
    if (decision.has_value()) {
        result["decision"] = decision->decision;
    }
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
