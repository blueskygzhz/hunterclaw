#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/tools/tool_manager.h"
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

ReactResult ReactEngine::execute_loop(
    const std::string& user_input,
    const std::vector<llm::LLMMessage>& history,
    const StreamCallbacks& callbacks
) {
    ReactResult result;
    result.success = true;
    
    // 🎯 OpenClaw 对齐: 重置流式状态
    streaming_state_.reset();
    
    // ✅ OpenClaw 对齐: 重置 reasoning stream 累积状态
    accumulated_reasoning_raw_.clear();
    accumulated_reasoning_formatted_.clear();
    
    // 构建消息列表
    std::vector<llm::LLMMessage> messages = history;
    
    // 添加用户输入 (Observe)
    llm::LLMMessage user_msg;
    user_msg.role = llm::LLMMessageRole::User;
    user_msg.content = user_input;
    messages.push_back(user_msg);
    
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
        
        // ✅ 严格对齐 OpenClaw: flush 最终块回复 (带去重和冲突检测)
        if (config_.block_reply.enabled) {
            flush_block_reply_buffer(callbacks);
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
    
    result.total_steps = result.steps.size();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_
    ).count();
    
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
    std::string current_chunk_buffer;  // 用于 block_reply 分块
    
    // ✅ 向后兼容: 使用 LLMClient 接口
    auto stream_callback = [&](const std::string& delta) {
        if (delta.empty()) return;
        
        // 🎯 OpenClaw 对齐: 累积原始内容
        streaming_state_.delta_buffer += delta;
        
        // 1. 提取推理内容 (thinking tags)
        auto extraction = extract_reasoning(delta);
        
        // ✅ 严格对齐 OpenClaw: Reasoning Stream 增量处理
        // 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts:556-586
        if (extraction.has_reasoning && callbacks.on_reasoning_stream) {
            // 累积原始 reasoning
            accumulated_reasoning_raw_ += extraction.reasoning;
            
            // 格式化累积内容 (由回调层处理格式化，这里只发送原始内容)
            std::string formatted = format_reasoning_text(accumulated_reasoning_raw_);
            
            // 计算相对于上次的增量
            std::string delta_reasoning;
            if (!accumulated_reasoning_formatted_.empty() &&
                formatted.find(accumulated_reasoning_formatted_) == 0) {
                // 新内容是旧内容的前缀扩展 → 只发送增量
                delta_reasoning = formatted.substr(accumulated_reasoning_formatted_.length());
            } else {
                // 非前缀或首次 → 发送全量
                delta_reasoning = formatted;
            }
            
            accumulated_reasoning_formatted_ = formatted;
            
            // 只在有增量时发送
            if (!delta_reasoning.empty()) {
                callbacks.on_reasoning_stream(delta_reasoning);
            }
            
            accumulated_reasoning += extraction.reasoning;
        }
        
        // 2. 发送助手输出流 (on_assistant_delta)
        if (!extraction.remaining_text.empty() && callbacks.on_assistant_delta) {
            callbacks.on_assistant_delta(extraction.remaining_text);
            
            if (!callbacks.session_id.empty()) {
            }
        }
        
        // 3. 累积文本用于最终响应
        accumulated_text += extraction.remaining_text;
        current_chunk_buffer += extraction.remaining_text;
        
        // 🎯 OpenClaw 对齐: 同步更新 block_buffer (过滤后的内容)
        streaming_state_.block_buffer += extraction.remaining_text;
        
        // 4. 块回复逻辑 (block_reply) - 仅在 BreakMode::MessageEnd 时使用
        // 注意: TextEnd 模式在工具调用前处理，这里只处理流式分块
        if (config_.block_reply.enabled && 
            config_.block_reply.break_mode == ReactConfig::BlockReply::BreakMode::MessageEnd &&
            callbacks.on_block_reply) {
            
            bool should_send_block = false;
            
            if (current_chunk_buffer.size() >= config_.block_reply.max_chars) {
                should_send_block = true;
            } else if (current_chunk_buffer.size() >= config_.block_reply.min_chars) {
                // 检查是否在合适的断点
                switch (config_.block_reply.break_preference) {
                    case ReactConfig::BlockReply::BreakPreference::Paragraph:
                        if (current_chunk_buffer.back() == '\n') {
                            should_send_block = true;
                        }
                        break;
                    case ReactConfig::BlockReply::BreakPreference::Sentence:
                        if (is_sentence_boundary(current_chunk_buffer, current_chunk_buffer.size() - 1)) {
                            should_send_block = true;
                        }
                        break;
                    case ReactConfig::BlockReply::BreakPreference::Newline:
                        if (current_chunk_buffer.back() == '\n') {
                            should_send_block = true;
                        }
                        break;
                }
            }
            
            if (should_send_block) {
                callbacks.on_block_reply(current_chunk_buffer);
                
                if (!callbacks.session_id.empty()) {
                }
                
                current_chunk_buffer.clear();
            }
        }
    };
    
    // 调用 LLM 的流式接口 (从 messages)
    auto result = llm_client_->generate_stream_from_messages(messages, stream_callback);
    
    if (!result.ok) {
        // 失败时返回错误
        response.text = "Error: " + result.error;
        return response;
    }
    
    // 发送剩余的 buffer (如果有)
    if (!current_chunk_buffer.empty() && 
        config_.block_reply.enabled && 
        config_.block_reply.break_mode == ReactConfig::BlockReply::BreakMode::MessageEnd &&
        callbacks.on_block_reply) {
        callbacks.on_block_reply(current_chunk_buffer);
    }
    
    response.text = accumulated_text;
    response.reasoning = accumulated_reasoning;
    // TODO: 解析 tool_calls 和 usage
    // response.tool_calls = ...
    // response.usage = ...
    
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
    
    if (config_.parallel_tool_calls && tool_calls.size() > 1) {
        // ✅ 并行执行 (使用全局线程池)
        log()->info("Executing {} tools in parallel using thread pool", tool_calls.size());
        
        auto& thread_pool = get_thread_pool();
        std::vector<std::future<std::pair<std::string, tools::ToolResult>>> futures;
        
        for (const auto& tool_call : tool_calls) {
            // 发送工具开始事件
            if (callbacks.on_tool_start) {
                // TODO: 解析 tool_call.function.arguments
                callbacks.on_tool_start(tool_call.name, core::JsonValue{});
                
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
                    // TODO: 解析 tool_call.function.arguments (JSON string) 为 JsonValue
                    core::JsonValue args;
                    
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
                // 等待任务完成
                auto [call_id, result] = futures[i].get();
                
                // 发送工具完成事件
                if (callbacks.on_tool_end) {
                    callbacks.on_tool_end(tool_call.name, result);
                }
                
                // 记录结果
                results[call_id] = result;
                
            } catch (const std::exception& e) {
                // 处理异常 (例如线程抛出的异常)
                log()->error("Failed to get tool execution result: {}", e.what());
                
                // 记录错误结果
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
// 对齐 OpenClaw: emitBlockChunk
void ReactEngine::emit_block_chunk(const std::string& text, const StreamCallbacks& callbacks) {
    // ✅ 1. 动态抑制检测 (对齐 OpenClaw:478)
    if (streaming_state_.suppress_block_chunks) {
        return;
    }
    
    // ✅ 2. 过滤和修剪 (对齐 OpenClaw:483)
    std::string chunk = strip_thinking_tags(text);
    chunk = trim_end(chunk);
    if (chunk.empty()) {
        return;
    }
    
    // ✅ 3. 智能去重 (对齐 OpenClaw:487-489)
    if (chunk == streaming_state_.last_block_reply_text) {
        return;  // 与上次发送内容完全相同，跳过
    }
    
    // ✅ 4. Messaging Tool 冲突检测 (对齐 OpenClaw:493-497)
    std::string normalized_chunk = normalize_text_for_comparison(chunk);
    if (is_messaging_tool_duplicate(normalized_chunk)) {
        // log.debug("Skipping block reply - already sent via messaging tool");
        return;
    }
    
    // ✅ 5. 记录发送内容 (对齐 OpenClaw:503)
    streaming_state_.last_block_reply_text = chunk;
    
    // ✅ 6. 零开销检测 (对齐 OpenClaw:506-507)
    if (!callbacks.on_block_reply) {
        return;
    }
    
    // ✅ 7. 发送回调
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
    params.model_id = "claude-3-5-sonnet-20241022";  // TODO: 从配置读取
    
    // TODO: 设置 API key 和回调
    // params.api_key = get_api_key();
    // params.memory_flush_callback = ...
    // params.post_compaction_sync_callback = ...
    
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
    
    return result;
}

} // namespace react
} // namespace hunterclaw
