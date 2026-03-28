#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/sessions/transcript_io.h"
#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/queue/command_queue.h"
#include "hunterclaw/queue/lanes.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <filesystem>

using json = nlohmann::json;

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
    // ========================================================================
    std::string session_key;
    if (params.contains("session_key") && params["session_key"].is_string()) {
        session_key = params["session_key"].get<std::string>();
    }
    if (session_key.empty()) {
        return core::Result<json>::Err("Missing required parameter: session_key");
    }

    std::string message;
    if (params.contains("message") && params["message"].is_string()) {
        message = params["message"].get<std::string>();
    }
    if (message.empty()) {
        return core::Result<json>::Err("Missing required parameter: message");
    }

    // ✅ 对齐 OpenClaw: const idem = request.idempotencyKey
    std::string idem;
    if (params.contains("idempotency_key") && params["idempotency_key"].is_string()) {
        idem = params["idempotency_key"].get<std::string>();
    }
    if (idem.empty()) {
        // fallback: 使用 run_id 字段
        if (params.contains("run_id") && params["run_id"].is_string()) {
            idem = params["run_id"].get<std::string>();
        }
    }
    if (idem.empty()) {
        idem = core::utils::generate_uuid();
    }

    bool deliver = false;
    if (params.contains("deliver") && params["deliver"].is_boolean()) {
        deliver = params["deliver"].get<bool>();
    }

    // input_provenance 字段（对齐 OpenClaw: inputProvenance）
    std::string input_provenance_kind;
    std::string input_provenance_source_session_key;
    std::string input_provenance_source_tool;
    if (params.contains("input_provenance") && params["input_provenance"].is_object()) {
        const auto& prov = params["input_provenance"];
        if (prov.contains("kind") && prov["kind"].is_string()) {
            input_provenance_kind = prov["kind"].get<std::string>();
        }
        if (prov.contains("source_session_key") && prov["source_session_key"].is_string()) {
            input_provenance_source_session_key = prov["source_session_key"].get<std::string>();
        }
        if (prov.contains("source_tool") && prov["source_tool"].is_string()) {
            input_provenance_source_tool = prov["source_tool"].get<std::string>();
        }
    }
    // 也支持扁平字段（来自 attempt_delivery 的 inject_options 格式）
    if (input_provenance_source_session_key.empty() && params.contains("source_session_key")) {
        input_provenance_source_session_key = params["source_session_key"].get<std::string>();
    }
    if (input_provenance_source_tool.empty() && params.contains("source_tool")) {
        input_provenance_source_tool = params["source_tool"].get<std::string>();
    }

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
        return core::Result<json>::Err("Agent not found for session: " + session_key);
    }

    // ✅ 对齐 OpenClaw: const runId = idem
    // Gateway agent.ts: const runId = idem;
    // agent-runner-execution.ts: const runId = params.opts?.runId ?? crypto.randomUUID();
    const std::string run_id = idem;

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
    // 构建 process_message options，传递 run_id 和 deliver 标志
    std::map<std::string, std::string> inject_options = {
        {"run_id", run_id},
        {"deliver", deliver ? "true" : "false"},
        {"session_key", session_key},
        {"input_provenance_kind", input_provenance_kind.empty() ? "inter_session" : input_provenance_kind},
        {"input_provenance_source_session_key", input_provenance_source_session_key},
        {"input_provenance_source_tool", input_provenance_source_tool.empty() ? "subagent_announce" : input_provenance_source_tool},
    };

    // ✅ 对齐 OpenClaw: dispatchAgentRunFromGateway (fire-and-forget)
    // 通过 MessageDispatcher 调度（支持 queue mode），或直接调用 process_message
    auto msg_dispatcher = server->message_dispatcher();
    if (msg_dispatcher) {
        // 通过 MessageDispatcher 调度（对齐 OpenClaw: runReplyAgent）
        auto captured_agent = agent;
        auto captured_message = message;
        auto captured_options = inject_options;
        auto captured_session_key = session_key;
        auto captured_run_id = run_id;

        auto run_fn = [captured_agent, captured_message, captured_options,
                       captured_session_key, captured_run_id]()
            -> auto_reply::MessageDispatcher::DispatchResult {
            auto_reply::MessageDispatcher::DispatchResult result;
            try {
                auto res = captured_agent->process_message(captured_message, captured_options);
                result.success = !res.is_err();
                if (res.is_err()) {
                    result.error = res.error;
                }
            } catch (const std::exception& e) {
                result.success = false;
                result.error = e.what();
            }
            return result;
        };

        auto_reply::MessageDispatcher::RunReplyAgentParams agent_params;
        agent_params.session_key  = session_key;
        agent_params.run_fn       = run_fn;
        agent_params.label        = "agent:" + session_key + ":" + run_id;
        agent_params.prompt       = message;
        agent_params.is_heartbeat = false;

        auto& cmd_queue = queue::CommandQueue::Instance();
        const std::string session_lane = queue::ResolveSessionLane(session_key);

        std::thread([msg_dispatcher, agent_params, &cmd_queue, session_lane]() mutable {
            try {
                auto future = cmd_queue.enqueue_command_in_lane(
                    session_lane,
                    [msg_dispatcher, agent_params]() mutable {
                        msg_dispatcher->run_reply_agent(agent_params);
                    },
                    queue::EnqueueOptions{.label = agent_params.label, .urgent = false}
                );
                future.get();
            } catch (const std::exception& e) {
                spdlog::get("default")->error(
                    "[method_agent] Session lane error: session={}, error={}",
                    agent_params.session_key, e.what()
                );
            }
        }).detach();
    } else {
        // Fallback: 直接调用 process_message（MessageDispatcher 未初始化时）
        auto captured_agent = agent;
        auto captured_message = message;
        auto captured_options = inject_options;
        std::thread([captured_agent, captured_message, captured_options]() {
            captured_agent->process_message(captured_message, captured_options);
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

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
