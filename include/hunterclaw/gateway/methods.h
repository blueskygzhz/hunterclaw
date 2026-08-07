#pragma once

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/result.h"
#include <nlohmann/json.hpp>
#include <memory>

namespace hunterclaw {
namespace gateway {

// Forward declarations
class GatewayServer;
class MethodGuard;
struct ClientContext;

namespace methods {

using json = nlohmann::json;

// ============================================================================
// Health Methods (health.cc)
// 🎯 OpenClaw 严格对齐: server-methods/health.ts
// ============================================================================

// 🎯 OpenClaw 对齐: healthHandlers["health"]
core::Result<json> method_health(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: healthHandlers["status"]
// (已在下方 method_status 声明)

// HunterClaw 扩展 (调试用)
core::Result<json> method_ping(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_echo(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// ============================================================================
// Connect Fallback (health.cc)
// 🎯 OpenClaw 对齐: connect.ts - 握手完成后再收到 connect 返回错误
// ============================================================================

core::Result<json> method_connect_fallback(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// ============================================================================
// Session Methods (sessions.cc)
// 🎯 OpenClaw 严格对齐: server-methods/sessions.ts sessionsHandlers
// ============================================================================

// 🎯 OpenClaw 对齐: sessions.list
core::Result<json> method_session_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: sessions.preview
core::Result<json> method_session_preview(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: sessions.resolve
core::Result<json> method_session_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: sessions.patch
core::Result<json> method_session_patch(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: sessions.reset
core::Result<json> method_session_reset(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: sessions.delete
core::Result<json> method_session_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: sessions.get
core::Result<json> method_session_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: sessions.compact
core::Result<json> method_session_compact(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// HunterClaw 扩展 (OpenClaw 无对应)
core::Result<json> method_session_create(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_session_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// ============================================================================
// Channel Methods (channels.cc)
// 🎯 OpenClaw 严格对齐: server-methods/channels.ts
// ============================================================================

// 🎯 OpenClaw 对齐: channels.status
core::Result<json> method_channel_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: channels.logout
core::Result<json> method_channel_logout(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// HunterClaw 扩展方法 (OpenClaw 无对应)
core::Result<json> method_channel_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_channel_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_channel_start(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_channel_stop(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_channel_send(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_channel_health(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_channel_capabilities(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_channel_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_channel_logs(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// ============================================================================
// Agent Methods (agent.cc) - 2 methods
// ============================================================================

core::Result<json> method_agent_cancel(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_agent_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// ============================================================================
// Chat Methods (chat.cc) - 12 methods
// ============================================================================

core::Result<json> method_chat_send(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_abort(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_history(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_update_params(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_regenerate(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_fork(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_summarize(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_inject(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_edit(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_transcript(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_chat_export(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// ============================================================================
// Config Methods (config.cc)
// 🎯 OpenClaw 严格对齐: server-methods/config.ts configHandlers
// ============================================================================

// 🎯 OpenClaw 对齐: config.get
core::Result<json> method_config_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: config.schema
core::Result<json> method_config_schema(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: config.schema.lookup
core::Result<json> method_config_schema_lookup(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: config.set
core::Result<json> method_config_set(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: config.apply
core::Result<json> method_config_apply(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// 🎯 OpenClaw 对齐: config.openFile
core::Result<json> method_config_open_file(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// HunterClaw 扩展 (OpenClaw 无对应)
core::Result<json> method_config_reload(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_config_patch(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_config_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_config_validate(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_config_reset(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_config_export(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// ============================================================================
// Skill Methods (skills.cc) - 2 methods
// ============================================================================

core::Result<json> method_skill_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_skill_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// ============================================================================
// Cron Methods (cron.cc) - 8 methods
// ============================================================================

core::Result<json> method_wake(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_cron_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_cron_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_cron_add(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_cron_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_cron_remove(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_cron_run(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

core::Result<json> method_cron_runs(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
);

// ============================================================================
// Agents Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/agents.ts agentsHandlers
// ============================================================================
core::Result<json> method_agents_list(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_agents_create(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_agents_update(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_agents_delete(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_agents_files_list(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_agents_files_get(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_agents_files_set(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Agent Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/agent.ts agentHandlers
// ============================================================================
core::Result<json> method_agent_identity_get(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_agent_wait(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Node Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/nodes.ts nodeHandlers
// ============================================================================
core::Result<json> method_node_pair_request(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_pair_list(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_pair_approve(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_pair_reject(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_pair_verify(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_rename(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_list(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_describe(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_canvas_capability_refresh(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_pending_pull(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_pending_ack(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_pending_drain(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_pending_enqueue(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_invoke(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_invoke_result(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_node_event(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Logs Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/logs.ts logsHandlers
// ============================================================================
core::Result<json> method_logs_tail(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Models Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/models.ts modelsHandlers
// ============================================================================
core::Result<json> method_models_list(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Skills Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/skills.ts skillsHandlers
// ============================================================================
core::Result<json> method_skills_status(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_skills_bins(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_skills_install(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_skills_update(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// 🎯 Skill Proposal (Workshop) Methods — 对齐 OpenClaw skills.proposals.*
core::Result<json> method_skills_proposals_list(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_skills_proposals_events_list(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_skills_proposals_inspect(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_skills_proposals_evaluate(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_skills_proposals_action(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Secrets Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/secrets.ts secretsHandlers
// ============================================================================
core::Result<json> method_secrets_reload(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_secrets_resolve(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Usage Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/usage.ts usageHandlers
// ============================================================================
core::Result<json> method_usage_status(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_usage_cost(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_sessions_usage(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_sessions_usage_timeseries(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_sessions_usage_logs(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// TTS Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/tts.ts ttsHandlers
// ============================================================================
core::Result<json> method_tts_status(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_tts_enable(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_tts_disable(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_tts_convert(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_tts_set_provider(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_tts_providers(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Talk Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/talk.ts talkHandlers
// ============================================================================
core::Result<json> method_talk_config(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_talk_mode(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Web Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/web.ts webHandlers
// ============================================================================
core::Result<json> method_web_login_start(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_web_login_wait(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Browser Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/browser.ts browserHandlers
// ============================================================================
core::Result<json> method_browser_request(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Push Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/push.ts pushHandlers
// ============================================================================
core::Result<json> method_push_test(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Device Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/devices.ts devicesHandlers
// ============================================================================
core::Result<json> method_device_pair_list(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_device_pair_approve(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_device_pair_reject(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_device_pair_remove(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_device_token_rotate(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_device_token_revoke(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Exec Approval Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/exec-approval.ts execApprovalHandlers
// ============================================================================
core::Result<json> method_exec_approval_request(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_exec_approval_wait_decision(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_exec_approval_resolve(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Exec Approvals Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/exec-approvals.ts execApprovalsHandlers
// ============================================================================
core::Result<json> method_exec_approvals_get(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_exec_approvals_set(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_exec_approvals_node_get(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_exec_approvals_node_set(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Doctor Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/doctor.ts doctorHandlers
// ============================================================================
core::Result<json> method_doctor_memory_status(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// System Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/system.ts systemHandlers
// ============================================================================
core::Result<json> method_gateway_identity_get(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Tools Catalog Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/tools-catalog.ts toolsCatalogHandlers
// ============================================================================
core::Result<json> method_tools_catalog(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Update Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/update.ts updateHandlers
// ============================================================================
core::Result<json> method_update_run(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Wizard Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/wizard.ts wizardHandlers
// ============================================================================
core::Result<json> method_wizard_start(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_wizard_next(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_wizard_cancel(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_wizard_status(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// VoiceWake Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/voicewake.ts voicewakeHandlers
// ============================================================================
core::Result<json> method_voicewake_get(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_voicewake_set(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Send Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/send.ts sendHandlers
// ============================================================================
// 🎯 OpenClaw 对齐: "send" (SendParams)
core::Result<json> method_send(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_send_message(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
// 🎯 OpenClaw 对齐: "poll" (PollParams)
core::Result<json> method_poll(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Agent (top-level) Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/agent.ts agentHandlers["agent"]
// ============================================================================
// 🎯 OpenClaw 对齐: "agent" (AgentParams) - top-level agent method
core::Result<json> method_agent(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Heartbeat Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/heartbeat.ts
// ============================================================================
core::Result<json> method_last_heartbeat(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_set_heartbeats(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// System Presence / Event Methods (stubs.cc)
// 🎯 OpenClaw 严格对齐: server-methods/system.ts systemHandlers
// ============================================================================
core::Result<json> method_system_presence(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);
core::Result<json> method_system_event(std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>, const ClientContext&, const json&);

// ============================================================================
// Method Registration Helper
// ============================================================================

// 注册所有模块化方法到 Gateway
void register_all_methods(std::shared_ptr<GatewayServer> server);

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
