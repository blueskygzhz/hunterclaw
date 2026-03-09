#include "hunterclaw/react/message_processor.h"
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace hunterclaw {
namespace react {

namespace {

// 计算内容哈希 (用于 echo 检测)
std::string compute_content_hash(const std::string& content) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(content.c_str()), content.size(), hash);
    
    std::ostringstream oss;
    for (int i = 0; i < 8; i++) {  // 只取前8字节
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// 获取当前时间戳
int64_t current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace

// MessageProcessor 实现
std::shared_ptr<MessageProcessor> MessageProcessor::create(
    std::shared_ptr<routing::MessageRouter> router,
    std::shared_ptr<sessions::SessionManager> session_manager,
    std::shared_ptr<queue::DeliveryQueue> delivery_queue,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const MessageProcessorConfig& config
) {
    return std::shared_ptr<MessageProcessor>(
        new MessageProcessor(router, session_manager, delivery_queue, 
                           tool_manager, llm_client, config)
    );
}

MessageProcessor::MessageProcessor(
    std::shared_ptr<routing::MessageRouter> router,
    std::shared_ptr<sessions::SessionManager> session_manager,
    std::shared_ptr<queue::DeliveryQueue> delivery_queue,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const MessageProcessorConfig& config
) : router_(router),
    session_manager_(session_manager),
    delivery_queue_(delivery_queue),
    tool_manager_(tool_manager),
    llm_client_(llm_client),
    config_(config) {
}

ProcessResult MessageProcessor::process_message(
    const channels::InboundMessage& message
) {
    ProcessResult result;
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        // 1. 预处理: 路由解析
        auto context = preprocess_message(message);
        
        // 2. 权限检查
        if (config_.enable_permission_check && !check_permissions(context)) {
            result.success = false;
            result.error = "Permission denied: " + context.denial_reason;
            return result;
        }
        
        // 3. Echo 检测
        if (config_.echo_detection && is_echo(context)) {
            result.success = true;
            result.error = "Echo detected, skipping";
            return result;
        }
        
        // 4. 发送 ACK 反应
        if (config_.send_ack_reaction) {
            send_ack_reaction(context);
        }
        
        // 5. 加载会话和历史
        auto history = load_history(context);
        
        // 6. 执行 ReAct 循环 (带循环检测和模型回退)
        std::vector<std::string> fallback_attempts;
        ReactResult react_result;
        
        if (config_.enable_model_fallback) {
            react_result = execute_with_fallback(context, history, fallback_attempts);
            result.used_fallback = !fallback_attempts.empty();
            result.fallback_attempts = fallback_attempts;
        } else {
            react_result = execute_react_with_loop_detection(context, history);
        }
        
        result.react_result = react_result;
        result.success = react_result.success;
        
        if (!react_result.success) {
            result.error = react_result.error;
            return handle_error(context, react_result.error);
        }
        
        // 7. 投递响应
        deliver_response(context, react_result);
        
        // 8. 保存会话
        save_session(context, react_result);
        
        result.total_tokens = react_result.usage.total_tokens;
        result.success = true;
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error = std::string("Processing error: ") + e.what();
    }
    
    auto end_time = std::chrono::steady_clock::now();
    result.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    ).count();
    
    return result;
}

std::future<ProcessResult> MessageProcessor::process_message_async(
    const channels::InboundMessage& message
) {
    return std::async(std::launch::async, [this, message]() {
        return process_message(message);
    });
}

// 流水线实现
MessageContext MessageProcessor::preprocess_message(
    const channels::InboundMessage& message
) {
    MessageContext context;
    context.message = message;
    
    // 路由解析
    routing::RouteRequest route_req;
    route_req.channel = message.channel;
    route_req.peer.kind = message.chat_type;
    route_req.peer.id = message.sender_id;
    route_req.peer.guild_id = message.guild_id;
    
    auto route_result = router_->resolve_route(route_req);
    if (!route_result.has_value()) {
        throw std::runtime_error("Failed to resolve route");
    }
    
    context.route = route_result.value();
    context.agent_id = context.route.agent_id;
    context.user_id = message.sender_id;
    context.user_name = message.sender_name;
    
    // TODO: 加载 agent 工作空间和权限
    context.workspace_path = "~/.hunterclaw/agents/" + context.agent_id;
    context.is_owner = false;  // TODO: 从配置读取
    context.authorized = true;
    
    return context;
}

bool MessageProcessor::check_permissions(MessageContext& context) {
    // TODO: 实现完整的权限检查
    // - allowFrom (白名单)
    // - denyFrom (黑名单)
    // - groupPolicy (群组策略)
    // - 工具权限 (owner-only tools)
    
    if (!context.authorized) {
        context.denial_reason = "User not authorized";
        return false;
    }
    
    return true;
}

bool MessageProcessor::is_echo(const MessageContext& context) {
    int64_t now = current_timestamp_ms();
    int64_t window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        config_.echo_window
    ).count();
    
    std::string content_hash = compute_content_hash(context.message.text);
    
    // 清理过期记录
    echo_cache_.erase(
        std::remove_if(echo_cache_.begin(), echo_cache_.end(),
            [now, window_ms](const EchoRecord& record) {
                return (now - record.timestamp_ms) > window_ms;
            }),
        echo_cache_.end()
    );
    
    // 检查是否为 echo
    for (const auto& record : echo_cache_) {
        if (record.content_hash == content_hash) {
            return true;
        }
    }
    
    // 记录新消息
    EchoRecord new_record;
    new_record.message_id = context.message.message_id;
    new_record.content_hash = content_hash;
    new_record.timestamp_ms = now;
    echo_cache_.push_back(new_record);
    
    return false;
}

void MessageProcessor::send_ack_reaction(const MessageContext& context) {
    // TODO: 实现 ACK 反应发送
    // 发送 emoji 到原消息
    // channels::ReactionRequest req;
    // req.channel = context.message.channel;
    // req.message_id = context.message.message_id;
    // req.emoji = config_.ack_emoji;
    // channel_manager->add_reaction(req);
}

std::vector<llm::Message> MessageProcessor::load_history(const MessageContext& context) {
    std::vector<llm::Message> history;
    
    // 获取会话
    auto session_result = session_manager_->get_or_create_session(
        context.route.session_key,
        context.agent_id,
        sessions::SessionOptions{
            .sender_name = context.user_name
        }
    );
    
    if (!session_result.has_value()) {
        return history;
    }
    
    auto session = session_result.value();
    
    // 加载历史消息
    int max_messages = config_.max_history_messages;
    int start_index = std::max(0, static_cast<int>(session.messages.size()) - max_messages);
    
    for (size_t i = start_index; i < session.messages.size(); i++) {
        llm::Message msg;
        msg.role = session.messages[i].role;
        msg.content = session.messages[i].content;
        history.push_back(msg);
    }
    
    return history;
}

ReactResult MessageProcessor::execute_react_with_loop_detection(
    const MessageContext& context,
    const std::vector<llm::Message>& history
) {
    // 创建 ReAct 引擎
    auto react_engine = ReactEngine::create(
        tool_manager_,
        llm_client_,
        config_.react_config
    );
    
    // 创建循环检测状态
    auto loop_state = LoopDetectionState::create(config_.loop_detection_config);
    
    // 设置流式回调 (集成循环检测)
    StreamCallbacks callbacks;
    
    callbacks.on_tool_start = [loop_state, context](
        const std::string& tool_name,
        const core::JsonValue& args
    ) {
        // 在工具执行前检测循环
        auto loop_result = loop_state->detect_tool_call_loop(tool_name, args);
        
        if (loop_result.stuck && 
            loop_result.level == LoopDetectionResult::Level::Critical) {
            throw std::runtime_error(loop_result.message);
        }
        
        // 记录工具调用
        loop_state->record_tool_call(tool_name, args, "tool_call_" + std::to_string(current_timestamp_ms()));
    };
    
    callbacks.on_tool_end = [loop_state](
        const std::string& tool_name,
        const tools::ToolResult& result
    ) {
        // 记录工具结果
        loop_state->record_tool_call_outcome(tool_name, result);
    };
    
    // 块回复投递
    callbacks.on_block_reply = [this, context](const std::string& block) {
        queue::DeliveryRequest delivery;
        delivery.channel = context.message.channel;
        delivery.to = context.message.sender_id;
        
        queue::OutboundPayload payload;
        payload.text = block;
        
        delivery_queue_->enqueue(delivery, {payload});
    };
    
    // 执行 ReAct
    return react_engine->run(context.message.text, history, callbacks);
}

void MessageProcessor::deliver_response(
    const MessageContext& context,
    const ReactResult& result
) {
    if (result.final_response.empty()) {
        return;
    }
    
    // 创建投递请求
    queue::DeliveryRequest delivery;
    delivery.channel = context.message.channel;
    delivery.to = context.message.sender_id;
    
    // 如果是群组消息，可能需要 @提及
    if (context.message.chat_type == channels::ChatType::Group) {
        delivery.to = context.message.chat_id;  // 发送到群组
    }
    
    queue::OutboundPayload payload;
    payload.text = result.final_response;
    
    // TODO: 处理媒体 URL (图片、视频等)
    
    delivery_queue_->enqueue(delivery, {payload});
}

void MessageProcessor::save_session(
    const MessageContext& context,
    const ReactResult& result
) {
    // 获取会话
    auto session_result = session_manager_->get_or_create_session(
        context.route.session_key,
        context.agent_id,
        sessions::SessionOptions{
            .sender_name = context.user_name
        }
    );
    
    if (!session_result.has_value()) {
        return;
    }
    
    auto session = session_result.value();
    
    // 添加用户消息
    sessions::SessionMessage user_msg;
    user_msg.role = "user";
    user_msg.content = context.message.text;
    user_msg.timestamp_ms = current_timestamp_ms();
    session.messages.push_back(user_msg);
    
    // 添加助手响应
    sessions::SessionMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = result.final_response;
    assistant_msg.timestamp_ms = current_timestamp_ms();
    session.messages.push_back(assistant_msg);
    
    // 更新 token 统计
    session.token_usage.input_tokens += result.usage.input_tokens;
    session.token_usage.output_tokens += result.usage.output_tokens;
    session.token_usage.total_tokens += result.usage.total_tokens;
    
    // 保存会话
    session_manager_->update_session(session);
}

ProcessResult MessageProcessor::handle_error(
    const MessageContext& context,
    const std::string& error
) {
    ProcessResult result;
    result.success = false;
    result.error = error;
    
    // 发送错误消息给用户
    queue::DeliveryRequest delivery;
    delivery.channel = context.message.channel;
    delivery.to = context.message.sender_id;
    
    queue::OutboundPayload payload;
    payload.text = "Sorry, I encountered an error: " + error;
    
    delivery_queue_->enqueue(delivery, {payload});
    
    return result;
}

ReactResult MessageProcessor::execute_with_fallback(
    const MessageContext& context,
    const std::vector<llm::Message>& history,
    std::vector<std::string>& fallback_attempts
) {
    // 首先尝试主模型
    try {
        return execute_react_with_loop_detection(context, history);
    } catch (const std::exception& e) {
        fallback_attempts.push_back("Primary model failed: " + std::string(e.what()));
    }
    
    // 尝试回退模型
    for (const auto& fallback_model : config_.fallback_models) {
        try {
            // TODO: 切换 LLM 客户端的模型
            // llm_client_->set_model(fallback_model);
            
            auto result = execute_react_with_loop_detection(context, history);
            fallback_attempts.push_back("Fallback to " + fallback_model + " succeeded");
            return result;
            
        } catch (const std::exception& e) {
            fallback_attempts.push_back("Fallback to " + fallback_model + " failed: " + e.what());
        }
    }
    
    // 所有模型都失败
    ReactResult result;
    result.success = false;
    result.error = "All models failed. Attempts: " + std::to_string(fallback_attempts.size());
    return result;
}

bool MessageProcessor::try_auto_compaction(MessageContext& context) {
    // TODO: 实现自动压缩
    // 当会话 token 数超过阈值时，压缩旧消息
    return false;
}

} // namespace react
} // namespace hunterclaw
