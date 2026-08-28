// ============================================================================
// plugin_approval.cc - Plugin Approval Flow
// 🎯 严格对齐 OpenClaw server-methods/plugin-approval.ts
//
// 核心功能:
//   - plugin.approval.list: 列出待审批的插件请求
//   - plugin.approval.request: 创建插件审批请求
//   - plugin.approval.resolve: 审批/拒绝插件请求  (legacy, 见下)
//   - plugin.approval.waitDecision: 等待审批决定
//
// ★★ 与 exec approval 共用同一个 ExecApprovalManager (对齐 OpenClaw 的
//    【一个 store + 两类 payload】结构), 但归属由 payload 的【形状】判别
//    (PluginApprovalRequestPayload 有 title+description), 【不是】靠 id 前缀。
//
// ⚠ 本文件此前有四处偏离, 均已修正:
//   ① payload.command = request.dump()  —— 把整个 plugin 请求塞进 exec 的
//      command 字段。按 resolveApprovalRequestKind 的判据(有 command → exec),
//      它会被识别成【exec 审批】: 客户端按命令卡片渲染一段 JSON,
//      allow-always 还会把这段 JSON 当命令写进白名单。
//   ② decision 校验只认 "approve"/"deny" —— OpenClaw 的决策集是
//      "allow-once"/"allow-always"/"deny"。客户端发 allow-once 会被拒,
//      而 UI 按钮发的正是 allow-once。
//   ③ plugin.approval.list 永远返回空数组 —— 客户端看不到任何待审请求。
//   ④ resolve 未做前缀匹配/存在性校验, 与 exec.approval.resolve 行为不一致。
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/gateway/exec_approval_manager.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

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
    if (!mgr) return core::Result<json>::Ok(result);

    // ★ 修正 ③: 原实现只调 has_pending() 然后什么都不返回,
    //   客户端因此永远看不到待审的工具审批 —— 表现为
    //   "审批请求发了但列表是空的", 且日志里一个字都没有。
    for (const auto& rec : mgr->list_pending(ApprovalKind::Plugin)) {
        json item;
        item["id"] = rec.id;
        item["request"] = rec.request_to_json();
        item["createdAtMs"] = rec.created_at_ms;
        item["expiresAtMs"] = rec.expires_at_ms;
        result.push_back(std::move(item));
    }
    return core::Result<json>::Ok(result);
}

// plugin.approval.request (对齐 plugin-approval.ts:43-139)
core::Result<json> method_plugin_approval_request(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> /*method_guard*/,
    const ClientContext& /*client_ctx*/,
    const json& params
) {
    // Validate required params
    if (!params.contains("title") || !params["title"].is_string() ||
        !params.contains("description") || !params["description"].is_string()) {
        return core::Result<json>::Err("title and description required");
    }

    auto mgr = server->exec_approval_manager();
    if (!mgr) return core::Result<json>::Err("approval manager not available");

    // ★ 修正 ①: 构建【真正的】PluginApprovalRequestPayload,
    //   而不是把 JSON 塞进 exec 的 command 字段。
    PluginApprovalRequestPayload payload;
    payload.title = params["title"].get<std::string>();
    payload.description = params["description"].get<std::string>();
    auto opt_str = [&params](const char* k) -> std::optional<std::string> {
        auto it = params.find(k);
        if (it == params.end() || !it->is_string()) return std::nullopt;
        return it->get<std::string>();
    };
    payload.plugin_id = opt_str("pluginId");
    payload.detail = opt_str("detail");
    payload.severity = opt_str("severity");
    payload.tool_name = opt_str("toolName");
    payload.tool_call_id = opt_str("toolCallId");
    payload.agent_id = opt_str("agentId");
    payload.session_key = opt_str("sessionKey");
    payload.turn_source_channel = opt_str("turnSourceChannel");
    payload.turn_source_to = opt_str("turnSourceTo");
    payload.turn_source_account_id = opt_str("turnSourceAccountId");
    {
        auto it = params.find("turnSourceThreadId");
        if (it != params.end()) payload.turn_source_thread_id = *it;
    }
    {
        auto it = params.find("allowedDecisions");
        if (it != params.end() && it->is_array()) {
            std::vector<std::string> v;
            for (const auto& d : *it) {
                if (d.is_string()) v.push_back(d.get<std::string>());
            }
            if (!v.empty()) payload.allowed_decisions = std::move(v);
        }
    }

    int64_t timeout_ms = params.value("timeoutMs",
                                     (int64_t)DEFAULT_PLUGIN_APPROVAL_TIMEOUT_MS);
    bool two_phase = params.value("twoPhase", false);

    // ⚠ 不再自己拼 "plugin:" 前缀 —— 归属由 payload 形状决定。
    //   前缀会让归属判别有【两个来源】(前缀 与 形状), 两者不一致时
    //   行为取决于哪段代码先判, 是典型的静默分歧。
    auto record = mgr->create_plugin(payload, timeout_ms);
    if (!mgr->register_record(record, record.expires_at_ms - record.created_at_ms)) {
        return core::Result<json>::Err("failed to register approval request");
    }

    // 广播事件
    json event;
    event["id"] = record.id;
    event["request"] = record.request_to_json();
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
    auto decision = mgr->await_decision(
        record.id, record.expires_at_ms - record.created_at_ms);
    json result;
    result["id"] = record.id;
    result["decision"] = decision.has_value() ? *decision : "timeout";
    result["resolved"] = decision.has_value();
    return core::Result<json>::Ok(result);
}

// plugin.approval.resolve (对齐 plugin-approval.ts:150-189)
//
// ⚠ LEGACY: 新客户端应使用 canonical `approval.resolve` (带 kind 参数)。
//   保留本方法是为了兼容既有客户端 —— OpenClaw 同样保留
//   exec.approval.resolve / plugin.approval.resolve 两个 legacy 方法
//   (approval-gateway-resolver.ts:137-156 明确标注 legacy)。
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

    // ★ 修正 ②: 决策集必须是 allow-once / allow-always / deny。
    //   原实现只认 "approve"/"deny" —— UI 按钮发的是 allow-once,
    //   会被直接拒掉, 而用户看到的是"审批失败"而非"参数不对"。
    const auto& allowed = default_plugin_approval_decisions();
    if (std::find(allowed.begin(), allowed.end(), decision) == allowed.end()) {
        return core::Result<json>::Err("invalid decision");
    }

    auto mgr = server->exec_approval_manager();
    if (!mgr) return core::Result<json>::Err("approval manager not available");

    // ★ 修正 ④: 与 exec.approval.resolve 一致地做前缀匹配 + 歧义处理。
    //   否则同一个 id 在两个 legacy 方法上行为不同(一个支持前缀一个不支持),
    //   客户端换个方法就失败, 而失败原因完全看不出来。
    auto lookup = mgr->lookup_pending_id(id);
    if (lookup.kind == ExecApprovalIdLookupResult::Kind::None) {
        return core::Result<json>::Err("unknown or expired approval id");
    }
    if (lookup.kind == ExecApprovalIdLookupResult::Kind::Ambiguous) {
        return core::Result<json>::Err(
            "ambiguous approval id prefix; use the full id");
    }
    const std::string approval_id = lookup.id;

    // ⚠ 归属校验: 本方法只该决策 plugin 审批。
    //   放行 exec 审批会让"plugin 方法批了一条 exec 命令" ——
    //   审计上完全对不上, 且绕过了 exec 侧可能有的额外校验。
    auto snap = mgr->get_snapshot(approval_id);
    if (snap.has_value() && snap->kind() != ApprovalKind::Plugin) {
        return core::Result<json>::Err(
            "approval id does not belong to a plugin approval");
    }

    // ⚠ allowedDecisions 限定: 载荷可声明本次不允许某些决策
    //   (例如"确认转账"不允许 allow-always —— 永久授权绝不行)。
    if (snap.has_value() && snap->plugin_request.has_value() &&
        snap->plugin_request->allowed_decisions.has_value()) {
        const auto& ad = *snap->plugin_request->allowed_decisions;
        if (std::find(ad.begin(), ad.end(), decision) == ad.end()) {
            return core::Result<json>::Err(
                "decision not allowed for this approval request");
        }
    }

    std::string resolved_by = client_ctx.conn_id;
    bool ok = mgr->resolve(approval_id, decision,
                          resolved_by.empty() ? std::nullopt
                                              : std::make_optional(resolved_by));

    if (ok) {
        // 广播 resolved 事件
        json event;
        event["id"] = approval_id;
        event["decision"] = decision;
        if (!resolved_by.empty()) event["resolvedBy"] = resolved_by;
        event["ts"] = plugin_approval_now_ms();
        core::EventFrame ef;
        ef.event = "plugin.approval.resolved";
        ef.payload = core::JsonValue(event);
        server->broadcast(ef);
    }

    json result;
    result["ok"] = ok;
    result["id"] = approval_id;
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
    if (!mgr) return core::Result<json>::Err("approval manager not available");
    auto decision = mgr->await_decision(id, DEFAULT_PLUGIN_APPROVAL_TIMEOUT_MS);

    json result;
    result["id"] = id;
    result["resolved"] = decision.has_value();
    if (decision.has_value()) {
        result["decision"] = *decision;
    }
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
