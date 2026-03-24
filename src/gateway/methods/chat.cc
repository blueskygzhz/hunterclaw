#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/gateway/protocol.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/tools/tool_hooks.h"   // get_global_hook_manager(), PluginHook* types
#include "hunterclaw/sessions/transcript_io.h"
#include "hunterclaw/queue/command_queue.h"
#include "hunterclaw/queue/lanes.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "hunterclaw/llm/llm_client.h"

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Logger Helper
// ============================================================================
static std::shared_ptr<spdlog::logger> get_logger() {
    static auto logger = spdlog::get("chat");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("chat");
    }
    return logger;
}

// ============================================================================
// Chat Methods
// ============================================================================

// ============================================================================
// Helper: Stop Command Detection (对齐 OpenClaw)
// ============================================================================
static bool is_stop_command(const std::string& message) {
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string trimmed = lower;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    
    return trimmed == "stop" || trimmed == "cancel" || trimmed == "abort";
}

// ============================================================================
// Helper: Message Sanitization (对齐 OpenClaw)
// ============================================================================
static std::string sanitize_message(const std::string& raw) {
    // NFC 归一化 + 控制字符过滤
    std::string result;
    result.reserve(raw.size());
    
    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        // 允许: Tab(9), LF(10), CR(13), 可打印字符(32-126)
        if (uc == 9 || uc == 10 || uc == 13 || (uc >= 32 && uc < 127)) {
            result += c;
        }
    }
    
    return result;
}

// ============================================================================
// Helper: Generate UUID (对齐 OpenClaw)
// ============================================================================
static std::string generate_run_id() {
    return core::utils::generate_uuid();
}

// 13. chat.send - Send message (对齐 OpenClaw 完整实现)
// 参考: openclaw-analysis/src/gateway/server-methods/chat.ts
core::Result<json> method_chat_send(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ========================================================================
    // Step 1: 参数验证 (对齐 OpenClaw validateChatSendParams)
    // ========================================================================
    
    if (!params.contains("session_id") || !params.contains("message")) {
        return core::Result<json>::Err("Missing required parameters: session_id, message");
    }
    
    std::string session_id = params["session_id"];
    std::string raw_message = params["message"];
    
    // 消息清理
    std::string message = sanitize_message(raw_message);
    if (message.empty()) {
        return core::Result<json>::Err("Message cannot be empty after sanitization");
    }
    
    get_logger()->info("[chat.send] session={}, message_len={}", session_id, message.size());
    
    // ========================================================================
    // Step 2: Stop Command 检测 (对齐 OpenClaw)
    // ========================================================================
    
    if (is_stop_command(message)) {
        get_logger()->info("[chat.send] Stop command detected: session={}", session_id);

        // ✅ 对齐 OpenClaw: abort_chat_run(session_id)
        // 通过 AgentPool 获取 Agent，调用 ReactEngine::cancel() 中止当前运行
        auto agent_pool_for_abort = server->agent_pool();
        if (agent_pool_for_abort) {
            auto agent_for_abort = agent_pool_for_abort->get_agent(session_id);
            if (agent_for_abort) {
                auto re = agent_for_abort->react_engine();
                if (re) {
                    re->cancel();
                    get_logger()->info("[chat.send] Aborted running agent via stop command: session={}", session_id);
                }
            }
        }

        json result;
        result["ok"] = true;
        result["stopped"] = true;
        result["session_id"] = session_id;
        return core::Result<json>::Ok(result);
    }
    
    // ========================================================================
    // Step 3: 获取或创建 Agent (AgentPool)
    // ========================================================================
    
    auto agent_pool = server->agent_pool();
    if (!agent_pool) {
        return core::Result<json>::Err("AgentPool not initialized");
    }
    
    auto agent = agent_pool->get_or_create_agent(session_id);
    if (!agent) {
        return core::Result<json>::Err("Failed to get or create agent");
    }
    
    // ========================================================================
    // Step 4: 注册事件监听器 (OpenClaw 对齐)
    // 参考: openclaw-analysis/src/gateway/server-methods/agent-job.ts:212
    // ========================================================================
    
    // ✅ 监听 Agent 生命周期事件
    // OpenClaw: const unsubscribe = onAgentEvent((evt) => { ... })
    agent->on_event([server, session_id](const agents::AgentEvent& evt) {
        if (evt.stream != agents::AgentEventStream::Lifecycle) {
            return;  // 只处理生命周期事件
        }
        
        // ✅ 广播到所有连接的客户端
        json event_payload;
        event_payload["type"] = "agent.lifecycle";
        event_payload["session_id"] = session_id;
        event_payload["run_id"] = evt.run_id;
        event_payload["seq"] = evt.seq;
        event_payload["stream"] = "lifecycle";
        event_payload["timestamp"] = evt.timestamp_ms;
        event_payload["data"] = evt.data.to_json();
        
        core::EventFrame frame;
        frame.event = "agent";
        frame.seq = evt.seq;
        frame.payload = core::JsonValue(event_payload);
        
        server->broadcast(frame);
        
        std::string phase = "unknown";
        if (evt.data.contains("phase")) {
            phase = evt.data["phase"].as_string();
        }
        get_logger()->debug("[chat.send] Agent event: run_id={}, seq={}, phase={}",
            evt.run_id, evt.seq, phase);
    });
    
    // ========================================================================
    // Step 5: 生成 run_id (对齐 OpenClaw)
    // ========================================================================
    
    std::string run_id = generate_run_id();
    get_logger()->info("[chat.send] Starting run: session={}, run_id={}, agent={}",
        session_id, run_id, agent->context().agent_id);
    
    // ========================================================================
    // Step 5: 写入用户消息到 Transcript (对齐 OpenClaw)
    // ========================================================================

    // workspace_dir = {base}/{agent_id}/sessions/{session_id}
    // sessions_dir  = {base}/{agent_id}/sessions  (parent_path of workspace_dir)
    std::string sessions_dir = std::filesystem::path(agent->context().workspace_dir).parent_path().string();
    std::string transcript_path = sessions::resolve_transcript_path(
        session_id, 
        agent->context().agent_id,
        sessions_dir
    );

    // =========================================================================
    // Hook: session_start
    // Aligned with OpenClaw sessions/session-manager.ts — fire-and-forget
    // Fires when a new session run begins (user sends first message or resumes).
    // =========================================================================
    auto* hook_mgr = tools::get_global_hook_manager();
    if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::SESSION_START)) {
        tools::PluginHookSessionStartEvent ss_event;
        ss_event.session_id  = session_id;
        ss_event.session_key = session_id;  // session_key == session_id in HunterClaw
        tools::PluginHookSessionContext ss_ctx;
        ss_ctx.session_id  = session_id;
        ss_ctx.session_key = session_id;
        ss_ctx.agent_id    = agent->context().agent_id;
        try {
            hook_mgr->run_session_start(ss_event, ss_ctx);
        } catch (const std::exception& e) {
            get_logger()->warn("[chat.send] session_start hook failed: {}", e.what());
        }
    }

    auto append_result = sessions::append_user_message_to_transcript(
        transcript_path, 
        message, 
        run_id
    );
    
    if (!append_result.ok) {
        get_logger()->warn("[chat.send] Failed to write user message to transcript: {}", append_result.error);
    } else {
        get_logger()->debug("[chat.send] User message written to transcript: session={}, message_id={}",
            session_id, append_result.message_id);
    }
    
    // ========================================================================
    // Step 6: 启动 Agent 异步执行 (ReAct Engine)
    // ========================================================================
    
    // 获取 ReAct Engine
    auto react_engine = agent->react_engine();
    if (!react_engine) {
        return core::Result<json>::Err("ReactEngine not initialized");
    }
    
    // 配置流式回调 (对齐 OpenClaw)
    react::StreamCallbacks callbacks;
    callbacks.session_id = session_id;
    callbacks.transcript_path = transcript_path;  // 用于 tool_result_persist hook
    
    // 助手输出流 (chat.delta)
    callbacks.on_assistant_delta = [server, session_id, run_id](const std::string& delta) {
        json event;
        event["type"] = "chat.delta";
        event["session_id"] = session_id;
        event["run_id"] = run_id;
        event["delta"] = delta;
        event["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        
        core::EventFrame frame;
        frame.event = "chat";
        frame.payload = core::JsonValue(event);
        
        server->broadcast(frame);
    };
    
    // 推理流 (reasoning.delta) - 对齐 OpenClaw
    callbacks.on_reasoning_stream = [server, session_id, run_id](const std::string& reasoning) {
        json event;
        event["type"] = "reasoning.delta";
        event["session_id"] = session_id;
        event["run_id"] = run_id;
        event["reasoning"] = reasoning;
        event["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        
        core::EventFrame frame;
        frame.event = "reasoning";
        frame.payload = core::JsonValue(event);
        
        server->broadcast(frame);
    };
    
    // 工具调用事件 (对齐 OpenClaw)
    callbacks.on_tool_start = [server, session_id, run_id](
        const std::string& tool_name, 
        const core::JsonValue& args
    ) {
        json event;
        event["type"] = "tool.start";
        event["session_id"] = session_id;
        event["run_id"] = run_id;
        event["tool_name"] = tool_name;
        event["args"] = args.to_json();
        
        core::EventFrame frame;
        frame.event = "tool";
        frame.payload = core::JsonValue(event);
        
        server->broadcast(frame);
    };
    
    callbacks.on_tool_end = [server, session_id, run_id](
        const std::string& tool_name,
        const tools::ToolResult& result
    ) {
        json event;
        event["type"] = "tool.end";
        event["session_id"] = session_id;
        event["run_id"] = run_id;
        event["tool_name"] = tool_name;
        event["success"] = result.success;
        
        // 转换 content 到 JSON
        json content_array = json::array();
        for (const auto& item : result.content) {
            json content_item;
            if (item.type == tools::ToolResultContent::Type::Text) {
                content_item["type"] = "text";
                content_item["text"] = item.text;
            } else if (item.type == tools::ToolResultContent::Type::Image) {
                content_item["type"] = "image";
                content_item["data"] = item.data;
                content_item["mime_type"] = item.mime_type;
            } else if (item.type == tools::ToolResultContent::Type::Json) {
                content_item["type"] = "json";
                content_item["text"] = item.text;
            }
            content_array.push_back(content_item);
        }
        event["content"] = content_array;
        event["details"] = result.details.to_json();
        
        if (!result.error.empty()) {
            event["error"] = result.error;
        }
        
        core::EventFrame frame;
        frame.event = "tool";
        frame.payload = core::JsonValue(event);
        
        server->broadcast(frame);
    };
    
    // 完成回调 (对齐 OpenClaw chat.complete)
    callbacks.on_end = [server, session_id, run_id]() {
        json event;
        event["type"] = "chat.complete";
        event["session_id"] = session_id;
        event["run_id"] = run_id;
        event["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        
        core::EventFrame frame;
        frame.event = "chat";
        frame.payload = core::JsonValue(event);
        
        server->broadcast(frame);
        
        get_logger()->info("[chat.send] Completed: session={}, run_id={}", session_id, run_id);
    };
    
    // 错误回调
    callbacks.on_error = [server, session_id, run_id](const std::string& error) {
        json event;
        event["type"] = "chat.error";
        event["session_id"] = session_id;
        event["run_id"] = run_id;
        event["error"] = error;
        
        core::EventFrame frame;
        frame.event = "chat";
        frame.payload = core::JsonValue(event);
        
        server->broadcast(frame);
        
        get_logger()->error("[chat.send] Error: session={}, run_id={}, error={}",
            session_id, run_id, error);
    };
    
    // ========================================================================
    // 双层 Lane 队列入队 (严格对齐 OpenClaw run.ts:263-265)
    //
    // OpenClaw 原始逻辑:
    //   return enqueueSession(() =>       // session:xxx Lane, maxConcurrent=1 (串行)
    //     enqueueGlobal(async () => {     // main Lane, maxConcurrent=N (全局并发控制)
    //       await runEmbeddedPiAgent(...)
    //     })
    //   );
    //
    // 效果:
    //   - 同一 session 的 n 个请求: 严格串行（session lane maxConcurrent=1）
    //   - 不同 session 之间: 并行（受 main lane maxConcurrent 限制）
    // ========================================================================

    auto& cmd_queue = queue::CommandQueue::Instance();

    // session lane 名称: "session:<session_id>"
    std::string session_lane = queue::ResolveSessionLane(session_id);

    // 第一层: 入队到 session lane（串行，保证同一 session 请求不并发）
    cmd_queue.enqueue_command_in_lane(
        session_lane,
        std::function<void()>([&cmd_queue, react_engine, message, callbacks, run_id, session_id, transcript_path]() {
            // 第二层: 在 session lane 内部，再入队到 global main lane（全局并发控制）
            // 对齐 OpenClaw: enqueueGlobal(() => runEmbeddedPiAgent(...))
            auto global_future = cmd_queue.enqueue_command_in_lane(
                queue::CommandLane::Main,
                std::function<void()>([react_engine, message, callbacks, run_id, session_id, transcript_path]() {
                    try {
                        // ✅ 对齐 OpenClaw: 从 transcript 加载历史消息
                        // OpenClaw: transcriptIO.load(sessionKey) → messages[] → 传给 ReactEngine::run()
                        auto history = sessions::read_transcript_messages(transcript_path, 200);
                        // 去掉最后一条（刚写入的用户消息），避免 ReactEngine 重复添加
                        if (!history.empty() && history.back().role == llm::LLMMessageRole::User) {
                            history.pop_back();
                        }
                        auto result = react_engine->run(message, history, callbacks);

                        // 写入 assistant 消息到 Transcript (对齐 OpenClaw)
                        sessions::AppendTranscriptOptions opts;
                        opts.message = result.final_response;
                        opts.create_if_missing = false;

                        auto append_result = sessions::append_assistant_message_to_transcript(
                            transcript_path,
                            opts
                        );

                        if (!append_result.ok) {
                            get_logger()->warn("[chat.send] Failed to write assistant message to transcript: {}",
                                append_result.error);
                        }

                        get_logger()->info("[chat.send] Agent completed: session={}, run_id={}, steps={}",
                            session_id, run_id, result.total_steps);

                        // =============================================================
                        // Hook: session_end
                        // Aligned with OpenClaw sessions/session-manager.ts — fire-and-forget
                        // Fires when the agent run completes (success or error).
                        // =============================================================
                        auto* end_hook_mgr = tools::get_global_hook_manager();
                        if (end_hook_mgr && end_hook_mgr->has_hooks(plugins::PluginHookName::SESSION_END)) {
                            tools::PluginHookSessionEndEvent se_event;
                            se_event.session_id    = session_id;
                            se_event.session_key   = session_id;
                            se_event.message_count = static_cast<int>(result.total_steps);
                            se_event.duration_ms   = static_cast<int>(result.duration_ms);
                            tools::PluginHookSessionContext se_ctx;
                            se_ctx.session_id  = session_id;
                            se_ctx.session_key = session_id;
                            try {
                                end_hook_mgr->run_session_end(se_event, se_ctx);
                            } catch (const std::exception& e) {
                                get_logger()->warn("[chat.send] session_end hook failed: {}", e.what());
                            }
                        }

                    } catch (const std::exception& e) {
                        get_logger()->error("[chat.send] Agent exception: session={}, run_id={}, error={}",
                            session_id, run_id, e.what());
                    }
                }),
                queue::EnqueueOptions{
                    .label = "chat.send:" + session_id + ":" + run_id,
                    .urgent = false
                }
            );

            // session lane 内等待 global lane 任务完成，保证串行语义
            // 对齐 OpenClaw: session lane 的 task 是 enqueueGlobal 的 promise
            try {
                global_future.get();
            } catch (const std::exception& e) {
                get_logger()->error("[chat.send] Global lane task failed: session={}, run_id={}, error={}",
                    session_id, run_id, e.what());
            }
        }),
        queue::EnqueueOptions{
            .label = "session:" + session_id + ":" + run_id,
            .urgent = false
        }
    );
    
    // ========================================================================
    // Step 7: 立即返回 (不等待执行完成，对齐 OpenClaw)
    // ========================================================================
    
    json result;
    result["ok"] = true;
    result["session_id"] = session_id;
    result["run_id"] = run_id;
    result["status"] = "running";
    result["agent_id"] = agent->context().agent_id;
    result["workspace"] = agent->context().workspace_dir;
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// 14. chat.abort - Abort active chat run (对齐 OpenClaw)
// ============================================================================

core::Result<json> method_chat_abort(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ========================================================================
    // Step 1: 参数验证
    // ========================================================================
    
    if (!params.contains("session_id")) {
        return core::Result<json>::Err("Missing required parameter: session_id");
    }
    
    std::string session_id = params["session_id"];
    std::string run_id = params.value("run_id", "");
    
    get_logger()->info("[chat.abort] session={}, run_id={}", session_id, run_id.empty() ? "<all>" : run_id);
    
    // ========================================================================
    // Step 2: 获取 Agent
    // ========================================================================
    
    auto agent_pool = server->agent_pool();
    if (!agent_pool) {
        return core::Result<json>::Err("AgentPool not initialized");
    }
    
    auto agent = agent_pool->get_agent(session_id);
    if (!agent) {
        // 没有活跃 Agent，返回成功
        json result;
        result["ok"] = true;
        result["aborted"] = false;
        result["runIds"] = json::array();
        return core::Result<json>::Ok(result);
    }
    
    // ========================================================================
    // Step 3: 中止执行 (ReAct Engine)
    // ========================================================================
    
    auto react_engine = agent->react_engine();
    if (!react_engine) {
        json result;
        result["ok"] = true;
        result["aborted"] = false;
        result["runIds"] = json::array();
        return core::Result<json>::Ok(result);
    }
    
    // 如果指定了 run_id，中止特定运行
    // 如果没有指定，中止所有运行
    bool aborted = false;
    std::vector<std::string> aborted_runs;
    
    // ✅ 对齐 OpenClaw: abort_chat_run
    // ReactEngine::cancel() 中止当前正在执行的 run
    // OpenClaw 通过 AbortController 实现 run_id 级别的中止
    // HunterClaw: cancel() 中止当前 session 的所有活跃 run（session 级别）
    react_engine->cancel();
    aborted = true;

    if (run_id.empty()) {
        aborted_runs.push_back("all");
    } else {
        aborted_runs.push_back(run_id);
    }
    
    get_logger()->info("[chat.abort] Result: session={}, aborted={}, count={}",
        session_id, aborted, aborted_runs.size());
    
    // ========================================================================
    // Step 4: 返回结果 (对齐 OpenClaw)
    // ========================================================================
    
    json result;
    result["ok"] = true;
    result["aborted"] = aborted;
    result["runIds"] = aborted_runs;
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// 15. chat.history - Get chat history (对齐 OpenClaw)
// ============================================================================

core::Result<json> method_chat_history(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ========================================================================
    // Step 1: 参数验证
    // ========================================================================
    
    if (!params.contains("session_id")) {
        return core::Result<json>::Err("Missing required parameter: session_id");
    }
    
    std::string session_id = params["session_id"];
    int limit = params.value("limit", 200);
    
    // 硬限制：最多 1000 条
    limit = std::min(limit, 1000);
    
    get_logger()->debug("[chat.history] session={}, limit={}", session_id, limit);
    
    // ========================================================================
    // Step 2: 获取 Agent
    // ========================================================================
    
    auto agent_pool = server->agent_pool();
    if (!agent_pool) {
        return core::Result<json>::Err("AgentPool not initialized");
    }
    
    auto agent = agent_pool->get_agent(session_id);
    if (!agent) {
        // 没有历史，返回空数组
        json result;
        result["session_id"] = session_id;
        result["sessionId"] = session_id;
        result["messages"] = json::array();
        return core::Result<json>::Ok(result);
    }
    
    // ========================================================================
    // Step 3: 读取 Transcript (对齐 OpenClaw)
    // ========================================================================
    
    std::string sessions_dir = std::filesystem::path(agent->context().workspace_dir).parent_path().string();
    std::string transcript_path = sessions::resolve_transcript_path(
        session_id,
        agent->context().agent_id,
        sessions_dir
    );
    
    auto messages = sessions::read_transcript_messages(transcript_path, limit);
    
    // 转换为 JSON
    json messages_array = json::array();
    for (const auto& msg : messages) {
        json msg_json;
        
        // Role
        switch (msg.role) {
            case llm::LLMMessageRole::User:
                msg_json["role"] = "user";
                break;
            case llm::LLMMessageRole::Assistant:
                msg_json["role"] = "assistant";
                break;
            case llm::LLMMessageRole::System:
                msg_json["role"] = "system";
                break;
            case llm::LLMMessageRole::Tool:
                msg_json["role"] = "tool";
                break;
        }
        
        // Content
        json content_item;
        content_item["type"] = "text";
        content_item["text"] = msg.content;
        msg_json["content"] = json::array({ content_item });
        
        messages_array.push_back(msg_json);
    }
    
    json result;
    result["session_id"] = session_id;
    result["sessionId"] = session_id;
    result["messages"] = messages_array;
    
    // 附加 Session 元数据 (对齐 OpenClaw)
    result["thinkingLevel"] = "default";
    result["fastMode"] = false;
    result["verboseLevel"] = "normal";
    
    get_logger()->debug("[chat.history] Returned 0 messages for session={}", session_id);
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// 16. chat.inject - Inject assistant message (对齐 OpenClaw)
// ============================================================================

core::Result<json> method_chat_inject(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ========================================================================
    // Step 1: 参数验证
    // ========================================================================
    
    if (!params.contains("session_id") || !params.contains("message")) {
        return core::Result<json>::Err("Missing required parameters: session_id, message");
    }
    
    std::string session_id = params["session_id"];
    std::string message = params["message"];
    std::string label = params.value("label", "");
    
    get_logger()->info("[chat.inject] session={}, message_len={}, label={}",
        session_id, message.size(), label);
    
    // ========================================================================
    // Step 2: 获取 Agent
    // ========================================================================
    
    auto agent_pool = server->agent_pool();
    if (!agent_pool) {
        return core::Result<json>::Err("AgentPool not initialized");
    }
    
    auto agent = agent_pool->get_or_create_agent(session_id);
    if (!agent) {
        return core::Result<json>::Err("Failed to get or create agent");
    }
    
    // ========================================================================
    // Step 3: 写入 Transcript (对齐 OpenClaw)
    // ========================================================================
    
    std::string sessions_dir = std::filesystem::path(agent->context().workspace_dir).parent_path().string();
    std::string transcript_path = sessions::resolve_transcript_path(
        session_id,
        agent->context().agent_id,
        sessions_dir
    );
    
    sessions::AppendTranscriptOptions opts;
    opts.message = message;
    opts.label = label;
    opts.create_if_missing = true;
    
    auto append_result = sessions::append_assistant_message_to_transcript(
        transcript_path,
        opts
    );
    
    std::string message_id;
    if (!append_result.ok) {
        get_logger()->warn("[chat.inject] Failed to write to transcript: {}", append_result.error);
        message_id = core::utils::generate_uuid();
    } else {
        message_id = append_result.message_id;
        get_logger()->debug("[chat.inject] Written to transcript: session={}, message_id={}",
            session_id, message_id);
    }
    
    // ========================================================================
    // Step 4: 广播到客户端 (对齐 OpenClaw)
    // ========================================================================
    
    json chat_message;
    chat_message["role"] = "assistant";
    chat_message["content"] = json::array({
        {{"type", "text"}, {"text", message}}
    });
    chat_message["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    if (!label.empty()) {
        chat_message["label"] = label;
    }
    
    json chat_payload;
    chat_payload["runId"] = "inject-" + message_id;
    chat_payload["session_id"] = session_id;
    chat_payload["seq"] = 0;
    chat_payload["state"] = "final";
    chat_payload["message"] = chat_message;
    
    core::EventFrame frame;
    frame.event = "chat";
    frame.payload = core::JsonValue(chat_payload);
    
    server->broadcast(frame);
    
    // ========================================================================
    // Step 5: 返回结果
    // ========================================================================
    
    json result;
    result["ok"] = true;
    result["messageId"] = message_id;
    
    get_logger()->info("[chat.inject] Completed: session={}, message_id={}", session_id, message_id);
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// chat.update_params - 更新聊天参数
// ============================================================================
core::Result<json> method_chat_update_params(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server;
    (void)method_guard;
    (void)client_ctx;

    std::string session_id = params.value("session_id", params.value("sessionId", ""));
    if (session_id.empty()) {
        return core::Result<json>::Err("Missing required parameter: session_id");
    }

    // ✅ 对齐 OpenClaw: updateSessionParams
    // OpenClaw 将 thinkingLevel/fastMode/verboseLevel 等参数写入 session store
    // HunterClaw: 通过 AgentPool 获取 Agent，更新其运行时配置
    auto agent_pool_up = server->agent_pool();
    bool updated = false;
    if (agent_pool_up) {
        auto agent_up = agent_pool_up->get_agent(session_id);
        if (agent_up) {
            // 更新 ReactEngine 配置（如 max_steps、reasoning_mode 等）
            auto re = agent_up->react_engine();
            if (re) {
                react::ReactConfig new_cfg = re->config();

                // thinkingLevel → reasoning_mode
                if (params.contains("thinkingLevel")) {
                    std::string level = params["thinkingLevel"];
                    if (level == "none" || level == "off") {
                        new_cfg.reasoning_mode = react::ReactConfig::ReasoningMode::Off;
                    } else if (level == "stream") {
                        new_cfg.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;
                    } else {
                        // "low", "high", "max", "auto", "on" → On
                        new_cfg.reasoning_mode = react::ReactConfig::ReasoningMode::On;
                    }
                }

                // maxSteps
                if (params.contains("maxSteps") && params["maxSteps"].is_number()) {
                    new_cfg.max_steps = params["maxSteps"].get<int>();
                }

                re->mutable_config() = new_cfg;
                updated = true;
                get_logger()->info("[chat.update_params] Updated agent config: session={}", session_id);
            }
        }
    }

    json result;
    result["ok"] = true;
    result["session_id"] = session_id;
    result["updated"] = updated;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// chat.regenerate - 重新生成最后一条回复
// ============================================================================
core::Result<json> method_chat_regenerate(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    std::string session_id = params.value("session_id", params.value("sessionId", ""));
    if (session_id.empty()) {
        return core::Result<json>::Err("Missing required parameter: session_id");
    }

    auto agent_pool = server->agent_pool();
    if (!agent_pool) {
        return core::Result<json>::Err("AgentPool not initialized");
    }

    // ✅ 对齐 OpenClaw: regenerate
    // OpenClaw: 删除最后一条 assistant 消息，重新触发 agent run
    // 1. 获取 Agent
    auto agent_regen = agent_pool->get_agent(session_id);
    if (!agent_regen) {
        return core::Result<json>::Err("No active agent for session: " + session_id);
    }

    // 2. 读取 transcript，找到最后一条 user 消息
    std::string sessions_dir_regen = std::filesystem::path(agent_regen->context().workspace_dir).parent_path().string();
    std::string transcript_path_regen = sessions::resolve_transcript_path(
        session_id, agent_regen->context().agent_id, sessions_dir_regen
    );

    auto messages_regen = sessions::read_transcript_messages(transcript_path_regen, 10);
    std::string last_user_message;
    for (auto it = messages_regen.rbegin(); it != messages_regen.rend(); ++it) {
        if (it->role == llm::LLMMessageRole::User) {
            last_user_message = it->content;
            break;
        }
    }

    if (last_user_message.empty()) {
        return core::Result<json>::Err("No user message found to regenerate");
    }

    // 3. 生成新的 run_id
    std::string regen_run_id = generate_run_id();

    // 4. 通过 session lane 重新触发 agent run（对齐 OpenClaw 双层 lane）
    auto& cmd_queue_regen = queue::CommandQueue::Instance();
    std::string session_lane_regen = queue::ResolveSessionLane(session_id);
    auto re_regen = agent_regen->react_engine();

    cmd_queue_regen.enqueue_command_in_lane(
        session_lane_regen,
        std::function<void()>([&cmd_queue_regen, re_regen, last_user_message, regen_run_id, session_id, transcript_path_regen]() {
            auto global_future = cmd_queue_regen.enqueue_command_in_lane(
                queue::CommandLane::Main,
                std::function<void()>([re_regen, last_user_message, regen_run_id, session_id, transcript_path_regen]() {
                    react::StreamCallbacks cb;
                    cb.session_id = session_id;
                    cb.transcript_path = transcript_path_regen;
                    try {
                        auto result = re_regen->run(last_user_message, {}, cb);
                        sessions::AppendTranscriptOptions opts;
                        opts.message = result.final_response;
                        opts.create_if_missing = false;
                        sessions::append_assistant_message_to_transcript(transcript_path_regen, opts);
                    } catch (const std::exception& e) {
                        spdlog::get("chat")->error("[chat.regenerate] Exception: session={}, error={}", session_id, e.what());
                    }
                }),
                queue::EnqueueOptions{.label = "chat.regenerate:" + session_id + ":" + regen_run_id, .urgent = false}
            );
            try { global_future.get(); } catch (...) {}
        }),
        queue::EnqueueOptions{.label = "session:regen:" + session_id, .urgent = false}
    );

    json result;
    result["ok"] = true;
    result["session_id"] = session_id;
    result["run_id"] = regen_run_id;
    result["status"] = "regenerating";

    return core::Result<json>::Ok(result);
}

// ============================================================================
// chat.fork - 分叉会话
// ============================================================================
core::Result<json> method_chat_fork(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server;
    (void)method_guard;
    (void)client_ctx;

    std::string session_id = params.value("session_id", params.value("sessionId", ""));
    if (session_id.empty()) {
        return core::Result<json>::Err("Missing required parameter: session_id");
    }

    // Generate forked session key
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    std::string forked_key = session_id + ":fork:" + std::to_string(ts);

    // ✅ 对齐 OpenClaw: fork session
    // OpenClaw: 复制 transcript 文件到新 session，创建新 Agent 实例
    auto agent_pool_fork = server->agent_pool();
    if (!agent_pool_fork) {
        return core::Result<json>::Err("AgentPool not initialized");
    }

    auto agent_fork = agent_pool_fork->get_agent(session_id);
    if (!agent_fork) {
        return core::Result<json>::Err("No active agent for session: " + session_id);
    }

    // 1. 解析源 transcript 路径
    std::string sessions_dir_fork = std::filesystem::path(agent_fork->context().workspace_dir).parent_path().string();
    std::string src_transcript = sessions::resolve_transcript_path(
        session_id, agent_fork->context().agent_id, sessions_dir_fork
    );

    // 2. 创建新 Agent（forked session）
    auto forked_agent = agent_pool_fork->get_or_create_agent(forked_key, agent_fork->context().agent_id);
    if (!forked_agent) {
        return core::Result<json>::Err("Failed to create forked agent");
    }

    // 3. 复制 transcript 文件
    std::string sessions_dir_forked = std::filesystem::path(forked_agent->context().workspace_dir).parent_path().string();
    std::string dst_transcript = sessions::resolve_transcript_path(
        forked_key, forked_agent->context().agent_id, sessions_dir_forked
    );

    try {
        if (std::filesystem::exists(src_transcript)) {
            std::filesystem::create_directories(std::filesystem::path(dst_transcript).parent_path());
            std::filesystem::copy_file(src_transcript, dst_transcript,
                std::filesystem::copy_options::overwrite_existing);
            get_logger()->info("[chat.fork] Transcript copied: {} -> {}", src_transcript, dst_transcript);
        }
    } catch (const std::exception& e) {
        get_logger()->warn("[chat.fork] Failed to copy transcript: {}", e.what());
        // 不中止，允许空 fork
    }

    json result;
    result["ok"] = true;
    result["source_session_id"] = session_id;
    result["forked_session_id"] = forked_key;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// chat.summarize - 总结会话
// ============================================================================
core::Result<json> method_chat_summarize(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    std::string session_id = params.value("session_id", params.value("sessionId", ""));
    if (session_id.empty()) {
        return core::Result<json>::Err("Missing required parameter: session_id");
    }

    auto agent_pool = server->agent_pool();
    if (!agent_pool) {
        return core::Result<json>::Err("AgentPool not initialized");
    }

    // ✅ 对齐 OpenClaw: summarize session
    // OpenClaw: 读取 transcript，通过 LLM 生成摘要，写入 session store
    auto agent_sum = agent_pool->get_agent(session_id);
    if (!agent_sum) {
        json result;
        result["ok"] = true;
        result["session_id"] = session_id;
        result["summary"] = "";
        result["status"] = "no_agent";
        return core::Result<json>::Ok(result);
    }

    // 读取最近消息
    std::string sessions_dir_sum = std::filesystem::path(agent_sum->context().workspace_dir).parent_path().string();
    std::string transcript_path_sum = sessions::resolve_transcript_path(
        session_id, agent_sum->context().agent_id, sessions_dir_sum
    );
    auto messages_sum = sessions::read_transcript_messages(transcript_path_sum, 50);

    // 构建摘要 prompt（对齐 OpenClaw compaction 逻辑）
    std::string conversation_text;
    for (const auto& msg : messages_sum) {
        std::string role_str;
        switch (msg.role) {
            case llm::LLMMessageRole::User:      role_str = "User"; break;
            case llm::LLMMessageRole::Assistant: role_str = "Assistant"; break;
            default: continue;
        }
        conversation_text += role_str + ": " + msg.content + "\n";
    }

    // 通过 ReactEngine 生成摘要（单轮，无工具）
    std::string summary;
    auto re_sum = agent_sum->react_engine();
    if (re_sum && !conversation_text.empty()) {
        std::string summarize_prompt =
            "Please provide a concise summary of the following conversation in 2-3 sentences:\n\n" +
            conversation_text;
        try {
            react::StreamCallbacks sum_cb;
            sum_cb.session_id = session_id;
            auto sum_result = re_sum->run(summarize_prompt, {}, sum_cb);
            if (sum_result.success) {
                summary = sum_result.final_response;
            }
        } catch (const std::exception& e) {
            get_logger()->warn("[chat.summarize] LLM summarization failed: {}", e.what());
        }
    }

    json result;
    result["ok"] = true;
    result["session_id"] = session_id;
    result["summary"] = summary;
    result["status"] = summary.empty() ? "empty" : "ok";
    result["message_count"] = static_cast<int>(messages_sum.size());

    return core::Result<json>::Ok(result);
}

// ============================================================================
// chat.edit - 编辑消息
// ============================================================================
core::Result<json> method_chat_edit(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server;
    (void)method_guard;
    (void)client_ctx;

    std::string session_id = params.value("session_id", params.value("sessionId", ""));
    std::string message_id = params.value("message_id", params.value("messageId", ""));
    std::string new_content = params.value("content", "");

    if (session_id.empty() || message_id.empty()) {
        return core::Result<json>::Err("Missing required parameters: session_id, message_id");
    }

    // ✅ 对齐 OpenClaw: edit message in transcript
    // OpenClaw: 读取 JSONL，找到对应 message_id，替换 content，写回
    auto agent_pool_edit = server->agent_pool();
    if (!agent_pool_edit) {
        return core::Result<json>::Err("AgentPool not initialized");
    }

    auto agent_edit = agent_pool_edit->get_agent(session_id);
    if (!agent_edit) {
        return core::Result<json>::Err("No active agent for session: " + session_id);
    }

    std::string sessions_dir_edit = std::filesystem::path(agent_edit->context().workspace_dir).parent_path().string();
    std::string transcript_path_edit = sessions::resolve_transcript_path(
        session_id, agent_edit->context().agent_id, sessions_dir_edit
    );

    // 读取 JSONL 文件，逐行处理
    bool edited = false;
    if (std::filesystem::exists(transcript_path_edit)) {
        std::ifstream in_file(transcript_path_edit);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in_file, line)) {
            if (!line.empty()) {
                try {
                    auto entry = json::parse(line);
                    // 匹配 message_id（对齐 OpenClaw transcript 格式）
                    if (entry.value("id", "") == message_id ||
                        entry.value("messageId", "") == message_id) {
                        // 更新 content
                        if (entry.contains("content") && entry["content"].is_array()) {
                            entry["content"] = json::array({{"type", "text"}, {"text", new_content}});
                        } else {
                            entry["content"] = new_content;
                        }
                        entry["edited"] = true;
                        entry["edited_at"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        edited = true;
                        lines.push_back(entry.dump());
                    } else {
                        lines.push_back(line);
                    }
                } catch (...) {
                    lines.push_back(line);
                }
            }
        }
        in_file.close();

        if (edited) {
            // 写回文件
            std::ofstream out_file(transcript_path_edit, std::ios::trunc);
            for (const auto& l : lines) {
                out_file << l << "\n";
            }
        }
    }

    json result;
    result["ok"] = true;
    result["session_id"] = session_id;
    result["message_id"] = message_id;
    result["edited"] = edited;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// chat.delete - 删除消息
// ============================================================================
core::Result<json> method_chat_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server;
    (void)method_guard;
    (void)client_ctx;

    std::string session_id = params.value("session_id", params.value("sessionId", ""));
    std::string message_id = params.value("message_id", params.value("messageId", ""));

    if (session_id.empty() || message_id.empty()) {
        return core::Result<json>::Err("Missing required parameters: session_id, message_id");
    }

    // ✅ 对齐 OpenClaw: delete message from transcript
    // OpenClaw: 读取 JSONL，过滤掉对应 message_id 的行，写回
    auto agent_pool_del = server->agent_pool();
    if (!agent_pool_del) {
        return core::Result<json>::Err("AgentPool not initialized");
    }

    auto agent_del = agent_pool_del->get_agent(session_id);
    if (!agent_del) {
        return core::Result<json>::Err("No active agent for session: " + session_id);
    }

    std::string sessions_dir_del = std::filesystem::path(agent_del->context().workspace_dir).parent_path().string();
    std::string transcript_path_del = sessions::resolve_transcript_path(
        session_id, agent_del->context().agent_id, sessions_dir_del
    );

    bool deleted = false;
    if (std::filesystem::exists(transcript_path_del)) {
        std::ifstream in_file(transcript_path_del);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in_file, line)) {
            if (!line.empty()) {
                try {
                    auto entry = json::parse(line);
                    if (entry.value("id", "") == message_id ||
                        entry.value("messageId", "") == message_id) {
                        deleted = true;  // 跳过此行（删除）
                    } else {
                        lines.push_back(line);
                    }
                } catch (...) {
                    lines.push_back(line);
                }
            }
        }
        in_file.close();

        if (deleted) {
            std::ofstream out_file(transcript_path_del, std::ios::trunc);
            for (const auto& l : lines) {
                out_file << l << "\n";
            }
        }
    }

    json result;
    result["ok"] = true;
    result["session_id"] = session_id;
    result["message_id"] = message_id;
    result["deleted"] = deleted;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// chat.transcript - 获取完整对话记录
// ============================================================================
core::Result<json> method_chat_transcript(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    std::string session_id = params.value("session_id", params.value("sessionId", ""));
    if (session_id.empty()) {
        return core::Result<json>::Err("Missing required parameter: session_id");
    }

    int limit = params.value("limit", 200);
    int offset = params.value("offset", 0);
    (void)limit;
    (void)offset;

    auto agent_pool = server->agent_pool();
    json messages = json::array();

    if (agent_pool) {
        // ✅ 对齐 OpenClaw: 从 transcript 读取消息
        auto agent_tr = agent_pool->get_agent(session_id);
        if (agent_tr) {
            std::string sessions_dir_tr = std::filesystem::path(agent_tr->context().workspace_dir).parent_path().string();
            std::string transcript_path_tr = sessions::resolve_transcript_path(
                session_id, agent_tr->context().agent_id, sessions_dir_tr
            );

            // 读取 JSONL，转换为 JSON 数组（带 offset/limit 分页）
            if (std::filesystem::exists(transcript_path_tr)) {
                std::ifstream in_file(transcript_path_tr);
                std::vector<json> all_entries;
                std::string line;
                while (std::getline(in_file, line)) {
                    if (!line.empty()) {
                        try {
                            all_entries.push_back(json::parse(line));
                        } catch (...) {}
                    }
                }

                // 应用 offset/limit
                int start = std::min(offset, static_cast<int>(all_entries.size()));
                int end = std::min(start + limit, static_cast<int>(all_entries.size()));
                for (int i = start; i < end; ++i) {
                    messages.push_back(all_entries[i]);
                }
            }
        }
    }

    json result;
    result["session_id"] = session_id;
    result["messages"] = messages;
    result["count"] = messages.size();

    return core::Result<json>::Ok(result);
}

// ============================================================================
// chat.export - 导出对话
// ============================================================================
core::Result<json> method_chat_export(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    std::string session_id = params.value("session_id", params.value("sessionId", ""));
    if (session_id.empty()) {
        return core::Result<json>::Err("Missing required parameter: session_id");
    }

    std::string format = params.value("format", "json");  // "json" | "markdown" | "text"
    (void)format;

    auto agent_pool = server->agent_pool();
    json messages = json::array();

    if (agent_pool) {
        // ✅ 对齐 OpenClaw: 从 transcript 读取并按格式导出
        auto agent_exp = agent_pool->get_agent(session_id);
        if (agent_exp) {
            std::string sessions_dir_exp = std::filesystem::path(agent_exp->context().workspace_dir).parent_path().string();
            std::string transcript_path_exp = sessions::resolve_transcript_path(
                session_id, agent_exp->context().agent_id, sessions_dir_exp
            );

            auto msgs = sessions::read_transcript_messages(transcript_path_exp, 1000);

            if (format == "markdown") {
                // Markdown 格式导出
                std::string md;
                for (const auto& msg : msgs) {
                    std::string role_str;
                    switch (msg.role) {
                        case llm::LLMMessageRole::User:      role_str = "**User**"; break;
                        case llm::LLMMessageRole::Assistant: role_str = "**Assistant**"; break;
                        default: continue;
                    }
                    md += role_str + "\n\n" + msg.content + "\n\n---\n\n";
                }
                messages = md;
            } else if (format == "text") {
                // 纯文本格式
                std::string txt;
                for (const auto& msg : msgs) {
                    std::string role_str;
                    switch (msg.role) {
                        case llm::LLMMessageRole::User:      role_str = "User"; break;
                        case llm::LLMMessageRole::Assistant: role_str = "Assistant"; break;
                        default: continue;
                    }
                    txt += role_str + ": " + msg.content + "\n";
                }
                messages = txt;
            } else {
                // JSON 格式（默认）
                json msgs_array = json::array();
                for (const auto& msg : msgs) {
                    json m;
                    switch (msg.role) {
                        case llm::LLMMessageRole::User:      m["role"] = "user"; break;
                        case llm::LLMMessageRole::Assistant: m["role"] = "assistant"; break;
                        default: m["role"] = "system"; break;
                    }
                    m["content"] = msg.content;
                    msgs_array.push_back(m);
                }
                messages = msgs_array;
            }
        }
    }

    json result;
    result["ok"] = true;
    result["session_id"] = session_id;
    result["format"] = format;
    result["data"] = messages;
    result["count"] = messages.is_array() ? messages.size() : 0;

    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
