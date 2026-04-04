#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/tools/tool_hooks.h"             // PluginHook* types + get_global_hook_manager()
#include "hunterclaw/sessions/transcript_io.h"       // append_tool_result_to_transcript (tool_result_persist)
#include "hunterclaw/observability/observability.h"  // ✅ Span, Tracer, SpanKind
#include "hunterclaw/observability/diagnostic_events.h"  // ✅ 可观测性集成
#include <regex>
#include <algorithm>
#include <future>
#include <thread>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace react {

namespace {

// Logger
auto log() {
    static auto logger = spdlog::get("react");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("react");
    }
    return logger;
}

// ========== OpenClaw 对齐: 格式化推理消息 (不添加前缀) ==========
// 注意: ReactEngine 内部只做流式提取，格式化由上层处理
// 参考: openclaw-analysis/src/agents/pi-embedded-utils.ts:273-287
std::string format_reasoning_text(const std::string& text) {
    // ReactEngine 内部不添加 "Reasoning:" 前缀
    // 只返回 trim 后的原始内容
    std::string trimmed = text;
    auto start = trimmed.find_first_not_of(" \t\n\r");
    auto end = trimmed.find_last_not_of(" \t\n\r");
    
    if (start == std::string::npos) {
        return "";
    }
    
    return trimmed.substr(start, end - start + 1);
}

// 获取 CPU 核心数 (用于默认线程池大小)
int get_hardware_concurrency() {
    int cores = std::thread::hardware_concurrency();
    return (cores > 0) ? cores : 4;  // 默认 4 核
}

// 提取 <think>, <thinking>, <thought>, <antthinking> 标签
// 对齐 OpenClaw: THINKING_TAG_SCAN_RE
std::regex thinking_tag_re(R"(<\s*(/?)?\s*(?:think(?:ing)?|thought|antthinking)\s*>)", 
                          std::regex_constants::icase);

// 提取 <final> 标签
std::regex final_tag_re(R"(<\s*(/?)?\s*final\s*>)", 
                       std::regex_constants::icase);

// 分块分隔符
bool is_paragraph_boundary(char c) {
    return c == '\n';
}

bool is_sentence_boundary(const std::string& text, size_t pos) {
    if (pos >= text.size()) return false;
    char c = text[pos];
    return (c == '.' || c == '!' || c == '?') && 
           (pos + 1 >= text.size() || text[pos + 1] == ' ' || text[pos + 1] == '\n');
}

} // namespace

// ✅ 全局线程池 (懒加载单例)
std::shared_ptr<core::ThreadPool>& ReactEngine::get_thread_pool() {
    static std::shared_ptr<core::ThreadPool> pool;
    static std::once_flag init_flag;
    
    std::call_once(init_flag, []() {
        int pool_size = get_hardware_concurrency();
        log()->info("Initializing global thread pool with {} threads", pool_size);
        pool = std::make_shared<core::ThreadPool>(pool_size);
    });
    
    return pool;
}

// ReactEngine 实现
std::shared_ptr<ReactEngine> ReactEngine::create(
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const ReactConfig& config
) {
    return std::shared_ptr<ReactEngine>(
        new ReactEngine(tool_manager, llm_client, config)
    );
}

// ✅ create_with_provider - 直接使用 Provider (推荐)
std::shared_ptr<ReactEngine> ReactEngine::create_with_provider(
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::Provider> provider,
    const ReactConfig& config
) {
    return std::shared_ptr<ReactEngine>(
        new ReactEngine(tool_manager, provider, config)
    );
}

ReactEngine::ReactEngine(
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const ReactConfig& config
) : tool_manager_(tool_manager),
    llm_client_(llm_client),
    provider_(nullptr),  // ✅ 使用 LLMClient 时 provider_ 为 null
    config_(config) {
    
    // 🎯 如果配置了自定义线程池大小,重新初始化
    if (config_.thread_pool_size > 0) {
        auto& pool = get_thread_pool();
        if (!pool || pool->num_threads() != static_cast<size_t>(config_.thread_pool_size)) {
            log()->info("Reinitializing thread pool with {} threads", config_.thread_pool_size);
            pool = std::make_shared<core::ThreadPool>(config_.thread_pool_size);
        }
    }
}

// ✅ Provider 构造函数
ReactEngine::ReactEngine(
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::Provider> provider,
    const ReactConfig& config
) : tool_manager_(tool_manager),
    llm_client_(nullptr),  // ✅ 使用 Provider 时 llm_client_ 为 null
    provider_(provider),
    config_(config) {
    
    // 线程池初始化
    if (config_.thread_pool_size > 0) {
        auto& pool = get_thread_pool();
        if (!pool || pool->num_threads() != static_cast<size_t>(config_.thread_pool_size)) {
            log()->info("Reinitializing thread pool with {} threads", config_.thread_pool_size);
            pool = std::make_shared<core::ThreadPool>(config_.thread_pool_size);
        }
    }
}

ReactResult ReactEngine::run(
    const std::string& user_input,
    const std::vector<llm::LLMMessage>& history,
    const StreamCallbacks& callbacks
) {
    cancelled_ = false;
    running_ = true;  // ✅ Mark engine as running (enables steer)
    // Clear any leftover steer messages from previous runs
    {
        std::lock_guard<std::mutex> lock(steer_mutex_);
        std::queue<std::string> empty;
        steer_queue_.swap(empty);
    }
    start_time_ = std::chrono::steady_clock::now();
    
    // ✅ 可观测性: 开启 Span (对齐 OpenClaw)
    std::unique_ptr<observability::Span> span;
    try {
        span = observability::Tracer::instance().start_span(
            "react.run", 
            observability::SpanKind::Internal
        );
        span->set_attribute("user_input_length", static_cast<int64_t>(user_input.size()));
        span->set_attribute("history_size", static_cast<int64_t>(history.size()));
        if (!callbacks.session_id.empty()) {
            span->set_attribute("session_id", callbacks.session_id);
        }
    } catch (...) {
        // 故障隔离: Span 创建失败不影响主流程
    }
    
    // 生命周期: start
    if (callbacks.on_start) {
        callbacks.on_start();
    }
    
    try {
        auto result = execute_loop(user_input, history, callbacks);
        
        // ✅ Span 成功标记
        if (span) {
            span->set_status(result.success, result.error);
            span->set_attribute("total_steps", static_cast<int64_t>(result.steps.size()));
            span->set_attribute("tool_calls", static_cast<int64_t>(result.tool_calls));
            span->set_attribute("input_tokens", static_cast<int64_t>(result.usage.input_tokens));
            span->set_attribute("output_tokens", static_cast<int64_t>(result.usage.output_tokens));
        }
        
        // 生命周期: end
        if (callbacks.on_end) {
            callbacks.on_end();
        }
        
        running_ = false;  // ✅ Mark engine as stopped (disables steer)
        return result;
    } catch (const std::exception& e) {
        // ✅ Span 失败标记
        if (span) {
            span->set_status(false, e.what());
            span->add_event("error", {{"exception", e.what()}});
        }
        
        // 生命周期: error
        if (callbacks.on_error) {
            callbacks.on_error(e.what());
        }
        
        running_ = false;  // ✅ Mark engine as stopped (disables steer)
        ReactResult result;
        result.success = false;
        result.error = e.what();
        return result;
    }
}

std::future<ReactResult> ReactEngine::run_async(
    const std::string& user_input,
    const std::vector<llm::LLMMessage>& history,
    const StreamCallbacks& callbacks
) {
    return std::async(std::launch::async, [this, user_input, history, callbacks]() {
        return run(user_input, history, callbacks);
    });
}

void ReactEngine::cancel() {
    cancelled_ = true;
}

// ============================================================================
// steer — 向正在运行的 execute_loop 实时注入 user message
// 🎯 对齐 OpenClaw: activeSession.steer(text)  (attempt.ts:2231)
//   → session.agent.steer(AgentMessage{role:"user", content:text})
//
// 线程安全: steer_mutex_ 保护 steer_queue_。
// execute_loop 在每轮循环开始时消费 steer_queue_ 中的消息。
//
// 返回 true: 消息已入队（engine 正在运行 execute_loop）
// 返回 false: engine 未运行，steer 被拒绝
// ============================================================================
bool ReactEngine::steer(const std::string& text) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (text.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(steer_mutex_);
        steer_queue_.push(text);
    }
    log()->info("[ReactEngine] steer: queued user message ({} chars)", text.size());
    return true;
}

ReactResult ReactEngine::execute_loop(
    const std::string& user_input,
    const std::vector<llm::LLMMessage>& history,
    const StreamCallbacks& callbacks
) {
    ReactResult result;
    result.success = true;

    // =========================================================================
    // Hook: before_model_resolve
    // Aligned with OpenClaw run.ts:329 — runs before resolveModel()
    // Allows plugins to override provider/model for this run.
    // =========================================================================
    auto* hook_mgr = tools::get_global_hook_manager();
    std::string effective_model_override;
    std::string effective_provider_override;

    if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::BEFORE_MODEL_RESOLVE)) {
        tools::PluginHookBeforeModelResolveEvent bmr_event;
        bmr_event.prompt = user_input;
        tools::PluginHookAgentContext hook_ctx;
        hook_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);
        try {
            auto bmr_result = hook_mgr->run_before_model_resolve(bmr_event, hook_ctx);
            if (bmr_result) {
                if (bmr_result->model_override)    effective_model_override    = *bmr_result->model_override;
                if (bmr_result->provider_override) effective_provider_override = *bmr_result->provider_override;
                if (!effective_model_override.empty())
                    log()->info("[hooks] model overridden to {}", effective_model_override);
                if (!effective_provider_override.empty())
                    log()->info("[hooks] provider overridden to {}", effective_provider_override);
            }
        } catch (const std::exception& e) {
            log()->warn("before_model_resolve hook failed: {}", e.what());
        }
    }

    // =========================================================================
    // Hook: before_agent_start (legacy) + before_prompt_build
    // Aligned with OpenClaw run.ts:339 and attempt.ts:1172/1188
    // Legacy: before_agent_start may also carry model/provider overrides.
    // before_prompt_build: inject system prompt / context additions.
    // =========================================================================
    std::optional<std::string> hook_system_prompt;
    std::optional<std::string> hook_prepend_context;
    std::optional<std::string> hook_prepend_system_context;
    std::optional<std::string> hook_append_system_context;

    if (hook_mgr) {
        tools::PluginHookAgentContext hook_ctx;
        hook_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);

        // Legacy: before_agent_start
        if (hook_mgr->has_hooks(plugins::PluginHookName::BEFORE_AGENT_START)) {
            tools::PluginHookBeforeAgentStartEvent bas_event;
            bas_event.prompt = user_input;
            try {
                auto bas_result = hook_mgr->run_before_agent_start(bas_event, hook_ctx);
                if (bas_result) {
                    if (bas_result->system_prompt)           hook_system_prompt           = bas_result->system_prompt;
                    if (bas_result->prepend_context)         hook_prepend_context         = bas_result->prepend_context;
                    if (bas_result->prepend_system_context)  hook_prepend_system_context  = bas_result->prepend_system_context;
                    if (bas_result->append_system_context)   hook_append_system_context   = bas_result->append_system_context;
                    // Legacy model/provider override (lower priority than before_model_resolve)
                    if (effective_model_override.empty()    && bas_result->model_override)
                        effective_model_override    = *bas_result->model_override;
                    if (effective_provider_override.empty() && bas_result->provider_override)
                        effective_provider_override = *bas_result->provider_override;
                }
            } catch (const std::exception& e) {
                log()->warn("before_agent_start hook (legacy) failed: {}", e.what());
            }
        }

        // before_prompt_build (takes precedence over legacy for prompt fields)
        if (hook_mgr->has_hooks(plugins::PluginHookName::BEFORE_PROMPT_BUILD)) {
            tools::PluginHookBeforePromptBuildEvent bpb_event;
            bpb_event.prompt = user_input;
            bpb_event.messages_json = "[]";  // history serialized; simplified here
            try {
                auto bpb_result = hook_mgr->run_before_prompt_build(bpb_event, hook_ctx);
                if (bpb_result) {
                    // before_prompt_build takes precedence (last wins for systemPrompt)
                    if (bpb_result->system_prompt)          hook_system_prompt          = bpb_result->system_prompt;
                    if (bpb_result->prepend_context)        hook_prepend_context        = bpb_result->prepend_context;
                    if (bpb_result->prepend_system_context) hook_prepend_system_context = bpb_result->prepend_system_context;
                    if (bpb_result->append_system_context)  hook_append_system_context  = bpb_result->append_system_context;
                }
            } catch (const std::exception& e) {
                log()->warn("before_prompt_build hook failed: {}", e.what());
            }
        }
    }
    // (hook_system_prompt / hook_prepend_context etc. are available for use
    //  when building the actual LLM prompt — passed to call_llm via config)
    (void)hook_system_prompt;
    (void)hook_prepend_context;
    (void)hook_prepend_system_context;
    (void)hook_append_system_context;
    (void)effective_model_override;
    (void)effective_provider_override;

    // 🎯 OpenClaw 对齐: 重置流式状态 (pi-embedded-subscribe.ts: state init)
    streaming_state_.reset();
    // accumulated_reasoning_raw_/formatted_ are part of streaming_state_ now
    accumulated_reasoning_raw_.clear();
    accumulated_reasoning_formatted_.clear();

    // 构建消息列表
    std::vector<llm::LLMMessage> messages = history;
    
    // 添加用户输入 (Observe)
    llm::LLMMessage user_msg;
    user_msg.role = llm::LLMMessageRole::User;
    user_msg.content = user_input;
    messages.push_back(user_msg);

    // 🎯 对齐 OpenClaw: handleAgentStart — emit lifecycle start event
    if (callbacks.on_start) {
        callbacks.on_start();
    }

    ReactStep observe_step;
    observe_step.type = StepType::Observe;
    observe_step.content = user_input;
    observe_step.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    result.steps.push_back(observe_step);

    // ReAct 循环: Reason → Act → Observe → ...
    int step_count = 0;
    std::string accumulated_response;
    // 🎯 对齐 OpenClaw: assistantTexts accumulator (mirrors state.assistantTexts)
    std::vector<std::string> assistant_texts;
    
    while (step_count < config_.max_steps && !cancelled_) {
        // ✅ 新增: Token Overflow 检查 (对齐 OpenClaw)
        if (compaction_mgr_) {
            int current_tokens = estimate_conversation_tokens(messages);
            int context_window = context_window_config_.context_window_tokens;
            
            // 转换 messages 为 AgentMessage (用于 should_compact 检查)
            std::vector<memory::AgentMessage> agent_messages;
            for (const auto& msg : messages) {
                memory::AgentMessage am;
                am.role = [&]() {
                    switch (msg.role) {
                        case llm::LLMMessageRole::User: return "user";
                        case llm::LLMMessageRole::Assistant: return "assistant";
                        case llm::LLMMessageRole::System: return "system";
                        case llm::LLMMessageRole::Tool: return "toolResult";
                        default: return "unknown";
                    }
                }();
                am.content = msg.content;
                agent_messages.push_back(am);
            }
            
            // 检查是否需要 compaction
            if (compaction_mgr_->should_compact(agent_messages, current_tokens, context_window)) {
                log()->warn("[ReactEngine] Token overflow detected: {} >= threshold, triggering compaction",
                           current_tokens);
                
                // 触发 compaction
                auto compact_result = trigger_compaction(messages);
                
                if (compact_result.compacted) {
                    log()->info("[ReactEngine] Compaction completed, continuing with {} messages",
                               messages.size());
                    // messages 已被 trigger_compaction 更新
                } else {
                    log()->warn("[ReactEngine] Compaction skipped: {}", compact_result.reason);
                }
            }
        }
        
        // 检查超时
        auto elapsed = std::chrono::steady_clock::now() - start_time_;
        if (elapsed > config_.total_timeout) {
            result.success = false;
            result.error = "ReAct loop timeout";
            break;
        }
        
        // ✅ 新增: 消费 steer 队列 (对齐 OpenClaw: activeSession.steer → agent.steer)
        // OpenClaw 语义: steer 消息作为新的 user turn 实时注入到正在运行的 LLM session
        //   attempt.ts:2231: queueHandle.queueMessage → activeSession.steer(text)
        //   pi-agent-core: session.agent.steer(AgentMessage{role:"user", content:text})
        //
        // HunterClaw 实现: 在每轮 ReAct 循环开始时，将 steer 队列中的消息
        // 作为 user 消息追加到 messages 列表。这会在下一次 call_llm 时让 LLM 看到。
        {
            std::lock_guard<std::mutex> lock(steer_mutex_);
            while (!steer_queue_.empty()) {
                std::string steer_text = std::move(steer_queue_.front());
                steer_queue_.pop();
                
                llm::LLMMessage steer_msg;
                steer_msg.role = llm::LLMMessageRole::User;
                steer_msg.content = std::move(steer_text);
                messages.push_back(std::move(steer_msg));
                
                log()->info("[ReactEngine] steer consumed: injected user message into messages (total={})",
                           messages.size());
            }
        }
        
        // 🎯 对齐 OpenClaw: handleMessageStart — reset state at message boundary
        // ASSUME: message_start is the only reliable boundary for "new assistant message begins".
        // Resetting at text_end is unsafe (late/duplicate end events).
        streaming_state_.reset_assistant_message_state(
            static_cast<int>(assistant_texts.size()));
        // Emit onAssistantMessageStart (earliest "writing" signal)
        if (callbacks.on_assistant_message_start) {
            callbacks.on_assistant_message_start();
        }

        // 步骤 1: Reason (推理)
        auto llm_response = call_llm(messages, callbacks);
        
        // 更新 token 统计
        result.usage.input_tokens += llm_response.input_tokens;
        result.usage.output_tokens += llm_response.output_tokens;
        result.usage.total_tokens += llm_response.input_tokens + llm_response.output_tokens;
        result.usage.cache_read_tokens += llm_response.cache_read_tokens;
        result.usage.cache_write_tokens += llm_response.cache_write_tokens;
        
        // 记录推理步骤
        ReactStep reason_step;
        reason_step.type = StepType::Reason;
        reason_step.content = llm_response.text;
        reason_step.reasoning = llm_response.reasoning;
        reason_step.has_reasoning = !llm_response.reasoning.empty();
        reason_step.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        
        // 累积每一轮的响应文本（包括工具调用前的推理）
        accumulated_response += llm_response.text;
        
        // 如果有工具调用
        if (!llm_response.tool_calls.empty()) {
            // 步骤 2: Act (行动)
            result.tool_calls += llm_response.tool_calls.size();
            
            // 发送块回复 (在工具执行前)
            if (config_.block_reply.enabled && 
                config_.block_reply.break_mode == ReactConfig::BlockReply::BreakMode::TextEnd &&
                !accumulated_response.empty()) {
                if (callbacks.on_block_reply) {
                    callbacks.on_block_reply(accumulated_response);
                }
                accumulated_response.clear();
            }
            
            // 执行工具
            auto tool_results = execute_tools(llm_response.tool_calls, callbacks);
            reason_step.tool_calls.reserve(llm_response.tool_calls.size());
            for (const auto& tc : llm_response.tool_calls) {
                reason_step.tool_calls.push_back(tc.name);  // ToolCall has name field directly
            }
            // 转换 tool_results 类型: ToolResult -> JsonValue
            for (const auto& [tool_name, tool_result] : tool_results) {
                // 使用 details 字段作为结构化结果
                reason_step.tool_results[tool_name] = tool_result.details;
            }
            
            // 添加助手消息 (带工具调用)
            llm::LLMMessage assistant_msg;
            assistant_msg.role = llm::LLMMessageRole::Assistant;
            assistant_msg.content = llm_response.text;
            // assistant_msg.tool_calls = llm_response.tool_calls;  // 注释掉,类型不匹配
            messages.push_back(assistant_msg);
            
            // 添加工具结果消息
            for (const auto& [tool_call_id, tool_result] : tool_results) {
                llm::LLMMessage tool_msg;
                tool_msg.role = llm::LLMMessageRole::Tool;
                tool_msg.tool_call_id = tool_call_id;
                
                // 构建工具结果内容
                std::string result_content;
                if (tool_result.success) {
                    for (const auto& content : tool_result.content) {
                        if (content.type == tools::ToolResultContent::Type::Text) {
                            result_content += content.text;
                        } else if (content.type == tools::ToolResultContent::Type::Json) {
                            result_content += content.text;  // JSON 已序列化
                        }
                    }
                } else {
                    result_content = "Error: " + tool_result.error;
                }
                
                tool_msg.content = result_content;
                messages.push_back(tool_msg);

                // =========================================================
                // Hook: tool_result_persist
                // Aligned with OpenClaw attempt.ts — sync hook, called when
                // a tool result is about to be persisted to the transcript.
                // We delegate to append_tool_result_to_transcript which
                // internally fires tool_result_persist + before_message_write.
                // =========================================================
                if (!callbacks.transcript_path.empty()) {
                    sessions::AppendToolResultOptions trp_opts;
                    trp_opts.tool_name    = tool_call_id;  // tool_call_id maps to tool name here
                    trp_opts.tool_call_id = tool_call_id;
                    trp_opts.result_json  = result_content;
                    trp_opts.run_id       = callbacks.session_id;
                    sessions::append_tool_result_to_transcript(
                        callbacks.transcript_path, trp_opts);
                } else {
                    // No transcript path: fire tool_result_persist hook directly
                    auto* trp_hook_mgr = tools::get_global_hook_manager();
                    if (trp_hook_mgr && trp_hook_mgr->has_tool_result_persist_hooks()) {
                        tools::PluginHookToolResultPersistEvent trp_event;
                        trp_event.tool_call_id = tool_call_id.empty()
                            ? std::nullopt : std::make_optional(tool_call_id);
                        trp_event.message_json = result_content;
                        trp_event.is_synthetic = false;
                        tools::PluginHookToolResultPersistContext trp_ctx;
                        trp_ctx.tool_call_id = trp_event.tool_call_id;
                        try {
                            trp_hook_mgr->run_tool_result_persist(trp_event, trp_ctx);
                        } catch (const std::exception& e) {
                            log()->warn("tool_result_persist hook failed: {}", e.what());
                        }
                    }
                }
                
                // Observe (观察工具结果)
                ReactStep observe_tool_step;
                observe_tool_step.type = StepType::Observe;
                observe_tool_step.content = result_content;
                observe_tool_step.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count();
                result.steps.push_back(observe_tool_step);
            }
            
            result.steps.push_back(reason_step);
            
            // 继续下一轮循环 (工具结果 → 再次推理)
            step_count++;
            continue;
        }
        
        // 没有工具调用 - 这是最终响应
        reason_step.is_final = true;
        result.steps.push_back(reason_step);

        // 步骤 3: Respond (响应)
        // 🎯 OpenClaw 对齐: 根据 reasoning_mode 决定 final_response 内容
        switch (config_.reasoning_mode) {
            case ReactConfig::ReasoningMode::Off:
                // 用户不想看到 thinking - 使用过滤后的 block_buffer
                result.final_response = streaming_state_.block_buffer;
                break;
            case ReactConfig::ReasoningMode::On:
            case ReactConfig::ReasoningMode::Stream:
                // 用户想看到 thinking - 使用原始 delta_buffer (包含 <thinking>)
                result.final_response = streaming_state_.delta_buffer;
                break;
        }

        // 🎯 对齐 OpenClaw: handleAgentEnd — flush block reply buffer
        // Called at agent_end, not just at loop end, to ensure delivery before compaction.
        if (config_.block_reply.enabled) {
            flush_block_reply_buffer(callbacks);
        }
        // 🎯 对齐 OpenClaw: onBlockReplyFlush — called at agent_end (#35074)
        // Ensures buffered block replies reach the channel before compaction wait blocks the run.
        if (callbacks.on_block_reply_flush) {
            callbacks.on_block_reply_flush();
        }

        // 🎯 对齐 OpenClaw: finalizeAssistantTexts
        // If includeReasoning=true and no onBlockReply, replace streamed chunks with final text.
        if (config_.reasoning_mode != ReactConfig::ReasoningMode::Off &&
            !callbacks.on_block_reply && !llm_response.text.empty()) {
            // Replace all chunks added during this message with the final text
            if (static_cast<int>(assistant_texts.size()) > streaming_state_.assistant_text_baseline) {
                assistant_texts.resize(streaming_state_.assistant_text_baseline);
            }
            assistant_texts.push_back(llm_response.text);
            streaming_state_.suppress_block_chunks = true;
        }

        break;  // 完成
    }
    
    if (cancelled_) {
        result.success = false;
        result.error = "ReAct loop cancelled";
    } else if (step_count >= config_.max_steps) {
        result.success = false;
        result.error = "ReAct loop exceeded max steps";
    }

    // 🎯 对齐 OpenClaw: store assistantTexts in result
    result.assistant_texts = std::move(assistant_texts);

    result.total_steps = result.steps.size();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_
    ).count();

    // =========================================================================
    // Hook: agent_end
    // Aligned with OpenClaw attempt.ts:2678 — fire-and-forget, runs even on error
    // Allows plugins to analyze the completed conversation.
    // =========================================================================
    if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::AGENT_END)) {
        tools::PluginHookAgentEndEvent ae_event;
        // Serialize messages to JSON string
        ae_event.messages_json = "[]";  // simplified; full serialization via transcript
        ae_event.success = result.success;
        if (!result.error.empty()) ae_event.error = result.error;
        ae_event.duration_ms = static_cast<int>(result.duration_ms);

        tools::PluginHookAgentContext ae_ctx;
        ae_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);
        try {
            hook_mgr->run_agent_end(ae_event, ae_ctx);
        } catch (const std::exception& e) {
            log()->warn("agent_end hook failed: {}", e.what());
        }
    }
    
    // ✅ 可观测性: 发射 ToolLoop 事件 (对齐 OpenClaw)
    try {
        auto loop_event = std::make_shared<observability::ToolLoopEvent>();
        loop_event->session_id = callbacks.session_id.empty() ? "unknown" : callbacks.session_id;
        loop_event->consecutive_calls = step_count;
        loop_event->is_loop_detected = step_count >= config_.max_steps;
        loop_event->timestamp = std::chrono::system_clock::now();
        
        observability::DiagnosticEventBus::instance().emit(loop_event);
    } catch (...) {
        // 故障隔离
    }
    
    return result;
}

ReactEngine::LLMResponseData ReactEngine::call_llm(
    const std::vector<llm::LLMMessage>& messages,
    const StreamCallbacks& callbacks,
    const std::vector<llm::ToolDefinition>& tools
) {
    LLMResponseData response;
    
    // 使用流式接口 (对齐 OpenClaw)
    std::string accumulated_text;
    std::string accumulated_reasoning;
    std::vector<llm::ToolCall> accumulated_tool_calls_;

    // ✅ 向后兼容: 使用 LLMClient 接口
    // 🎯 对齐 OpenClaw: handleMessageUpdate — text_delta / text_end / thinking_delta / thinking_end
    // The stream_callback here simulates the text_delta path (most common).
    // text_end monotonicity is handled by the LLM client layer (or approximated below).
    auto stream_callback = [&](const std::string& delta) {
        if (delta.empty()) return;

        // 🎯 对齐 OpenClaw: deltaBuffer accumulation (text_delta path)
        streaming_state_.delta_buffer += delta;

        // 1. 提取推理内容 (thinking tags)
        auto extraction = extract_reasoning(delta);

        // 🎯 对齐 OpenClaw: thinking_delta / thinking_end handling
        // emitReasoningStream: compute delta vs lastStreamedReasoning
        if (extraction.has_reasoning && callbacks.on_reasoning_stream) {
            accumulated_reasoning_raw_ += extraction.reasoning;
            std::string formatted = format_reasoning_text(accumulated_reasoning_raw_);

            // Compute delta: new text since last emitted reasoning.
            // Guard against non-prefix changes (e.g. trim/format altering earlier content).
            const std::string& prior = streaming_state_.last_streamed_reasoning;
            std::string delta_reasoning;
            if (!prior.empty() && formatted.find(prior) == 0) {
                delta_reasoning = formatted.substr(prior.length());
            } else {
                delta_reasoning = formatted;
            }
            streaming_state_.last_streamed_reasoning = formatted;

            if (!delta_reasoning.empty()) {
                streaming_state_.reasoning_stream_open = true;
                callbacks.on_reasoning_stream(delta_reasoning);
            }
            accumulated_reasoning += extraction.reasoning;
        }

        // 2. 发送助手输出流 (on_assistant_delta / onPartialReply)
        if (!extraction.remaining_text.empty()) {
            if (callbacks.on_assistant_delta) {
                callbacks.on_assistant_delta(extraction.remaining_text);
            }
        }

        // 3. 累积文本用于最终响应
        accumulated_text += extraction.remaining_text;

        // 🎯 对齐 OpenClaw: blockBuffer accumulation (filtered, no <think> tags)
        streaming_state_.block_buffer += extraction.remaining_text;

        // 4. 🎯 对齐 OpenClaw: blockReplyBreak="text_end" — flush at each text_end
        // In the streaming callback we approximate text_end as each delta chunk.
        // For blockReplyBreak="message_end", flushing happens at message_end (after call_llm).
        if (config_.block_reply.enabled &&
            config_.block_reply.break_mode == ReactConfig::BlockReply::BreakMode::TextEnd &&
            callbacks.on_block_reply &&
            !streaming_state_.block_buffer.empty()) {
            // Flush on paragraph/sentence boundaries (approximate text_end behavior)
            bool should_flush = false;
            const auto& buf = streaming_state_.block_buffer;
            if ((int)buf.size() >= config_.block_reply.max_chars) {
                should_flush = true;
            } else if ((int)buf.size() >= config_.block_reply.min_chars) {
                switch (config_.block_reply.break_preference) {
                    case ReactConfig::BlockReply::BreakPreference::Paragraph:
                    case ReactConfig::BlockReply::BreakPreference::Newline:
                        should_flush = (buf.back() == '\n');
                        break;
                    case ReactConfig::BlockReply::BreakPreference::Sentence:
                        should_flush = is_sentence_boundary(buf, buf.size() - 1);
                        break;
                }
            }
            if (should_flush) {
                emit_block_chunk(streaming_state_.block_buffer, callbacks);
                streaming_state_.block_buffer.clear();
            }
        }
    };
    
    // =========================================================================
    // Hook: llm_input
    // Aligned with OpenClaw attempt.ts:2431 — fire-and-forget, before LLM call
    // Allows plugins to observe the exact input payload sent to the LLM.
    // =========================================================================
    auto* llm_hook_mgr = tools::get_global_hook_manager();
    if (llm_hook_mgr && llm_hook_mgr->has_hooks(plugins::PluginHookName::LLM_INPUT)) {
        tools::PluginHookLlmInputEvent li_event;
        li_event.run_id     = callbacks.session_id + "-run";  // approximate run_id
        li_event.session_id = callbacks.session_id;
        li_event.provider   = llm_client_ ? llm_client_->get_config().provider : "unknown";
        li_event.model      = llm_client_ ? llm_client_->get_config().model    : "unknown";
        li_event.prompt     = messages.empty() ? "" : messages.back().content;
        li_event.history_messages_json = "[]";  // simplified
        li_event.images_count = 0;

        tools::PluginHookAgentContext li_ctx;
        li_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);
        try {
            llm_hook_mgr->run_llm_input(li_event, li_ctx);
        } catch (const std::exception& e) {
            log()->warn("llm_input hook failed: {}", e.what());
        }
    }

    // 调用 LLM 的流式接口 (从 messages)
    auto result = llm_client_->generate_stream_from_messages(messages, stream_callback);
    
    if (!result.ok) {
        // 失败时返回错误
        response.text = "Error: " + result.error;
        return response;
    }
    
    // 🎯 对齐 OpenClaw: message_end — flush blockBuffer for message_end break mode
    if (config_.block_reply.enabled &&
        config_.block_reply.break_mode == ReactConfig::BlockReply::BreakMode::MessageEnd &&
        callbacks.on_block_reply &&
        !streaming_state_.block_buffer.empty()) {
        emit_block_chunk(streaming_state_.block_buffer, callbacks);
        streaming_state_.block_buffer.clear();
    }

    // 🎯 对齐 OpenClaw: reasoning stream end
    if (streaming_state_.reasoning_stream_open && callbacks.on_reasoning_end) {
        streaming_state_.reasoning_stream_open = false;
        callbacks.on_reasoning_end();
    }

    response.text = accumulated_text;
    response.reasoning = accumulated_reasoning;
    // ✅ 对齐 OpenClaw: 解析 tool_calls 和 usage
    // tool_calls 已在流式回调中收集，这里从 accumulated_tool_calls_ 读取
    if (!accumulated_tool_calls_.empty()) {
        response.tool_calls = accumulated_tool_calls_;
        accumulated_tool_calls_.clear();
    }
    // usage 已在流式回调中更新到 response.input_tokens/output_tokens

    // =========================================================================
    // Hook: llm_output
    // Aligned with OpenClaw attempt.ts:2737 — fire-and-forget, after LLM response
    // Allows plugins to observe the exact output payload returned by the LLM.
    // =========================================================================
    if (llm_hook_mgr && llm_hook_mgr->has_hooks(plugins::PluginHookName::LLM_OUTPUT)) {
        tools::PluginHookLlmOutputEvent lo_event;
        lo_event.run_id     = callbacks.session_id + "-run";
        lo_event.session_id = callbacks.session_id;
        lo_event.provider   = llm_client_ ? llm_client_->get_config().provider : "unknown";
        lo_event.model      = llm_client_ ? llm_client_->get_config().model    : "unknown";
        if (!accumulated_text.empty()) lo_event.assistant_texts.push_back(accumulated_text);
        tools::PluginHookLlmOutputUsage lo_usage;
        lo_usage.input  = response.input_tokens  > 0 ? std::make_optional(response.input_tokens)  : std::nullopt;
        lo_usage.output = response.output_tokens > 0 ? std::make_optional(response.output_tokens) : std::nullopt;
        lo_event.usage = lo_usage;

        tools::PluginHookAgentContext lo_ctx;
        lo_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);
        try {
            llm_hook_mgr->run_llm_output(lo_event, lo_ctx);
        } catch (const std::exception& e) {
            log()->warn("llm_output hook failed: {}", e.what());
        }
    }

    // ✅ 可观测性: 发射 ModelUsage 事件 (对齐 OpenClaw)
    try {
        // ✅ 从 llm_client_ 获取真实的 provider 和 model
        const auto& llm_config = llm_client_->get_config();
        
        // ✅ 计算成本 (简化版,可根据实际定价调整)
        double cost_usd = 0.0;
        if (llm_config.provider == "openai") {
            // GPT-4 定价: $0.03/1K input + $0.06/1K output
            cost_usd = (response.input_tokens * 0.03 / 1000.0) +
                      (response.output_tokens * 0.06 / 1000.0);
        } else if (llm_config.provider == "anthropic") {
            // Claude 定价: $0.015/1K input + $0.075/1K output
            cost_usd = (response.input_tokens * 0.015 / 1000.0) +
                      (response.output_tokens * 0.075 / 1000.0);
        }
        
        auto usage_event = std::make_shared<observability::ModelUsageEvent>();
        usage_event->session_id = callbacks.session_id.empty() ? "unknown" : callbacks.session_id;
        usage_event->provider = llm_config.provider;
        usage_event->model = llm_config.model;
        usage_event->prompt_tokens = response.input_tokens;
        usage_event->completion_tokens = response.output_tokens;
        usage_event->total_tokens = response.input_tokens + response.output_tokens;
        usage_event->cost_usd = cost_usd;
        usage_event->timestamp = std::chrono::system_clock::now();
        
        observability::DiagnosticEventBus::instance().emit(usage_event);
    } catch (...) {
        // 故障隔离: 永不阻塞主流程
    }
    
    return response;
}
std::map<std::string, tools::ToolResult> ReactEngine::execute_tools(
    const std::vector<llm::ToolCall>& tool_calls,
    const StreamCallbacks& callbacks
) {
    std::map<std::string, tools::ToolResult> results;

    // 🎯 对齐 OpenClaw: serial execution path (default)
    // OpenClaw executes tools via pi-agent-core which handles them one by one
    // unless the model requests parallel tool calls.
    if (!config_.parallel_tool_calls || tool_calls.size() <= 1) {
        for (const auto& tool_call : tool_calls) {
            if (callbacks.on_tool_start) {
                callbacks.on_tool_start(tool_call.name, tool_call.input);
            }
            std::unique_ptr<observability::Span> span;
            try {
                span = observability::Tracer::instance().start_span(
                    "tool.execute", observability::SpanKind::Client);
                span->set_attribute("tool_name", tool_call.name);
                span->set_attribute("tool_call_id", tool_call.id);
            } catch (...) {}
            try {
                auto result = tool_manager_->execute_tool(tool_call.name, tool_call.input);
                if (span) span->set_status(result.success, result.error);
                if (callbacks.on_tool_end) callbacks.on_tool_end(tool_call.name, result);
                results[tool_call.id] = result;
            } catch (const std::exception& e) {
                if (span) { span->set_status(false, e.what()); }
                tools::ToolResult error_result;
                error_result.success = false;
                error_result.error = std::string("Tool execution failed: ") + e.what();
                if (callbacks.on_tool_end) callbacks.on_tool_end(tool_call.name, error_result);
                results[tool_call.id] = error_result;
            }
        }
        return results;
    }

    if (config_.parallel_tool_calls && tool_calls.size() > 1) {
        // ✅ 并行执行 (使用全局线程池)
        log()->info("Executing {} tools in parallel using thread pool", tool_calls.size());
        
        auto& thread_pool = get_thread_pool();
        std::vector<std::future<std::pair<std::string, tools::ToolResult>>> futures;
        
        for (const auto& tool_call : tool_calls) {
            // 发送工具开始事件
            if (callbacks.on_tool_start) {
                // ✅ 对齐 OpenClaw: 解析 tool_call.function.arguments
                callbacks.on_tool_start(tool_call.name, tool_call.input);
                
                if (!callbacks.session_id.empty()) {
                }
            }
            
            // ✅ 提交任务到全局线程池 (避免频繁创建/销毁线程)
            auto future = thread_pool->submit([this, tool_call, callbacks]() 
                -> std::pair<std::string, tools::ToolResult> {
                
                // ✅ 可观测性: 工具执行 Span (对齐 OpenClaw)
                std::unique_ptr<observability::Span> span;
                try {
                    span = observability::Tracer::instance().start_span(
                        "tool.execute", 
                        observability::SpanKind::Client
                    );
                    span->set_attribute("tool_name", tool_call.name);
                    span->set_attribute("tool_call_id", tool_call.id);
                } catch (...) {
                    // 故障隔离
                }
                
                try {
                    // ✅ 对齐 OpenClaw: 解析 tool_call.function.arguments (JSON string) 为 JsonValue
                    // tool_call.input 已经是解析后的 JsonValue
                    core::JsonValue args = tool_call.input;
                    
                    // 执行工具 (可能耗时)
                    auto result = tool_manager_->execute_tool(tool_call.name, args);
                    
                    // ✅ Span 成功标记
                    if (span) {
                        span->set_status(result.success, result.error);
                    }
                    
                    return std::make_pair(tool_call.id, result);
                    
                } catch (const std::exception& e) {
                    // ✅ Span 失败标记
                    if (span) {
                        span->set_status(false, e.what());
                        span->add_event("error", {{"exception", e.what()}});
                    }
                    
                    // 捕获异常并转换为错误结果
                    tools::ToolResult error_result;
                    error_result.success = false;
                    error_result.error = std::string("Tool execution failed: ") + e.what();
                    return {tool_call.id, error_result};
                }
            });
            
            futures.push_back(std::move(future));
        }
        
        // 收集所有结果 (保持原始顺序)
        for (size_t i = 0; i < futures.size(); ++i) {
            const auto& tool_call = tool_calls[i];
            try {
                auto [call_id, result] = futures[i].get();
                if (callbacks.on_tool_end) callbacks.on_tool_end(tool_call.name, result);
                results[call_id] = result;
            } catch (const std::exception& e) {
                log()->error("Failed to get tool execution result: {}", e.what());
                tools::ToolResult error_result;
                error_result.success = false;
                error_result.error = std::string("Tool execution failed: ") + e.what();
                results[tool_call.id] = error_result;
            }
        }
    }

    return results;
}

ReactEngine::ReasoningExtraction ReactEngine::extract_reasoning(const std::string& text) {
    ReasoningExtraction extraction;
    extraction.remaining_text = text;
    
    // 查找 thinking tags
    std::smatch match;
    std::string search_text = text;
    std::string thinking_content;
    std::string non_thinking_content;
    
    bool in_thinking = false;
    size_t last_pos = 0;
    
    while (std::regex_search(search_text, match, thinking_tag_re)) {
        size_t match_pos = match.position(0);
        
        if (!in_thinking) {
            // 开始 thinking
            non_thinking_content += search_text.substr(last_pos, match_pos - last_pos);
            in_thinking = true;
        } else {
            // 结束 thinking
            thinking_content += search_text.substr(last_pos, match_pos - last_pos);
            in_thinking = false;
            extraction.has_reasoning = true;
        }
        
        last_pos = match.position(0) + match.length(0);
        search_text = match.suffix().str();
    }
    
    // 剩余内容
    if (in_thinking) {
        thinking_content += search_text.substr(last_pos);
    } else {
        non_thinking_content += search_text.substr(last_pos);
    }
    
    extraction.reasoning = thinking_content;
    extraction.remaining_text = non_thinking_content;
    
    return extraction;
}

std::vector<std::string> ReactEngine::chunk_text(const std::string& text) {
    std::vector<std::string> chunks;
    
    if (text.size() <= config_.block_reply.min_chars) {
        chunks.push_back(text);
        return chunks;
    }
    
    size_t pos = 0;
    std::string current_chunk;
    
    while (pos < text.size()) {
        // 尝试找到合适的分割点
        size_t chunk_end = std::min(pos + config_.block_reply.max_chars, text.size());
        
        if (chunk_end < text.size()) {
            // 寻找分割点
            switch (config_.block_reply.break_preference) {
                case ReactConfig::BlockReply::BreakPreference::Paragraph:
                    // 查找段落边界
                    while (chunk_end > pos + config_.block_reply.min_chars) {
                        if (is_paragraph_boundary(text[chunk_end])) {
                            break;
                        }
                        chunk_end--;
                    }
                    break;
                    
                case ReactConfig::BlockReply::BreakPreference::Sentence:
                    // 查找句子边界
                    while (chunk_end > pos + config_.block_reply.min_chars) {
                        if (is_sentence_boundary(text, chunk_end)) {
                            chunk_end++;  // 包含标点
                            break;
                        }
                        chunk_end--;
                    }
                    break;
                    
                case ReactConfig::BlockReply::BreakPreference::Newline:
                    // 查找换行
                    while (chunk_end > pos + config_.block_reply.min_chars) {
                        if (text[chunk_end] == '\n') {
                            break;
                        }
                        chunk_end--;
                    }
                    break;
            }
        }
        
        current_chunk = text.substr(pos, chunk_end - pos);
        if (!current_chunk.empty()) {
            chunks.push_back(current_chunk);
        }
        
        pos = chunk_end;
    }
    
    return chunks;
}

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw 的流式处理机制
// 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts:477-553
// ═══════════════════════════════════════════════════════════════

// 发送块回复 (带智能去重和冲突检测)
// 🎯 对齐 OpenClaw: emitBlockChunk (pi-embedded-subscribe.ts)
void ReactEngine::emit_block_chunk(const std::string& text, const StreamCallbacks& callbacks) {
    // 🎯 1. 动态抑制检测 (对齐 OpenClaw: suppressBlockChunks)
    // Set by finalizeAssistantTexts when includeReasoning=true and no onBlockReply.
    if (streaming_state_.suppress_block_chunks) {
        return;
    }

    // 🎯 2. 过滤 thinking/final 标签 + trim (对齐 OpenClaw: stripBlockTags + trimEnd)
    std::string chunk = strip_thinking_tags(text);
    chunk = trim_end(chunk);
    if (chunk.empty()) {
        return;
    }

    // 🎯 3. 智能去重 (对齐 OpenClaw: chunk === state.lastBlockReplyText)
    if (chunk == streaming_state_.last_block_reply_text) {
        return;
    }

    // 🎯 4. Messaging Tool 冲突检测 (对齐 OpenClaw: isMessagingToolDuplicateNormalized)
    // Only check committed (successful) messaging tool texts — checking pending texts
    // is risky because if the tool fails after suppression, the user gets no response.
    std::string normalized_chunk = normalize_text_for_comparison(chunk);
    if (is_messaging_tool_duplicate(normalized_chunk)) {
        log()->debug("Skipping block reply - already sent via messaging tool: {}...",
                     chunk.substr(0, std::min((size_t)50, chunk.size())));
        return;
    }

    // 🎯 5. shouldSkipAssistantText check (对齐 OpenClaw: emitBlockChunk calls shouldSkipAssistantText)
    // Prevents emitting a chunk that duplicates a previously-emitted assistant text.
    if (streaming_state_.last_assistant_text_message_index == streaming_state_.assistant_message_index) {
        const std::string trimmed = trim_end(chunk);
        if (!trimmed.empty() && trimmed == streaming_state_.last_assistant_text_trimmed) {
            return;
        }
        const std::string norm = normalize_text_for_comparison(chunk);
        if (!norm.empty() && norm == streaming_state_.last_assistant_text_normalized) {
            return;
        }
    }

    // 🎯 6. 记录发送内容 (对齐 OpenClaw: state.lastBlockReplyText = chunk)
    streaming_state_.last_block_reply_text = chunk;
    // Also track in lastAssistantText for future shouldSkipAssistantText checks
    streaming_state_.last_assistant_text_message_index = streaming_state_.assistant_message_index;
    streaming_state_.last_assistant_text_trimmed = trim_end(chunk);
    streaming_state_.last_assistant_text_normalized = normalize_text_for_comparison(chunk);

    // 🎯 7. 零开销检测
    if (!callbacks.on_block_reply) {
        return;
    }

    // 🎯 8. 发送回调
    callbacks.on_block_reply(chunk);
}

// 刷新块回复缓冲区
// 对齐 OpenClaw: flushBlockReplyBuffer (line 540-553)
void ReactEngine::flush_block_reply_buffer(const StreamCallbacks& callbacks) {
    // ✅ 零开销检测 (对齐 OpenClaw:541-542)
    if (!callbacks.on_block_reply) {
        return;
    }
    
    // ✅ 发送 block_buffer 中的内容 (对齐 OpenClaw:549-552)
    if (!streaming_state_.block_buffer.empty()) {
        emit_block_chunk(streaming_state_.block_buffer, callbacks);
        streaming_state_.block_buffer.clear();
    }
}

// 文本规范化 (用于去重比较)
// 对齐 OpenClaw: normalizeTextForComparison
std::string ReactEngine::normalize_text_for_comparison(const std::string& text) {
    std::string result = text;
    
    // 1. 去除首尾空白
    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end = result.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    result = result.substr(start, end - start + 1);
    
    // 2. 折叠多个空白为单个空格
    std::regex whitespace_re("\\s+");
    result = std::regex_replace(result, whitespace_re, " ");
    
    return result;
}

// 检测 Messaging Tool 冲突
// 对齐 OpenClaw: isMessagingToolDuplicateNormalized
bool ReactEngine::is_messaging_tool_duplicate(const std::string& normalized_text) {
    for (const auto& sent : streaming_state_.messaging_tool_sent_texts_normalized) {
        if (normalized_text == sent) {
            return true;
        }
    }
    return false;
}

// 去除 thinking 标签
std::string ReactEngine::strip_thinking_tags(const std::string& text) {
    // 简化实现: 使用正则替换
    std::string result = std::regex_replace(text, thinking_tag_re, "");
    return result;
}

// 去除尾部空白
std::string ReactEngine::trim_end(const std::string& text) {
    size_t end = text.find_last_not_of(" \t\n\r");
    if (end == std::string::npos) {
        return "";
    }
    return text.substr(0, end + 1);
}

// ═══════════════════════════════════════════════════════════════
// ✅ Compaction 集成 (对齐 OpenClaw)
// 参考: openclaw-analysis/src/agents/pi-embedded-runner/run.ts:980-1034
// ═══════════════════════════════════════════════════════════════

// Token 估算 (简化版: 1 token ≈ 4 chars)
int ReactEngine::estimate_conversation_tokens(const std::vector<LLMMessage>& messages) const {
    int total = 0;
    for (const auto& msg : messages) {
        // 估算消息内容 token
        total += msg.content.size() / 4;
        
        // 估算角色和格式开销
        total += 10;  // <role>content</role> 开销
    }
    return total;
}

// 检测 Context Overflow Error (对齐 OpenClaw: isLikelyContextOverflowError)
bool ReactEngine::is_context_overflow_error(const std::string& error_msg) const {
    std::string lower = error_msg;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    return (
        (lower.find("context") != std::string::npos && 
         lower.find("overflow") != std::string::npos) ||
        lower.find("too long") != std::string::npos ||
        (lower.find("maximum") != std::string::npos && 
         lower.find("token") != std::string::npos) ||
        (lower.find("exceeds") != std::string::npos && 
         lower.find("limit") != std::string::npos)
    );
}

// 提取 Overflow Token Count (对齐 OpenClaw: extractObservedOverflowTokenCount)
std::optional<int> ReactEngine::extract_overflow_token_count(const std::string& error_msg) const {
    // 匹配 "277403 tokens > 200000 maximum"
    std::regex token_count_re(R"((\d+)\s*tokens?\s*>\s*(\d+))");
    std::smatch match;
    
    if (std::regex_search(error_msg, match, token_count_re)) {
        return std::stoi(match[1].str());
    }
    
    return std::nullopt;
}

// 触发 Compaction (对齐 OpenClaw)
ReactEngine::CompactionResult ReactEngine::trigger_compaction(
    std::vector<LLMMessage>& messages
) {
    CompactionResult result;
    
    if (!compaction_mgr_) {
        result.reason = "CompactionManager not initialized";
        return result;
    }
    
    int messages_before = messages.size();
    int current_tokens = estimate_conversation_tokens(messages);

    log()->info("[ReactEngine] Triggering compaction: {} messages, ~{} tokens",
               messages_before, current_tokens);

    // =========================================================================
    // Hook: before_compaction
    // Aligned with OpenClaw compact.ts:857 and run.ts:1045
    // Fire-and-forget, runs before compaction starts.
    // =========================================================================
    auto* compact_hook_mgr = tools::get_global_hook_manager();
    if (compact_hook_mgr && compact_hook_mgr->has_hooks(plugins::PluginHookName::BEFORE_COMPACTION)) {
        tools::PluginHookBeforeCompactionEvent bc_event;
        bc_event.message_count   = static_cast<int>(messages.size());
        bc_event.compacting_count = static_cast<int>(messages.size());
        bc_event.token_count     = current_tokens;
        tools::PluginHookAgentContext bc_ctx;
        try {
            compact_hook_mgr->run_before_compaction(bc_event, bc_ctx);
        } catch (const std::exception& e) {
            log()->warn("before_compaction hook failed: {}", e.what());
        }
    }
    
    // 转换 LLMMessage → AgentMessage
    std::vector<memory::AgentMessage> agent_messages;
    for (const auto& msg : messages) {
        memory::AgentMessage am;
        am.role = [&]() {
            switch (msg.role) {
                case llm::LLMMessageRole::User: return "user";
                case llm::LLMMessageRole::Assistant: return "assistant";
                case llm::LLMMessageRole::System: return "system";
                case llm::LLMMessageRole::Tool: return "toolResult";
                default: return "unknown";
            }
        }();
        am.content = msg.content;
        agent_messages.push_back(am);
    }
    
    // 构建 CompactionParams
    memory::CompactionParams params;
    params.messages = agent_messages;
    params.current_token_count = current_tokens;
    params.context_window_tokens = context_window_config_.context_window_tokens;
    params.trigger = "overflow";
    params.model_id = llm_client_ ? llm_client_->get_config().model : "claude-3-5-sonnet-20241022";  // ✅ 从配置读取
    
    // ✅ 对齐 OpenClaw: 设置 API key 和回调
    if (llm_client_) {
        params.api_key = llm_client_->get_config().api_key;
    }
    // params.memory_flush_callback 和 params.post_compaction_sync_callback
    // 在 CompactionManager 内部处理
    
    // 执行 compaction
    auto compact_result = compaction_mgr_->compact_session(params);
    
    if (!compact_result.ok) {
        log()->error("[ReactEngine] Compaction failed: {}", compact_result.error);
        result.reason = compact_result.error;
        return result;
    }
    
    auto& compact_data = compact_result.get();

    if (!compact_data.compacted) {
        log()->info("[ReactEngine] Compaction skipped: {}", compact_data.reason);
        result.reason = compact_data.reason;
        return result;
    }
    
    log()->info("[ReactEngine] Compaction successful:");
    log()->info("  Messages: {} → {}", 
               compact_data.messages_before,
               compact_data.messages_after);
    log()->info("  Tokens saved: {}", compact_data.tokens_saved);
    
    // ✅ 转换回 LLMMessage
    messages.clear();
    for (const auto& am : compact_data.compressed_messages) {
        llm::LLMMessage msg;
        msg.role = [&]() {
            if (am.role == "user") return llm::LLMMessageRole::User;
            if (am.role == "assistant") return llm::LLMMessageRole::Assistant;
            if (am.role == "system") return llm::LLMMessageRole::System;
            if (am.role == "toolResult") return llm::LLMMessageRole::Tool;
            return llm::LLMMessageRole::User;
        }();
        msg.content = am.content;
        messages.push_back(msg);
    }
    
    result.compacted = true;
    result.compressed_messages = messages;
    result.messages_before = messages_before;
    result.messages_after = messages.size();
    result.tokens_saved = compact_data.tokens_saved;

    // =========================================================================
    // Hook: after_compaction
    // Aligned with OpenClaw compact.ts:988 and run.ts:1106
    // Fire-and-forget, runs after successful compaction.
    // =========================================================================
    if (compact_hook_mgr && compact_hook_mgr->has_hooks(plugins::PluginHookName::AFTER_COMPACTION)) {
        tools::PluginHookAfterCompactionEvent ac_event;
        ac_event.message_count   = static_cast<int>(messages.size());
        ac_event.compacted_count = messages_before - static_cast<int>(messages.size());
        ac_event.token_count     = estimate_conversation_tokens(messages);
        tools::PluginHookAgentContext ac_ctx;
        try {
            compact_hook_mgr->run_after_compaction(ac_event, ac_ctx);
        } catch (const std::exception& e) {
            log()->warn("after_compaction hook failed: {}", e.what());
        }
    }

    return result;
}

} // namespace react
} // namespace hunterclaw
