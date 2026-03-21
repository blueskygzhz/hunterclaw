// ============================================================================
// stubs.cc - OpenClaw 对齐的 Stub 方法实现
// 🎯 严格对齐 OpenClaw server-methods/ 目录下所有方法
// 这些方法目前返回 stub 响应，待后续完整实现
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// 辅助宏: 生成 stub 方法
// ============================================================================
#define STUB_METHOD_NOT_IMPLEMENTED(name) \
    return core::Result<json>::Err(std::string(name) + " not yet implemented");

#define STUB_METHOD_OK(result_json) \
    return core::Result<json>::Ok(result_json);

static inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// Agents Methods (agents.ts)
// 🎯 OpenClaw 对齐: agentsHandlers
// ============================================================================

// agents.list
core::Result<json> method_agents_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["agents"] = json::array();
    STUB_METHOD_OK(result);
}

// agents.create
core::Result<json> method_agents_create(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("agents.create");
}

// agents.update
core::Result<json> method_agents_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("agents.update");
}

// agents.delete
core::Result<json> method_agents_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("agents.delete");
}

// agents.files.list
core::Result<json> method_agents_files_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["files"] = json::array();
    STUB_METHOD_OK(result);
}

// agents.files.get
core::Result<json> method_agents_files_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("agents.files.get");
}

// agents.files.set
core::Result<json> method_agents_files_set(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("agents.files.set");
}

// ============================================================================
// Agent Methods (agent.ts)
// 🎯 OpenClaw 对齐: agentHandlers
// ============================================================================

// agent.identity.get
core::Result<json> method_agent_identity_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["agentId"] = "main";
    result["name"] = "HunterClaw";
    STUB_METHOD_OK(result);
}

// agent.wait
core::Result<json> method_agent_wait(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("agent.wait");
}

// ============================================================================
// Node Methods (nodes.ts)
// 🎯 OpenClaw 对齐: nodeHandlers
// ============================================================================

// node.pair.request
core::Result<json> method_node_pair_request(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.pair.request");
}

// node.pair.list
core::Result<json> method_node_pair_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["pairs"] = json::array();
    STUB_METHOD_OK(result);
}

// node.pair.approve
core::Result<json> method_node_pair_approve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.pair.approve");
}

// node.pair.reject
core::Result<json> method_node_pair_reject(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.pair.reject");
}

// node.pair.verify
core::Result<json> method_node_pair_verify(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.pair.verify");
}

// node.rename
core::Result<json> method_node_rename(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.rename");
}

// node.list
core::Result<json> method_node_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["nodes"] = json::array();
    STUB_METHOD_OK(result);
}

// node.describe
core::Result<json> method_node_describe(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.describe");
}

// node.canvas.capability.refresh
core::Result<json> method_node_canvas_capability_refresh(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.canvas.capability.refresh");
}

// node.pending.pull
core::Result<json> method_node_pending_pull(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["items"] = json::array();
    STUB_METHOD_OK(result);
}

// node.pending.ack
core::Result<json> method_node_pending_ack(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.pending.ack");
}

// node.invoke
core::Result<json> method_node_invoke(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.invoke");
}

// node.event
core::Result<json> method_node_event(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("node.event");
}

// ============================================================================
// Logs Methods (logs.ts)
// 🎯 OpenClaw 对齐: logsHandlers
// ============================================================================

// logs.tail
core::Result<json> method_logs_tail(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["lines"] = json::array();
    result["ts"] = now_ms();
    STUB_METHOD_OK(result);
}

// ============================================================================
// Models Methods (models.ts)
// 🎯 OpenClaw 对齐: modelsHandlers
// ============================================================================

// models.list
core::Result<json> method_models_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["models"] = json::array();
    STUB_METHOD_OK(result);
}

// ============================================================================
// Skills Methods (skills.ts)
// 🎯 OpenClaw 对齐: skillsHandlers
// ============================================================================

// skills.status
core::Result<json> method_skills_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["ok"] = true;
    result["skills"] = json::array();
    STUB_METHOD_OK(result);
}

// skills.bins
core::Result<json> method_skills_bins(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["bins"] = json::array();
    STUB_METHOD_OK(result);
}

// skills.install
core::Result<json> method_skills_install(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("skills.install");
}

// skills.update
core::Result<json> method_skills_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("skills.update");
}

// ============================================================================
// Secrets Methods (secrets.ts)
// 🎯 OpenClaw 对齐: secretsHandlers
// ============================================================================

// secrets.reload
core::Result<json> method_secrets_reload(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["ok"] = true;
    STUB_METHOD_OK(result);
}

// secrets.resolve
core::Result<json> method_secrets_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("secrets.resolve");
}

// ============================================================================
// Usage Methods (usage.ts)
// 🎯 OpenClaw 对齐: usageHandlers
// ============================================================================

// usage.status
core::Result<json> method_usage_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["ts"] = now_ms();
    result["totalCost"] = 0.0;
    result["totalTokens"] = 0;
    STUB_METHOD_OK(result);
}

// usage.cost
core::Result<json> method_usage_cost(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["cost"] = 0.0;
    result["tokens"] = 0;
    STUB_METHOD_OK(result);
}

// sessions.usage
core::Result<json> method_sessions_usage(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["sessions"] = json::array();
    STUB_METHOD_OK(result);
}

// sessions.usage.timeseries
core::Result<json> method_sessions_usage_timeseries(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["timeseries"] = json::array();
    STUB_METHOD_OK(result);
}

// sessions.usage.logs
core::Result<json> method_sessions_usage_logs(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["logs"] = json::array();
    STUB_METHOD_OK(result);
}

// ============================================================================
// TTS Methods (tts.ts)
// 🎯 OpenClaw 对齐: ttsHandlers
// ============================================================================

// tts.status
core::Result<json> method_tts_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["enabled"] = false;
    result["provider"] = nullptr;
    STUB_METHOD_OK(result);
}

// tts.enable
core::Result<json> method_tts_enable(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("tts.enable");
}

// tts.disable
core::Result<json> method_tts_disable(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("tts.disable");
}

// tts.convert
core::Result<json> method_tts_convert(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("tts.convert");
}

// tts.setProvider
core::Result<json> method_tts_set_provider(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("tts.setProvider");
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
    STUB_METHOD_OK(result);
}

// ============================================================================
// Talk Methods (talk.ts)
// 🎯 OpenClaw 对齐: talkHandlers
// ============================================================================

// talk.config
core::Result<json> method_talk_config(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["enabled"] = false;
    STUB_METHOD_OK(result);
}

// talk.mode
core::Result<json> method_talk_mode(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("talk.mode");
}

// ============================================================================
// Web Methods (web.ts)
// 🎯 OpenClaw 对齐: webHandlers
// ============================================================================

// web.login.start
core::Result<json> method_web_login_start(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("web.login.start");
}

// web.login.wait
core::Result<json> method_web_login_wait(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("web.login.wait");
}

// ============================================================================
// Browser Methods (browser.ts)
// 🎯 OpenClaw 对齐: browserHandlers
// ============================================================================

// browser.request
core::Result<json> method_browser_request(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("browser.request");
}

// ============================================================================
// Push Methods (push.ts)
// 🎯 OpenClaw 对齐: pushHandlers
// ============================================================================

// push.test
core::Result<json> method_push_test(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("push.test");
}

// ============================================================================
// Device Methods (devices.ts)
// 🎯 OpenClaw 对齐: devicesHandlers
// ============================================================================

// device.pair.list
core::Result<json> method_device_pair_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["pairs"] = json::array();
    STUB_METHOD_OK(result);
}

// device.pair.approve
core::Result<json> method_device_pair_approve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("device.pair.approve");
}

// device.pair.reject
core::Result<json> method_device_pair_reject(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("device.pair.reject");
}

// device.pair.remove
core::Result<json> method_device_pair_remove(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("device.pair.remove");
}

// device.token.rotate
core::Result<json> method_device_token_rotate(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("device.token.rotate");
}

// device.token.revoke
core::Result<json> method_device_token_revoke(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("device.token.revoke");
}

// ============================================================================
// Exec Approval Methods (exec-approval.ts)
// 🎯 OpenClaw 对齐: execApprovalHandlers
// ============================================================================

// exec.approval.request
core::Result<json> method_exec_approval_request(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("exec.approval.request");
}

// exec.approval.waitDecision
core::Result<json> method_exec_approval_wait_decision(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("exec.approval.waitDecision");
}

// exec.approval.resolve
core::Result<json> method_exec_approval_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("exec.approval.resolve");
}

// ============================================================================
// Exec Approvals Methods (exec-approvals.ts)
// 🎯 OpenClaw 对齐: execApprovalsHandlers
// ============================================================================

// exec.approvals.get
core::Result<json> method_exec_approvals_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["approvals"] = json::array();
    STUB_METHOD_OK(result);
}

// exec.approvals.set
core::Result<json> method_exec_approvals_set(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("exec.approvals.set");
}

// exec.approvals.node.get
core::Result<json> method_exec_approvals_node_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["approvals"] = json::array();
    STUB_METHOD_OK(result);
}

// exec.approvals.node.set
core::Result<json> method_exec_approvals_node_set(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("exec.approvals.node.set");
}

// ============================================================================
// Doctor Methods (doctor.ts)
// 🎯 OpenClaw 对齐: doctorHandlers
// ============================================================================

// doctor.memory.status
core::Result<json> method_doctor_memory_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["ok"] = true;
    result["ts"] = now_ms();
    STUB_METHOD_OK(result);
}

// ============================================================================
// System Methods (system.ts)
// 🎯 OpenClaw 对齐: systemHandlers
// ============================================================================

// gateway.identity.get
// \ud83c\udfaf OpenClaw \u5bf9\u9f50: system.ts systemHandlers["gateway.identity.get"]
// \u5bf9\u5e94 OpenClaw: { deviceId, publicKey }
// \u5bf9\u5e94 OpenClaw: loadOrCreateDeviceIdentity() + publicKeyRawBase64UrlFromPem()
core::Result<json> method_gateway_identity_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    // TODO: \u52a0\u8f7d\u771f\u5b9e\u7684\u8bbe\u5907\u8eab\u4efd (device-identity.ts \u5bf9\u5e94\u5b9e\u73b0)
    // \u5bf9\u5e94 OpenClaw: const identity = loadOrCreateDeviceIdentity();
    // respond(true, { deviceId: identity.deviceId, publicKey: publicKeyRawBase64UrlFromPem(identity.publicKeyPem) })
    json result;
    result["deviceId"] = "hunterclaw-device-id";  // TODO: \u4ece\u8bbe\u5907\u8eab\u4efd\u6587\u4ef6\u52a0\u8f7d
    result["publicKey"] = nullptr;                 // TODO: \u4ece\u8bbe\u5907\u8eab\u4efd\u6587\u4ef6\u52a0\u8f7d
    STUB_METHOD_OK(result);
}

// ============================================================================
// Tools Catalog Methods (tools-catalog.ts)
// 🎯 OpenClaw 对齐: toolsCatalogHandlers
// ============================================================================

// tools.catalog
core::Result<json> method_tools_catalog(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    json result;
    result["tools"] = json::array();
    STUB_METHOD_OK(result);
}

// ============================================================================
// Update Methods (update.ts)
// 🎯 OpenClaw 对齐: updateHandlers
// ============================================================================

// update.run
core::Result<json> method_update_run(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("update.run");
}

// ============================================================================
// Send Methods (send.ts)
// 🎯 OpenClaw 对齐: sendHandlers
// ============================================================================

// send.message
core::Result<json> method_send_message(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    STUB_METHOD_NOT_IMPLEMENTED("send.message");
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
