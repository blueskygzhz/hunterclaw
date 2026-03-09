#include "hunterclaw/react/react_engine.h"
#include <regex>
#include <algorithm>
#include <future>
#include <thread>

namespace hunterclaw {
namespace react {

namespace {

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

ReactEngine::ReactEngine(
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const ReactConfig& config
) : tool_manager_(tool_manager),
    llm_client_(llm_client),
    config_(config) {
}

ReactResult ReactEngine::run(
    const std::string& user_input,
    const std::vector<llm::Message>& history,
    const StreamCallbacks& callbacks
) {
    cancelled_ = false;
    start_time_ = std::chrono::steady_clock::now();
    
    // 生命周期: start
    if (callbacks.on_start) {
        callbacks.on_start();
    }
    
    try {
        auto result = execute_loop(user_input, history, callbacks);
        
        // 生命周期: end
        if (callbacks.on_end) {
            callbacks.on_end();
        }
        
        return result;
    } catch (const std::exception& e) {
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
    const std::vector<llm::Message>& history,
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
    const std::vector<llm::Message>& history,
    const StreamCallbacks& callbacks
) {
    ReactResult result;
    result.success = true;
    
    // 构建消息列表
    std::vector<llm::Message> messages = history;
    
    // 添加用户输入 (Observe)
    llm::Message user_msg;
    user_msg.role = "user";
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
        result.usage.input_tokens += llm_response.usage.input_tokens;
        result.usage.output_tokens += llm_response.usage.output_tokens;
        result.usage.total_tokens += llm_response.usage.total_tokens;
        result.usage.cache_read_tokens += llm_response.usage.cache_read_tokens;
        result.usage.cache_write_tokens += llm_response.usage.cache_write_tokens;
        
        // 记录推理步骤
        ReactStep reason_step;
        reason_step.type = StepType::Reason;
        reason_step.content = llm_response.text;
        reason_step.reasoning = llm_response.reasoning;
        reason_step.has_reasoning = !llm_response.reasoning.empty();
        reason_step.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        
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
                reason_step.tool_calls.push_back(tc.function.name);
            }
            reason_step.tool_results = tool_results;
            
            // 添加助手消息 (带工具调用)
            llm::Message assistant_msg;
            assistant_msg.role = "assistant";
            assistant_msg.content = llm_response.text;
            assistant_msg.tool_calls = llm_response.tool_calls;
            messages.push_back(assistant_msg);
            
            // 添加工具结果消息
            for (const auto& [tool_call_id, tool_result] : tool_results) {
                llm::Message tool_msg;
                tool_msg.role = "tool";
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
        accumulated_response += llm_response.text;
        result.final_response = accumulated_response;
        
        // 发送最终块回复
        if (config_.block_reply.enabled && callbacks.on_block_reply) {
            callbacks.on_block_reply(accumulated_response);
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
    
    return result;
}

ReactEngine::LLMResponse ReactEngine::call_llm(
    const std::vector<llm::Message>& messages,
    const StreamCallbacks& callbacks
) {
    LLMResponse response;
    
    // 获取工具列表
    auto llm_tools = tool_manager_->to_llm_format();
    
    llm::ChatRequest request;
    request.messages = messages;
    request.stream = (callbacks.on_assistant_delta != nullptr);
    
    // 转换工具定义
    for (const auto& llm_tool : llm_tools) {
        llm::Tool tool;
        tool.type = "function";
        tool.function.name = llm_tool.name;
        tool.function.description = llm_tool.description;
        tool.function.parameters = llm_tool.parameters;  // JSON string
        request.tools.push_back(tool);
    }
    
    if (request.stream) {
        // 流式调用
        std::string accumulated_text;
        std::string accumulated_reasoning;
        bool in_thinking = false;
        
        llm_client_->stream_chat(request, [&](const llm::ChatStreamChunk& chunk) {
            if (!chunk.choices.empty()) {
                const auto& choice = chunk.choices[0];
                const auto& delta = choice.delta;
                
                if (!delta.content.empty()) {
                    // 提取推理内容
                    std::string new_text = delta.content;
                    
                    if (config_.reasoning_mode == ReactConfig::ReasoningMode::Stream) {
                        // 检测 thinking tags
                        std::smatch match;
                        if (std::regex_search(new_text, match, thinking_tag_re)) {
                            bool is_closing = match[1].matched && match[1].str() == "/";
                            if (!is_closing) {
                                in_thinking = true;
                            } else {
                                in_thinking = false;
                            }
                        }
                        
                        if (in_thinking) {
                            accumulated_reasoning += new_text;
                            if (callbacks.on_reasoning_stream) {
                                callbacks.on_reasoning_stream(new_text);
                            }
                        } else {
                            accumulated_text += new_text;
                            if (callbacks.on_assistant_delta) {
                                callbacks.on_assistant_delta(new_text);
                            }
                        }
                    } else {
                        accumulated_text += new_text;
                        if (callbacks.on_assistant_delta) {
                            callbacks.on_assistant_delta(new_text);
                        }
                    }
                }
                
                // 工具调用
                if (!delta.tool_calls.empty()) {
                    response.tool_calls.insert(
                        response.tool_calls.end(),
                        delta.tool_calls.begin(),
                        delta.tool_calls.end()
                    );
                }
            }
            
            // Usage 信息
            if (chunk.usage.total_tokens > 0) {
                response.usage = chunk.usage;
            }
        });
        
        response.text = accumulated_text;
        response.reasoning = accumulated_reasoning;
        
    } else {
        // 非流式调用
        auto chat_response = llm_client_->chat(request);
        
        if (!chat_response.choices.empty()) {
            const auto& choice = chat_response.choices[0];
            const auto& message = choice.message;
            
            response.text = message.content;
            response.tool_calls = message.tool_calls;
            
            // 提取推理
            auto extraction = extract_reasoning(response.text);
            if (extraction.has_reasoning) {
                response.reasoning = extraction.reasoning;
                response.text = extraction.remaining_text;
            }
        }
        
        response.usage = chat_response.usage;
    }
    
    return response;
}

std::map<std::string, tools::ToolResult> ReactEngine::execute_tools(
    const std::vector<llm::ToolCall>& tool_calls,
    const StreamCallbacks& callbacks
) {
    std::map<std::string, tools::ToolResult> results;
    
    if (config_.parallel_tool_calls) {
        // 并行执行 (TODO: 实现线程池)
        for (const auto& tool_call : tool_calls) {
            if (callbacks.on_tool_start) {
                callbacks.on_tool_start(tool_call.function.name, 
                                       core::JsonValue{});  // TODO: parse arguments
            }
            
            // TODO: 解析 tool_call.function.arguments (JSON string) 为 JsonValue
            core::JsonValue args;
            auto result = tool_manager_->execute_tool(tool_call.function.name, args);
            
            if (callbacks.on_tool_end) {
                callbacks.on_tool_end(tool_call.function.name, result);
            }
            
            results[tool_call.id] = result;
        }
    } else {
        // 串行执行
        for (const auto& tool_call : tool_calls) {
            if (cancelled_) break;
            
            if (callbacks.on_tool_start) {
                callbacks.on_tool_start(tool_call.function.name, 
                                       core::JsonValue{});  // TODO: parse arguments
            }
            
            // TODO: 解析 tool_call.function.arguments (JSON string) 为 JsonValue
            core::JsonValue args;
            auto result = tool_manager_->execute_tool(tool_call.function.name, args);
            
            if (callbacks.on_tool_end) {
                callbacks.on_tool_end(tool_call.function.name, result);
            }
            
            results[tool_call.id] = result;
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

} // namespace react
} // namespace hunterclaw
