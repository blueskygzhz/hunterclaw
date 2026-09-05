#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/sessions/transcript_io.h"
#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/queue/command_queue.h"
#include "hunterclaw/queue/lanes.h"
#include "hunterclaw/queue/delivery_queue.h"   // ✅ DeliveryQueue (deliver 函数)
#include "hunterclaw/outbound/deliver.h"        // ✅ deliver_outbound_payloads
#include "hunterclaw/tasks/task_executor.h"     // ✅ 对齐 OC: createRunningTaskRun for agent run tracking
#include "hunterclaw/acp/acp_session_manager.h" // ✅ AcpSessionManager::run_turn (ACP subagent path)
#include "hunterclaw/acp/acp_reply_projector.h" // ✅ AcpReplyProjector (event → channel delivery)
#include "hunterclaw/acp/runtime_errors.h"      // ✅ AcpRuntimeError, format_acp_runtime_error_text
#include "hunterclaw/infra/agent_events.h"      // ✅ emit_agent_event (ACP → AgentEvent 总线桥接)
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cstdlib>   // ✅ std::getenv (resolve_present_proxy_env_keys)
#include <vector>
#include <string>

using json = nlohmann::json;

namespace {

// ── sanitize_acp_diagnostic_text ─────────────────────────────────────────────
// Aligned with openclaw: sanitizeAcpDiagnosticText (attempt-execution.ts:1564)
//   truncateUtf16Safe(redactSensitiveText(value).replace(/\s+/g, " ").trim(), 240)
std::string sanitize_acp_diagnostic_text(const std::string& value) {
    // 折叠所有连续空白为单空格
    std::string collapsed;
    collapsed.reserve(value.size());
    bool in_ws = false;
    for (char c : value) {
        const bool is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (is_ws) {
            in_ws = true;
            continue;
        }
        if (in_ws && !collapsed.empty()) collapsed += ' ';
        in_ws = false;
        collapsed += c;
    }
    // 截断到 240，且不切断 UTF-8 continuation byte
    constexpr size_t kMax = 240;
    if (collapsed.size() <= kMax) return collapsed;
    size_t cut = kMax;
    while (cut > 0 && (static_cast<unsigned char>(collapsed[cut]) & 0xC0) == 0x80) --cut;
    return collapsed.substr(0, cut);
}

// ── acp_runtime_event_diagnostics ────────────────────────────────────────────
// Aligned with openclaw: acpRuntimeEventDiagnostics (attempt-execution.ts:1568-1605)
json acp_runtime_event_diagnostics(const hunterclaw::acp::AcpRuntimeEvent& ev) {
    using Type = hunterclaw::acp::AcpRuntimeEvent::Type;
    json out = json::object();
    switch (ev.type) {
        case Type::Status:
            out["eventType"] = "status";
            out["text"] = sanitize_acp_diagnostic_text(ev.text);
            if (ev.tag) out["tag"] = *ev.tag;
            return out;
        case Type::ToolCall:
            out["eventType"] = "tool_call";
            out["text"] = sanitize_acp_diagnostic_text(ev.text);
            if (ev.tag)          out["tag"] = *ev.tag;
            if (ev.status)       out["status"] = sanitize_acp_diagnostic_text(*ev.status);
            if (ev.title)        out["title"] = sanitize_acp_diagnostic_text(*ev.title);
            if (ev.tool_call_id) out["toolCallId"] = sanitize_acp_diagnostic_text(*ev.tool_call_id);
            return out;
        case Type::Error:
            out["eventType"] = "error";
            out["message"] = sanitize_acp_diagnostic_text(ev.message.value_or(ev.text));
            if (ev.code)      out["code"] = sanitize_acp_diagnostic_text(*ev.code);
            if (ev.retryable) out["retryable"] = *ev.retryable;
            return out;
        case Type::Done:
            out["eventType"] = "done";
            if (ev.status)      out["status"] = *ev.status;
            if (ev.stop_reason) out["stopReason"] = sanitize_acp_diagnostic_text(*ev.stop_reason);
            return out;
        case Type::TextDelta:
        default:
            out["eventType"] = "text_delta";
            out["stream"] = ev.stream.value_or("output");
            return out;
    }
}

// ── emit_acp_runtime_event ───────────────────────────────────────────────────
// Aligned with openclaw: emitAcpRuntimeEvent (attempt-execution.ts:1620-1651)
//   emitAgentEvent({ runId, stream: "acp", data: { phase: "runtime_event", ...diagnostics } })
//
// 这是 ACP 路径唯一的 AgentEvent 事件源。缺它时 ParentStreamRelay 订阅了
// 全局总线但永远收不到 ACP 子会话事件，父会话只会在 60s 后收到 stall 告警。
void emit_acp_runtime_event(const std::string& run_id,
                            const hunterclaw::acp::AcpRuntimeEvent& ev) {
    if (run_id.empty()) return;
    hunterclaw::infra::AgentEvent bus_event;
    bus_event.run_id = run_id;
    bus_event.stream = "acp";
    json data = acp_runtime_event_diagnostics(ev);
    data["phase"] = "runtime_event";
    bus_event.data = std::move(data);
    hunterclaw::infra::emit_agent_event(bus_event);
}

// ── resolve_present_proxy_env_keys ───────────────────────────────────────────
// Aligned with openclaw: ACP_PROXY_ENV_KEYS (attempt-execution.ts:1326-1333) +
//   resolvePresentProxyEnvKeys (:1557-1562)
//
// 只上报「已设置且非空」的代理变量名（不含值，避免泄露凭据）。
// relay 在「prompt 已提交但上游无任何 runtime 事件」时用它提示排查方向。
std::vector<std::string> resolve_present_proxy_env_keys() {
    static const char* kProxyEnvKeys[] = {
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
        "http_proxy", "https_proxy", "all_proxy",
    };
    std::vector<std::string> present;
    for (const char* key : kProxyEnvKeys) {
        const char* value = std::getenv(key);
        if (value == nullptr) continue;
        std::string v(value);
        // 对齐真源 value.trim().length > 0
        if (v.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        present.emplace_back(key);
    }
    return present;
}

// ── emit_acp_prompt_submitted ────────────────────────────────────────────────
// Aligned with openclaw: emitAcpPromptSubmitted (attempt-execution.ts:1607-1618)
void emit_acp_prompt_submitted(const std::string& run_id, int64_t at_ms) {
    if (run_id.empty()) return;
    hunterclaw::infra::AgentEvent bus_event;
    bus_event.run_id = run_id;
    bus_event.stream = "acp";
    bus_event.data = json{
        {"phase", "prompt_submitted"},
        {"at", at_ms},
        // Aligned with openclaw: proxyEnvKeys: resolvePresentProxyEnvKeys()
        {"proxyEnvKeys", resolve_present_proxy_env_keys()},
    };
    hunterclaw::infra::emit_agent_event(bus_event);
}

} // namespace

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Agent Methods
// ============================================================================

// ============================================================================
// agent - 对齐 OpenClaw agentHandlers["agent"] (agent.ts)
//
// OpenClaw 完整链路:
//   callGateway("agent", { sessionKey, message, deliver, idempotencyKey, inputProvenance })
//   → Gateway agent handler:
//       1. 幂等去重: context.dedupe.get("agent:{idem}")
//       2. const runId = idem  ← 关键: runId = idempotencyKey
//       3. respond(accepted)   ← 立即返回 accepted
//       4. dispatchAgentRunFromGateway (异步)
//          → agentCommandFromIngress
//          → agent-runner-execution.ts: const runId = params.opts?.runId ?? crypto.randomUUID()
//
// HunterClaw 对齐:
//   1. 幂等去重: state->dedupe 检查
//   2. runId = idempotency_key  ← 确定性 run_id
//   3. 立即返回 accepted
//   4. 异步 dispatch: 通过 MessageDispatcher::run_reply_agent 或直接 process_message
// ============================================================================
core::Result<json> method_agent(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;

    // ========================================================================
    // Step 1: 参数提取
    // 同时支持 camelCase（GatewayClient WebSocket 路径）和 snake_case（进程内调用路径）
    // 对齐 OpenClaw agent.ts: request.sessionKey, request.idempotencyKey, request.inputProvenance
    // ========================================================================

    // helper: 优先取 camelCase，fallback 到 snake_case
    auto get_str = [&](const char* camel, const char* snake) -> std::string {
        if (params.contains(camel) && params[camel].is_string()) {
            return params[camel].get<std::string>();
        }
        if (params.contains(snake) && params[snake].is_string()) {
            return params[snake].get<std::string>();
        }
        return {};
    };

    // ✅ 对齐 OpenClaw: request.sessionKey
    std::string session_key = get_str("sessionKey", "session_key");
    if (session_key.empty()) {
        return core::Result<json>::Err("Missing required parameter: sessionKey");
    }

    std::string message;
    if (params.contains("message") && params["message"].is_string()) {
        message = params["message"].get<std::string>();
    }
    if (message.empty()) {
        return core::Result<json>::Err("Missing required parameter: message");
    }

    // 🎯 OpenClaw 对齐: /new /reset 命令检测 (agent.ts L316-340)
    // RESET_COMMAND_RE = /^\/(new|reset)(?:\s+([\s\S]*))?$/i
    {
        std::string trimmed_msg = message;
        while (!trimmed_msg.empty() && trimmed_msg.front() == ' ') trimmed_msg.erase(0, 1);
        if (trimmed_msg.size() >= 4 && trimmed_msg[0] == '/') {
            std::string cmd_word;
            size_t space_pos = trimmed_msg.find(' ', 1);
            if (space_pos != std::string::npos) {
                cmd_word = trimmed_msg.substr(1, space_pos - 1);
            } else {
                cmd_word = trimmed_msg.substr(1);
            }
            std::transform(cmd_word.begin(), cmd_word.end(), cmd_word.begin(), ::tolower);
            if (cmd_word == "new" || cmd_word == "reset") {
                // Extract post-reset message (if any)
                std::string post_msg;
                if (space_pos != std::string::npos) {
                    post_msg = trimmed_msg.substr(space_pos + 1);
                    while (!post_msg.empty() && post_msg.front() == ' ') post_msg.erase(0, 1);
                    while (!post_msg.empty() && post_msg.back() == ' ') post_msg.pop_back();
                }
                // Perform session reset
                auto agent_pool_reset = server->agent_pool();
                if (agent_pool_reset) {
                    agent_pool_reset->remove_agent(session_key);
                    spdlog::get("default")->info("[method_agent] Session reset via /{}: session={}", cmd_word, session_key);
                }
                if (!post_msg.empty()) {
                    message = post_msg;
                } else {
                    message = "Hello! I'm ready to help. What would you like to work on?";
                }
            }
        }
    }

    // ✅ 对齐 OpenClaw: const idem = request.idempotencyKey (NonEmptyString, required)
    // OC 使用 AJV schema 校验 idempotencyKey 必填且非空。
    // 当缺失时应返回 INVALID_REQUEST 而非静默生成 UUID。
    std::string idem = get_str("idempotencyKey", "idempotency_key");
    if (idem.empty()) {
        idem = get_str("runId", "run_id");
    }
    if (idem.empty()) {
        // ✅ 对齐 OC: 协议要求 idempotencyKey 必填。
        // 向后兼容：生成 UUID 但记录警告（未来版本应硬拒绝）
        idem = core::utils::generate_uuid();
        spdlog::get("default")->warn(
            "[method_agent] Missing idempotencyKey, generated fallback: {} (session={}). "
            "Clients should always provide idempotencyKey for correct retry semantics.",
            idem, session_key);
    }

    bool deliver = false;
    if (params.contains("deliver") && params["deliver"].is_boolean()) {
        deliver = params["deliver"].get<bool>();
    }

    // ✅ 对齐 OpenClaw: request.internalEvents
    // 结构化内部事件，传递给 agentCommandFromIngress
    json internal_events = json::array();
    if (params.contains("internalEvents") && params["internalEvents"].is_array()) {
        internal_events = params["internalEvents"];
    } else if (params.contains("internal_events") && params["internal_events"].is_array()) {
        internal_events = params["internal_events"];
    }

    // ✅ 对齐 OpenClaw: request.inputProvenance
    // 同时支持 camelCase（inputProvenance）和 snake_case（input_provenance）
    std::string input_provenance_kind;
    std::string input_provenance_source_session_key;
    std::string input_provenance_source_tool;
    auto resolve_provenance = [&](const json& prov) {
        if (!prov.is_object()) return;
        // camelCase 优先（对齐 OpenClaw）
        if (prov.contains("kind") && prov["kind"].is_string())
            input_provenance_kind = prov["kind"].get<std::string>();
        // sourceSessionKey (camelCase) or source_session_key (snake_case)
        if (prov.contains("sourceSessionKey") && prov["sourceSessionKey"].is_string())
            input_provenance_source_session_key = prov["sourceSessionKey"].get<std::string>();
        else if (prov.contains("source_session_key") && prov["source_session_key"].is_string())
            input_provenance_source_session_key = prov["source_session_key"].get<std::string>();
        // sourceTool (camelCase) or source_tool (snake_case)
        if (prov.contains("sourceTool") && prov["sourceTool"].is_string())
            input_provenance_source_tool = prov["sourceTool"].get<std::string>();
        else if (prov.contains("source_tool") && prov["source_tool"].is_string())
            input_provenance_source_tool = prov["source_tool"].get<std::string>();
    };
    if (params.contains("inputProvenance"))
        resolve_provenance(params["inputProvenance"]);
    else if (params.contains("input_provenance"))
        resolve_provenance(params["input_provenance"]);
    // 也支持扁平字段（来自 inject_options 格式）
    if (input_provenance_source_session_key.empty())
        input_provenance_source_session_key = get_str("sourceSessionKey", "source_session_key");
    if (input_provenance_source_tool.empty())
        input_provenance_source_tool = get_str("sourceTool", "source_tool");

    // ========================================================================
    // Step 2: 幂等去重 (对齐 OpenClaw: context.dedupe.get("agent:{idem}"))
    // ========================================================================
    auto state = server->runtime_state();
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto dedupe_key = "agent:" + idem;
        auto it = state->dedupe.find(dedupe_key);
        if (it != state->dedupe.end()) {
            // 已处理过，返回幂等结果
            spdlog::get("default")->debug(
                "[method_agent] Dedupe hit: idem={}, session={}",
                idem, session_key
            );
            json cached_result;
            cached_result["run_id"] = idem;
            cached_result["status"] = "accepted";
            cached_result["cached"] = true;
            return core::Result<json>::Ok(cached_result);
        }
        // 记录 dedupe entry（in-flight ack，防止重试产生第二个 run）
        state->dedupe[dedupe_key] = DedupeEntry{
            .ts = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            ),
            .method = "agent"
        };
    }

    // ========================================================================
    // Step 3: 获取 Agent 实例
    // ========================================================================
    auto agent_pool = server->agent_pool();
    if (!agent_pool) {
        return core::Result<json>::Err("AgentPool not available");
    }

    auto agent = agent_pool->get_agent(session_key);
    if (!agent) {
        // ── ACP Session Fast Path ─────────────────────────────────────────────
        // Aligned with openclaw: dispatchAgentRunFromGateway → dispatch_reply_from_config
        //   → Step 6: tryDispatchAcpReply → acpManager.runTurn
        //
        // When the remote Gateway receives an "agent" request from an ACP subagent,
        // the session_key is of the form "agent:{target}:acp:{uuid}".
        // There is NO AgentInstance in the local agent_pool for this session —
        // the session lives in AcpSessionManager (initialized by acp_spawn.cc on the
        // calling side, or by initialize_acp_spawn_runtime on the receiving side).
        //
        // Correct path: resolve_session → run_turn (bypasses ReactEngine entirely,
        // because inference is handled by the remote ACP backend, e.g. Cursor/Zed).
        try {
            auto resolution = acp::get_acp_session_manager().resolve_session(session_key);
            if (resolution.kind != acp::AcpSessionResolution::Kind::None) {
                // This IS an ACP session (Ready or Stale) — handle it directly.
                spdlog::get("default")->info(
                    "[method_agent] ACP session fast path: session={} kind={}",
                    session_key,
                    resolution.kind == acp::AcpSessionResolution::Kind::Ready ? "ready" : "stale"
                );

                if (resolution.kind == acp::AcpSessionResolution::Kind::Stale) {
                    // Stale: deliver error and return accepted (fire-and-forget semantics preserved)
                    const auto& stale_err = resolution.error.value_or(
                        acp::AcpRuntimeError(acp::AcpErrorCode::ACP_SESSION_INIT_FAILED,
                                             "ACP session is stale"));
                    spdlog::get("default")->warn(
                        "[method_agent] Stale ACP session: session={} error={}",
                        session_key, stale_err.what());
                    // Return accepted — the error is surfaced via the ACP event stream,
                    // not as a gateway-level failure (aligned with openclaw fire-and-forget).
                    json accepted;
                    accepted["run_id"] = idem;
                    accepted["status"] = "accepted";
                    accepted["acp_stale"] = true;
                    return core::Result<json>::Ok(accepted);
                }

                // Ready: fire run_turn in background thread (aligned with openclaw async dispatch)
                const std::string run_id = idem;
                const std::string captured_session_key = session_key;
                const std::string captured_message = message;
                const std::string captured_run_id = run_id;
                const std::string captured_deliver_channel = get_str("deliverChannel", "deliver_channel");
                const std::string captured_deliver_to = get_str("deliverTo", "deliver_to");
                const std::string captured_deliver_account_id = get_str("deliverAccountId", "deliver_account_id");
                const std::string captured_deliver_thread_id = get_str("deliverThreadId", "deliver_thread_id");
                const std::string captured_deliver_reply_to_id = get_str("deliverReplyToId", "deliver_reply_to_id");
                auto captured_delivery_queue = server->delivery_queue();

                // Build deliver_to_channel (same pattern as try_dispatch_acp_reply)
                auto deliver_to_channel = [captured_delivery_queue,
                                           captured_deliver_channel,
                                           captured_deliver_to,
                                           captured_deliver_account_id,
                                           captured_deliver_thread_id,
                                           captured_deliver_reply_to_id](const std::string& text) {
                    if (!captured_delivery_queue || text.empty() ||
                        captured_deliver_channel.empty() || captured_deliver_to.empty()) return;
                    ::hunterclaw::queue::MessagePayload payload;
                    payload.text = text;
                    ::hunterclaw::outbound::DeliverOutboundPayloadsParams p;
                    p.channel      = captured_deliver_channel;
                    p.to           = captured_deliver_to;
                    p.payloads     = {payload};
                    p.delivery_queue = captured_delivery_queue;
                    p.skip_queue   = false;
                    p.best_effort  = true;
                    p.silent       = false;
                    if (!captured_deliver_account_id.empty()) p.account_id = captured_deliver_account_id;
                    if (!captured_deliver_thread_id.empty())  p.thread_id  = captured_deliver_thread_id;
                    if (!captured_deliver_reply_to_id.empty()) p.reply_to_id = captured_deliver_reply_to_id;
                    (void)::hunterclaw::outbound::deliver_outbound_payloads(p);
                };

                std::thread([captured_session_key, captured_message, captured_run_id,
                             deliver_to_channel]() mutable {
                    const int64_t started_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    // Build projector (aligned with try_dispatch_acp_reply)
                    acp::AcpReplyProjector::Config proj_cfg;
                    proj_cfg.delivery_mode = acp::AcpDeliveryMode::Block;
                    proj_cfg.should_send_tool_summaries = true;
                    auto projector = std::make_shared<acp::AcpReplyProjector>(
                        proj_cfg,
                        [deliver_to_channel](const std::string&, const std::string& text,
                                             const std::optional<acp::AcpProjectedDeliveryMeta>&) {
                            deliver_to_channel(text);
                        }
                    );

                    try {
                        acp::AcpRunTurnInput turn_input;
                        turn_input.session_key = captured_session_key;
                        turn_input.text        = captured_message;
                        turn_input.mode        = acp::AcpRuntimePromptMode::Prompt;
                        turn_input.request_id  = captured_session_key + ":" + std::to_string(started_at);
                        // Aligned with openclaw: dispatch-acp.ts:935-951
                        //   onEvent → emitAcpRuntimeEvent(...) 然后 projector.onEvent(event)
                        // 两件事都要做：桥接到 AgentEvent 总线（供 ParentStreamRelay 感知
                        // 子会话进度）+ 投递到 channel（projector）。
                        turn_input.on_event =
                            [projector, captured_run_id](const acp::AcpRuntimeEvent& ev) {
                                emit_acp_runtime_event(captured_run_id, ev);
                                projector->on_event(ev);
                            };

                        // Aligned with openclaw: emitAcpPromptSubmitted before runtime turn
                        emit_acp_prompt_submitted(captured_run_id, started_at);

                        acp::get_acp_session_manager().run_turn(turn_input);
                        projector->flush(true);

                        const int64_t latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count() - started_at;
                        spdlog::get("default")->info(
                            "[method_agent] ACP turn completed: session={} latencyMs={}",
                            captured_session_key, latency_ms);
                    } catch (const acp::AcpRuntimeError& e) {
                        projector->flush(true);
                        spdlog::get("default")->error(
                            "[method_agent] ACP turn failed: session={} code={} error={}",
                            captured_session_key, e.code_string(), e.what());
                        deliver_to_channel(acp::format_acp_runtime_error_text(e));
                    } catch (const std::exception& e) {
                        projector->flush(true);
                        spdlog::get("default")->error(
                            "[method_agent] ACP turn exception: session={} error={}",
                            captured_session_key, e.what());
                        acp::AcpRuntimeError acp_err(acp::AcpErrorCode::ACP_TURN_FAILED, e.what());
                        deliver_to_channel(acp::format_acp_runtime_error_text(acp_err));
                    }
                }).detach();

                json accepted;
                accepted["run_id"] = run_id;
                accepted["status"] = "accepted";
                accepted["accepted_at"] = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                return core::Result<json>::Ok(accepted);
            }
        } catch (...) {
            // AcpSessionManager not initialized — fall through to normal error
        }

        return core::Result<json>::Err("Agent not found for session: " + session_key);
    }

    // ✅ 对齐 OpenClaw: const runId = idem
    // Gateway agent.ts: const runId = idem;
    // agent-runner-execution.ts: const runId = params.opts?.runId ?? crypto.randomUUID();
    const std::string run_id = idem;

    // ✅ 对齐 OC server-methods/agent.ts: createRunningTaskRun({ runtime: "acp", ... })
    // Create a background task record so the agent run is visible in /tasks and task audit.
    auto task_executor = server->task_executor();
    if (task_executor) {
        try {
            tasks::TaskRecord task_params;
            task_params.runtime = tasks::TaskRuntime::ACP;
            task_params.owner_key = session_key;
            task_params.scope_kind = tasks::TaskScopeKind::SESSION;
            task_params.run_id = run_id;
            task_params.task = message.size() > 120 ? message.substr(0, 120) : message;
            task_params.started_at = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );
            task_params.last_event_at = task_params.started_at;
            task_executor->create_running_task_run(task_params);
        } catch (const std::exception& e) {
            spdlog::get("default")->warn("[method_agent] Failed to create task record: {}", e.what());
        }
    }

    spdlog::get("default")->info(
        "[method_agent] Dispatching: session={}, run_id={}, deliver={}, source={}",
        session_key, run_id, deliver,
        input_provenance_source_session_key.empty() ? "user" : input_provenance_source_session_key
    );

    // ========================================================================
    // Step 4: 立即返回 accepted（对齐 OpenClaw: respond(accepted)）
    // ========================================================================
    json accepted;
    accepted["run_id"] = run_id;
    accepted["status"] = "accepted";
    accepted["accepted_at"] = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    // ========================================================================
    // Step 5: 异步 dispatch（对齐 OpenClaw: dispatchAgentRunFromGateway）
    // ========================================================================
    // 构建 process_message options，传递 run_id、deliver 标志和 internalEvents
    //
    // ✅ 对齐 OpenClaw agent-step.ts:103-117 —— callGateway("agent", {...}) 还会带
    //    extraSystemPrompt / transcriptMessage / sourceReplyDeliveryMode /
    //    channel / lane 五个字段。此前这五个**全部被静默丢弃**，后果最严重的是
    //    extraSystemPrompt：A2A 的 reply/announce 上下文（含 "reply exactly
    //    REPLY_SKIP" 这类控制约定）从未送达模型，整套 A2A 控制流形同虚设。
    std::string extra_system_prompt = get_str("extraSystemPrompt", "extra_system_prompt");
    std::string source_reply_delivery_mode =
        get_str("sourceReplyDeliveryMode", "source_reply_delivery_mode");
    std::string req_channel = get_str("channel", "channel");
    std::string req_lane = get_str("lane", "lane");

    std::map<std::string, std::string> inject_options = {
        {"run_id", run_id},
        {"deliver", deliver ? "true" : "false"},
        {"session_key", session_key},
        {"input_provenance_kind", input_provenance_kind.empty() ? "inter_session" : input_provenance_kind},
        {"input_provenance_source_session_key", input_provenance_source_session_key},
        {"input_provenance_source_tool", input_provenance_source_tool.empty() ? "subagent_announce" : input_provenance_source_tool},
        // ✅ 对齐 OpenClaw: internalEvents 序列化传递
        {"internal_events", internal_events.empty() ? "[]" : internal_events.dump()},
    };
    // 仅在确实传入时才写入，避免用空串覆盖下游默认值
    if (!extra_system_prompt.empty()) {
        inject_options["extra_system_prompt"] = extra_system_prompt;
    }
    if (!source_reply_delivery_mode.empty()) {
        // 対齐 agent-step.ts:89,110 —— "message_tool_only" 表示本次 run 的回复
        // 不得自动外发，只能由 message tool 显式投递。A2A 的内部步骤全靠它，
        // 否则 ping-pong 的每一轮中间回复都会漏给终端用户。
        inject_options["source_reply_delivery_mode"] = source_reply_delivery_mode;
    }
    if (!req_channel.empty()) {
        inject_options["channel"] = req_channel;
    }
    if (!req_lane.empty()) {
        // 対齐 agent-step.ts:80 —— per-session 泳道，串行化同一会话的嵌套工作
        inject_options["lane"] = req_lane;
    }
    // 対齐 agent-step.ts:82-86 —— transcriptMessage 有值（**含空串**）时透传。
    // 空串是有意义的取值（announce 步骤就传 ""），故须用 contains 判定而非非空。
    if (params.contains("transcriptMessage") && params["transcriptMessage"].is_string()) {
        inject_options["transcript_message"] = params["transcriptMessage"].get<std::string>();
        inject_options["has_transcript_message"] = "true";
    } else if (params.contains("transcript_message") &&
               params["transcript_message"].is_string()) {
        inject_options["transcript_message"] = params["transcript_message"].get<std::string>();
        inject_options["has_transcript_message"] = "true";
    }

    // ✅ 对齐 OpenClaw: dispatchAgentRunFromGateway (fire-and-forget)
    // 通过 MessageDispatcher 调度（支持 queue mode），或直接调用 process_message
    auto msg_dispatcher = server->message_dispatcher();
    auto delivery_queue = server->delivery_queue();

    // ✅ 对齐 OpenClaw: deliver=true 时，从 params 中读取 channel 信息
    // 用于构建 deliver 函数（channel-specific 发送函数）
    // 对应 OpenClaw: delivery.deliver("block", payload) → dispatcher.sendBlockReply → options.deliver
    std::string deliver_channel = get_str("deliverChannel", "deliver_channel");
    std::string deliver_to      = get_str("deliverTo", "deliver_to");
    std::string deliver_account_id = get_str("deliverAccountId", "deliver_account_id");
    std::string deliver_thread_id  = get_str("deliverThreadId", "deliver_thread_id");
    std::string deliver_reply_to_id = get_str("deliverReplyToId", "deliver_reply_to_id");

    if (msg_dispatcher) {
        // 通过 MessageDispatcher 调度（对齐 OpenClaw: runReplyAgent）
        auto captured_agent = agent;
        auto captured_message = message;
        auto captured_options = inject_options;
        auto captured_session_key = session_key;
        auto captured_run_id = run_id;
        auto captured_deliver = deliver;
        auto captured_delivery_queue = delivery_queue;
        auto captured_deliver_channel = deliver_channel;
        auto captured_deliver_to = deliver_to;
        auto captured_deliver_account_id = deliver_account_id;
        auto captured_deliver_thread_id = deliver_thread_id;
        auto captured_deliver_reply_to_id = deliver_reply_to_id;

        auto run_fn = [captured_agent, captured_message, captured_options,
                       captured_session_key, captured_run_id,
                       captured_deliver, captured_delivery_queue,
                       captured_deliver_channel, captured_deliver_to,
                       captured_deliver_account_id, captured_deliver_thread_id,
                       captured_deliver_reply_to_id]()
            -> auto_reply::MessageDispatcher::DispatchResult {
            auto_reply::MessageDispatcher::DispatchResult result;
            try {
                // ✅ 对齐 OpenClaw: deliver=true 时，注入 deliver 函数到 agent
                // 对应 OpenClaw: delivery.deliver("block", payload) → dispatcher.sendBlockReply → options.deliver → channel.send()
                // HunterClaw: deliver_fn_ → deliver_outbound_payloads(channel, to, payloads, delivery_queue)
                //
                // ✅ Fallback: 如果 params 中没有 channel 信息，从 agent->context() 中读取
                // 对应 OpenClaw: SessionEntry.lastChannel / lastTo（由 dispatch_reply_from_config 更新）
                std::string effective_channel = captured_deliver_channel;
                std::string effective_to      = captured_deliver_to;
                std::string effective_account_id = captured_deliver_account_id;
                std::string effective_thread_id  = captured_deliver_thread_id;
                std::string effective_reply_to_id = captured_deliver_reply_to_id;
                if (effective_channel.empty() || effective_to.empty()) {
                    const auto& ctx = captured_agent->context();
                    if (!ctx.last_channel.empty()) effective_channel   = ctx.last_channel;
                    if (!ctx.last_to.empty())      effective_to         = ctx.last_to;
                    if (!ctx.last_account_id.empty()) effective_account_id = ctx.last_account_id;
                    if (!ctx.last_thread_id.empty())  effective_thread_id  = ctx.last_thread_id;
                    if (!ctx.last_reply_to_id.empty()) effective_reply_to_id = ctx.last_reply_to_id;
                }

                if (captured_deliver && captured_delivery_queue &&
                    !effective_channel.empty() && !effective_to.empty()) {
                    auto deliver_fn = [captured_delivery_queue,
                                       effective_channel,
                                       effective_to,
                                       effective_account_id,
                                       effective_thread_id,
                                       effective_reply_to_id](const std::string& text) {
                        ::hunterclaw::queue::MessagePayload payload;
                        payload.text = text;

                        ::hunterclaw::outbound::DeliverOutboundPayloadsParams params;
                        params.channel      = effective_channel;
                        params.to           = effective_to;
                        params.payloads     = {payload};
                        params.delivery_queue = captured_delivery_queue;
                        params.skip_queue   = false;
                        params.best_effort  = true;
                        params.silent       = false;
                        if (!effective_account_id.empty()) {
                            params.account_id = effective_account_id;
                        }
                        if (!effective_thread_id.empty()) {
                            params.thread_id = effective_thread_id;
                        }
                        if (!effective_reply_to_id.empty()) {
                            params.reply_to_id = effective_reply_to_id;
                        }

                        auto deliver_result = ::hunterclaw::outbound::deliver_outbound_payloads(params);
                        if (!deliver_result.is_ok()) {
                            spdlog::get("default")->warn(
                                "[method_agent] deliver_fn failed: channel={} to={} error={}",
                                effective_channel, effective_to, deliver_result.error
                            );
                        }
                    };
                    captured_agent->set_deliver_fn(std::move(deliver_fn));
                    spdlog::get("default")->debug(
                        "[method_agent] Injected deliver_fn: channel={} to={}",
                        effective_channel, effective_to
                    );
                }

                auto res = captured_agent->process_message(captured_message, captured_options);
                result.success = !res.is_err();
                if (res.is_err()) {
                    result.error = res.error;
                }

                // ✅ 清理 deliver_fn（避免悬空引用）
                captured_agent->set_deliver_fn(nullptr);
            } catch (const std::exception& e) {
                result.success = false;
                result.error = e.what();
                captured_agent->set_deliver_fn(nullptr);
            }
            return result;
        };

        auto_reply::MessageDispatcher::RunReplyAgentParams agent_params;
        agent_params.session_key  = session_key;
        agent_params.run_fn       = run_fn;
        agent_params.label        = "agent:" + session_key + ":" + run_id;
        agent_params.prompt       = message;
        agent_params.is_heartbeat = false;
        agent_params.in_session_lane = true;  // ✅ 调用方已在 session lane 中

        auto& cmd_queue = queue::CommandQueue::Instance();
        const std::string session_lane = queue::ResolveSessionLane(session_key);

        std::thread([msg_dispatcher, agent_params, &cmd_queue, session_lane,
                     server, idem_copy = idem, run_id_copy = run_id]() mutable {
            try {
                std::function<void()> task = [msg_dispatcher, agent_params]() mutable {
                    msg_dispatcher->run_reply_agent(agent_params);
                };
                auto future = cmd_queue.enqueue_command_in_lane(
                    session_lane,
                    std::move(task),
                    queue::EnqueueOptions{.label = agent_params.label, .urgent = false}
                );
                future.get();

                // ✅ 对齐 OC dispatchAgentRunFromGateway .then(): 更新 dedupe 为终态
                auto state = server->runtime_state();
                if (state) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    auto dedupe_key = "agent:" + idem_copy;
                    json ok_payload;
                    ok_payload["runId"] = run_id_copy;
                    ok_payload["status"] = "ok";
                    ok_payload["summary"] = "completed";
                    state->dedupe[dedupe_key] = DedupeEntry{
                        .ts = static_cast<int64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                            ).count()
                        ),
                        .method = "agent",
                        .ok = true,
                        .payload = ok_payload,
                    };
                }
            } catch (const std::exception& e) {
                spdlog::get("default")->error(
                    "[method_agent] Session lane error: session={}, error={}",
                    agent_params.session_key, e.what()
                );
                // ✅ 对齐 OC dispatchAgentRunFromGateway .catch(): 更新 dedupe 为 error
                auto state = server->runtime_state();
                if (state) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    auto dedupe_key = "agent:" + idem_copy;
                    json err_payload;
                    err_payload["runId"] = run_id_copy;
                    err_payload["status"] = "error";
                    err_payload["summary"] = std::string(e.what());
                    state->dedupe[dedupe_key] = DedupeEntry{
                        .ts = static_cast<int64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                            ).count()
                        ),
                        .method = "agent",
                        .ok = false,
                        .payload = err_payload,
                    };
                }
            }
        }).detach();
    } else {
        // Fallback: 直接调用 process_message（MessageDispatcher 未初始化时）
        auto captured_agent = agent;
        auto captured_message = message;
        auto captured_options = inject_options;
        auto captured_server = server;
        auto captured_idem = idem;
        auto captured_run_id = run_id;
        std::thread([captured_agent, captured_message, captured_options,
                     captured_server, captured_idem, captured_run_id]() {
            try {
                captured_agent->process_message(captured_message, captured_options);
                // ✅ 对齐 OC: 更新 dedupe 为终态
                auto state = captured_server->runtime_state();
                if (state) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    auto dedupe_key = "agent:" + captured_idem;
                    json ok_payload;
                    ok_payload["runId"] = captured_run_id;
                    ok_payload["status"] = "ok";
                    ok_payload["summary"] = "completed";
                    state->dedupe[dedupe_key] = DedupeEntry{
                        .ts = static_cast<int64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                            ).count()
                        ),
                        .method = "agent",
                        .ok = true,
                        .payload = ok_payload,
                    };
                }
            } catch (const std::exception& e) {
                auto state = captured_server->runtime_state();
                if (state) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    auto dedupe_key = "agent:" + captured_idem;
                    json err_payload;
                    err_payload["runId"] = captured_run_id;
                    err_payload["status"] = "error";
                    err_payload["summary"] = std::string(e.what());
                    state->dedupe[dedupe_key] = DedupeEntry{
                        .ts = static_cast<int64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                            ).count()
                        ),
                        .method = "agent",
                        .ok = false,
                        .payload = err_payload,
                    };
                }
            }
        }).detach();
    }

    return core::Result<json>::Ok(accepted);
}

// ============================================================================
// agent.cancel - Cancel a running agent (NEW)
// ============================================================================
core::Result<json> method_agent_cancel(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("run_id")) {
        return core::Result<json>::Err("Missing required parameter: run_id");
    }

    std::string run_id = params["run_id"];

    // Get abort controller from state
    auto state = server->runtime_state();
    auto it = state->abort_controllers.find(run_id);

    if (it == state->abort_controllers.end()) {
        return core::Result<json>::Err("Run not found or already completed: " + run_id);
    }

    // Signal abort
    it->second->store(true);

    // Broadcast agent.cancelled event
    core::EventFrame cancelled_event;
    cancelled_event.event = "agent.cancelled";
    cancelled_event.payload = json::object();
    cancelled_event.payload["run_id"] = run_id;
    cancelled_event.payload["timestamp"] = core::utils::now().time_since_epoch().count();
    server->broadcast(cancelled_event);

    json result;
    result["run_id"] = run_id;
    result["status"] = "cancelled";
    result["cancelled_at"] = core::utils::now().time_since_epoch().count();

    return core::Result<json>::Ok(result);
}

// ============================================================================
// agent.status - Get agent run status (NEW)
// ============================================================================
core::Result<json> method_agent_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("run_id")) {
        return core::Result<json>::Err("Missing required parameter: run_id");
    }

    std::string run_id = params["run_id"];

    // Check if run exists
    auto state = server->runtime_state();
    auto it = state->abort_controllers.find(run_id);

    json result;
    result["run_id"] = run_id;

    if (it == state->abort_controllers.end()) {
        // Run completed or never existed
        result["status"] = "completed";
        result["exists"] = false;
    } else {
        // Run is active
        bool is_aborted = it->second->load();
        result["status"] = is_aborted ? "cancelling" : "running";
        result["exists"] = true;
    }

    return core::Result<json>::Ok(result);
}

// ============================================================================
// agent.wait — 等待 agent run 完成（对齐 OC agentHandlers["agent.wait"]）
//
// OC 实现要点:
//   1. validateAgentWaitParams (runId: NonEmptyString, timeoutMs?: number)
//   2. readTerminalSnapshotFromGatewayDedupe — 如果 dedupe 已有终态直接返回
//   3. dual-race: lifecyclePromise vs dedupePromise (先到先赢)
//   4. 超时返回 { runId, status: "timeout" }
//
// HC 简化实现: 轮询 dedupe entry（2s 间隔）直到有终态或超时
// ============================================================================
core::Result<json> method_agent_wait(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    // Step 1: 参数校验
    std::string run_id;
    if (params.contains("runId") && params["runId"].is_string()) {
        run_id = params["runId"].get<std::string>();
    }
    if (run_id.empty()) {
        return core::Result<json>::Err("invalid agent.wait params: runId required");
    }

    int64_t timeout_ms = 30000;  // 默认 30s
    if (params.contains("timeoutMs") && params["timeoutMs"].is_number()) {
        timeout_ms = std::max<int64_t>(0, params["timeoutMs"].get<int64_t>());
    }

    auto state = server->runtime_state();
    if (!state) {
        return core::Result<json>::Err("runtime state not available");
    }

    // Step 2: 立即检查 dedupe 终态
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto dedupe_key = "agent:" + run_id;
        auto it = state->dedupe.find(dedupe_key);
        if (it != state->dedupe.end() && it->second.ok.has_value()) {
            // ✅ 已有终态：立即返回（对齐 OC readTerminalSnapshotFromGatewayDedupe）
            json result;
            result["runId"] = run_id;
            result["status"] = it->second.ok.value() ? "ok" : "error";
            if (it->second.payload.has_value()) {
                result["startedAt"] = it->second.payload->value("startedAt", 0);
                result["endedAt"] = it->second.ts;
                if (it->second.payload->contains("summary")) {
                    result["error"] = (*it->second.payload)["summary"];
                }
            }
            return core::Result<json>::Ok(result);
        }
    }

    // Step 3: 轮询等待（对齐 OC dual-race 简化版）
    int64_t started_at = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    constexpr int64_t POLL_INTERVAL_MS = 500;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));

        int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );

        // 检查超时
        if (now - started_at > timeout_ms) {
            json result;
            result["runId"] = run_id;
            result["status"] = "timeout";
            return core::Result<json>::Ok(result);
        }

        // 检查 dedupe 终态
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto dedupe_key = "agent:" + run_id;
            auto it = state->dedupe.find(dedupe_key);
            if (it != state->dedupe.end() && it->second.ok.has_value()) {
                json result;
                result["runId"] = run_id;
                result["status"] = it->second.ok.value() ? "ok" : "error";
                if (it->second.payload.has_value()) {
                    result["endedAt"] = it->second.ts;
                    if (!it->second.ok.value() && it->second.payload->contains("summary")) {
                        result["error"] = (*it->second.payload)["summary"];
                    }
                }
                return core::Result<json>::Ok(result);
            }
        }
    }
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
