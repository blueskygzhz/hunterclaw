#include "hunterclaw/gateway/methods.h"
#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <functional>
#include <map>
#include <string>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ============================================================================
// create_method_handler - RPC Method Dispatcher
// Called from gateway.cc to initialize the method handler
// ============================================================================

std::function<core::Result<json>(const std::string&, const json&, const ClientContext&)>
create_method_handler(std::shared_ptr<GatewayServer> server) {
    using MethodFn = std::function<core::Result<json>(
        std::shared_ptr<GatewayServer>,
        std::shared_ptr<MethodGuard>,
        const ClientContext&,
        const json&
    )>;

    // Build method dispatch table
    // 🎯 OpenClaw 严格对齐: server-methods/ 目录下所有 handler 方法名
    static const std::map<std::string, MethodFn> method_table = {
        // ── Health (health.ts) ──────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: healthHandlers["health"] / healthHandlers["status"]
        {"health",                  methods::method_health},
        {"status",                  methods::method_status},
        // HunterClaw 扩展 (调试用)
        {"ping",                    methods::method_ping},
        {"echo",                    methods::method_echo},

        // ── Connect (connect.ts) ────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: connectHandlers["connect"]
        // 握手完成后再收到 connect 应返回错误 (由 gateway.cc 握手状态机处理)
        // 这里注册一个 fallback，防止已连接状态下重复 connect
        {"connect",                 methods::method_connect_fallback},

        // ── Sessions (sessions.ts) ──────────────────────────────────────────
        // 🎯 OpenClaw 对齐: sessionsHandlers
        {"sessions.list",           methods::method_session_list},
        {"sessions.preview",        methods::method_session_preview},
        {"sessions.resolve",        methods::method_session_resolve},
        {"sessions.patch",          methods::method_session_patch},
        {"sessions.reset",          methods::method_session_reset},
        {"sessions.delete",         methods::method_session_delete},
        {"sessions.get",            methods::method_session_get},
        {"sessions.compact",        methods::method_session_compact},

        // ── Channels (channels.ts) ──────────────────────────────────────────
        // 🎯 OpenClaw 对齐: channelsHandlers
        {"channels.status",         methods::method_channel_status},
        {"channels.logout",         methods::method_channel_logout},
        // HunterClaw 扩展方法 (OpenClaw 无对应，保留兼容)
        {"channel.list",            methods::method_channel_list},
        {"channel.get",             methods::method_channel_get},
        {"channel.start",           methods::method_channel_start},
        {"channel.stop",            methods::method_channel_stop},
        {"channel.send",            methods::method_channel_send},
        {"channel.health",          methods::method_channel_health},
        {"channels.capabilities",   methods::method_channel_capabilities},
        {"channels.resolve",        methods::method_channel_resolve},
        {"channels.logs",           methods::method_channel_logs},

        // ── Agent (agent.ts) ────────────────────────────────────────────────
        // HunterClaw 扩展 (OpenClaw 无对应)
        {"agent.cancel",            methods::method_agent_cancel},
        {"agent.status",            methods::method_agent_status},

        // ── Chat (chat.ts) ──────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: chatHandlers
        {"chat.send",               methods::method_chat_send},
        {"chat.abort",              methods::method_chat_abort},
        {"chat.history",            methods::method_chat_history},
        {"chat.inject",             methods::method_chat_inject},
        // HunterClaw 扩展 (OpenClaw 无对应)
        {"chat.update_params",      methods::method_chat_update_params},
        {"chat.regenerate",         methods::method_chat_regenerate},
        {"chat.fork",               methods::method_chat_fork},
        {"chat.summarize",          methods::method_chat_summarize},
        {"chat.edit",               methods::method_chat_edit},
        {"chat.delete",             methods::method_chat_delete},
        {"chat.transcript",         methods::method_chat_transcript},
        {"chat.export",             methods::method_chat_export},

        // ── Config (config.ts) ──────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: configHandlers
        {"config.get",              methods::method_config_get},
        {"config.schema",           methods::method_config_schema},
        {"config.schema.lookup",    methods::method_config_schema_lookup},
        {"config.set",              methods::method_config_set},
        {"config.patch",            methods::method_config_patch},
        {"config.apply",            methods::method_config_apply},
        {"config.openFile",         methods::method_config_open_file},
        // HunterClaw 扩展 (OpenClaw 无对应)
        {"config.reload",           methods::method_config_reload},
        {"config.list",             methods::method_config_list},
        {"config.validate",         methods::method_config_validate},
        {"config.reset",            methods::method_config_reset},
        {"config.export",           methods::method_config_export},

        // ── Agents (agents.ts) ──────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: agentsHandlers
        {"agents.list",                     methods::method_agents_list},
        {"agents.create",                   methods::method_agents_create},
        {"agents.update",                   methods::method_agents_update},
        {"agents.delete",                   methods::method_agents_delete},
        {"agents.files.list",               methods::method_agents_files_list},
        {"agents.files.get",                methods::method_agents_files_get},
        {"agents.files.set",                methods::method_agents_files_set},

        // ── Agent (agent.ts) ────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: agentHandlers
        {"agent.identity.get",              methods::method_agent_identity_get},
        {"agent.wait",                      methods::method_agent_wait},

        // ── Node (nodes.ts) ─────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: nodeHandlers
        {"node.pair.request",               methods::method_node_pair_request},
        {"node.pair.list",                  methods::method_node_pair_list},
        {"node.pair.approve",               methods::method_node_pair_approve},
        {"node.pair.reject",                methods::method_node_pair_reject},
        {"node.pair.verify",                methods::method_node_pair_verify},
        {"node.rename",                     methods::method_node_rename},
        {"node.list",                       methods::method_node_list},
        {"node.describe",                   methods::method_node_describe},
        {"node.canvas.capability.refresh",  methods::method_node_canvas_capability_refresh},
        {"node.pending.pull",               methods::method_node_pending_pull},
        {"node.pending.ack",                methods::method_node_pending_ack},
        {"node.pending.drain",              methods::method_node_pending_drain},
        {"node.pending.enqueue",            methods::method_node_pending_enqueue},
        {"node.invoke",                     methods::method_node_invoke},
        {"node.invoke.result",              methods::method_node_invoke_result},
        {"node.event",                      methods::method_node_event},

        // ── Logs (logs.ts) ──────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: logsHandlers
        {"logs.tail",                       methods::method_logs_tail},

        // ── Models (models.ts) ──────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: modelsHandlers
        {"models.list",                     methods::method_models_list},

        // ── Skills (skills.ts) ──────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: skillsHandlers (新增)
        {"skills.status",                   methods::method_skills_status},
        {"skills.bins",                     methods::method_skills_bins},
        {"skills.install",                  methods::method_skills_install},
        {"skills.update",                   methods::method_skills_update},
        // HunterClaw 扩展 (OpenClaw 无对应)
        {"skill.list",              methods::method_skill_list},
        {"skill.get",               methods::method_skill_get},

        // ── Secrets (secrets.ts) ────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: secretsHandlers
        {"secrets.reload",                  methods::method_secrets_reload},
        {"secrets.resolve",                 methods::method_secrets_resolve},

        // ── Usage (usage.ts) ────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: usageHandlers
        {"usage.status",                    methods::method_usage_status},
        {"usage.cost",                      methods::method_usage_cost},
        {"sessions.usage",                  methods::method_sessions_usage},
        {"sessions.usage.timeseries",       methods::method_sessions_usage_timeseries},
        {"sessions.usage.logs",             methods::method_sessions_usage_logs},

        // ── TTS (tts.ts) ────────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: ttsHandlers
        {"tts.status",                      methods::method_tts_status},
        {"tts.enable",                      methods::method_tts_enable},
        {"tts.disable",                     methods::method_tts_disable},
        {"tts.convert",                     methods::method_tts_convert},
        {"tts.setProvider",                 methods::method_tts_set_provider},
        {"tts.providers",                   methods::method_tts_providers},

        // ── Talk (talk.ts) ──────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: talkHandlers
        {"talk.config",                     methods::method_talk_config},
        {"talk.mode",                       methods::method_talk_mode},

        // ── Web (web.ts) ────────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: webHandlers
        {"web.login.start",                 methods::method_web_login_start},
        {"web.login.wait",                  methods::method_web_login_wait},

        // ── Browser (browser.ts) ────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: browserHandlers
        {"browser.request",                 methods::method_browser_request},

        // ── Push (push.ts) ──────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: pushHandlers
        {"push.test",                       methods::method_push_test},

        // ── Device (devices.ts) ─────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: devicesHandlers
        {"device.pair.list",                methods::method_device_pair_list},
        {"device.pair.approve",             methods::method_device_pair_approve},
        {"device.pair.reject",              methods::method_device_pair_reject},
        {"device.pair.remove",              methods::method_device_pair_remove},
        {"device.token.rotate",             methods::method_device_token_rotate},
        {"device.token.revoke",             methods::method_device_token_revoke},

        // ── Exec Approval (exec-approval.ts) ────────────────────────────────
        // 🎯 OpenClaw 对齐: execApprovalHandlers
        {"exec.approval.request",           methods::method_exec_approval_request},
        {"exec.approval.waitDecision",      methods::method_exec_approval_wait_decision},
        {"exec.approval.resolve",           methods::method_exec_approval_resolve},

        // ── Exec Approvals (exec-approvals.ts) ──────────────────────────────
        // 🎯 OpenClaw 对齐: execApprovalsHandlers
        {"exec.approvals.get",              methods::method_exec_approvals_get},
        {"exec.approvals.set",              methods::method_exec_approvals_set},
        {"exec.approvals.node.get",         methods::method_exec_approvals_node_get},
        {"exec.approvals.node.set",         methods::method_exec_approvals_node_set},

        // ── Doctor (doctor.ts) ──────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: doctorHandlers
        {"doctor.memory.status",            methods::method_doctor_memory_status},

        // ── System (system.ts) ──────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: systemHandlers
        {"gateway.identity.get",            methods::method_gateway_identity_get},

        // ── Tools Catalog (tools-catalog.ts) ────────────────────────────────
        // 🎯 OpenClaw 对齐: toolsCatalogHandlers
        {"tools.catalog",                   methods::method_tools_catalog},

        // ── Update (update.ts) ──────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: updateHandlers
        {"update.run",                      methods::method_update_run},

        // ── Send (send.ts) ──────────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: sendHandlers
        // OpenClaw BASE_METHODS 中有 "send" (SendParams) 和 "send.message"
        {"send",                            methods::method_send},
        {"send.message",                    methods::method_send_message},

        // ── Agent top-level (agent.ts) ──────────────────────────────────────
        // 🎯 OpenClaw 对齐: agentHandlers["agent"] (AgentParams)
        {"agent",                           methods::method_agent},

        // ── Wizard (wizard.ts) ──────────────────────────────────────────────
        // 🎯 OpenClaw 对齐: wizardHandlers
        {"wizard.start",                    methods::method_wizard_start},
        {"wizard.next",                     methods::method_wizard_next},
        {"wizard.cancel",                   methods::method_wizard_cancel},
        {"wizard.status",                   methods::method_wizard_status},

        // ── VoiceWake (voicewake.ts) ─────────────────────────────────────────
        // 🎯 OpenClaw 对齐: voicewakeHandlers
        {"voicewake.get",                   methods::method_voicewake_get},
        {"voicewake.set",                   methods::method_voicewake_set},

        // ── Heartbeat (heartbeat.ts) ─────────────────────────────────────────
        // 🎯 OpenClaw 对齐: BASE_METHODS "last-heartbeat" / "set-heartbeats"
        {"last-heartbeat",                  methods::method_last_heartbeat},
        {"set-heartbeats",                  methods::method_set_heartbeats},

        // ── System Presence / Event (system.ts) ─────────────────────────────
        // 🎯 OpenClaw 对齐: systemHandlers "system-presence" / "system-event"
        {"system-presence",                 methods::method_system_presence},
        {"system-event",                    methods::method_system_event},

        // ── Cron (cron.ts) ──────────────────────────────────────────────────
        {"wake",                    methods::method_wake},
        {"cron.status",             methods::method_cron_status},
        {"cron.list",               methods::method_cron_list},
        {"cron.add",                methods::method_cron_add},
        {"cron.update",             methods::method_cron_update},
        {"cron.remove",             methods::method_cron_remove},
        {"cron.run",                methods::method_cron_run},
        {"cron.runs",               methods::method_cron_runs},
    };

    return [server](const std::string& method_name, const json& params, const ClientContext& client_ctx) -> core::Result<json> {
        auto it = method_table.find(method_name);
        if (it == method_table.end()) {
            return core::Result<json>::Err("Unknown method: " + method_name);
        }

        // Create a method guard
        // MethodGuard requires AuthManager; pass nullptr for now
        auto guard = std::make_shared<MethodGuard>(nullptr);

        try {
            return it->second(server, guard, client_ctx, params);
        } catch (const std::exception& e) {
            return core::Result<json>::Err(
                std::string("Method ") + method_name + " threw: " + e.what()
            );
        }
    };
}

namespace methods {

// ============================================================================
// Method Registration Implementation
// ============================================================================

void register_all_methods(std::shared_ptr<GatewayServer> server) {
    if (!server) {
        return;
    }
    // Method registration is handled via create_method_handler dispatch table
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
