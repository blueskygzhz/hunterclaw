#include "hunterclaw/react/message_processor.h"
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <filesystem>
#include <cstdlib>
#include "spdlog/spdlog.h"

namespace hunterclaw {
namespace react {

namespace {

// 🎯 OpenClaw 对齐: 格式化推理消息
// 参考: openclaw-analysis/src/agents/pi-embedded-utils.ts:formatReasoningMessage
std::string format_reasoning_message(const std::string& text) {
    // 修剪空白
    std::string trimmed = text;
    // 去除首尾空白
    auto start = trimmed.find_first_not_of(" \t\n\r");
    auto end = trimmed.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";  // 全是空白
    }
    trimmed = trimmed.substr(start, end - start + 1);
    
    if (trimmed.empty()) {
        return "";
    }
    
    // 将每行非空内容用下划线包裹 (Markdown 斜体)
    // 注意: Telegram 的下划线 markdown 无法跨行，所以每行单独包裹
    std::stringstream ss(trimmed);
    std::string line;
    std::vector<std::string> formatted_lines;
    
    while (std::getline(ss, line)) {
        if (!line.empty()) {
            formatted_lines.push_back("_" + line + "_");
        } else {
            formatted_lines.push_back("");  // 保留空行
        }
    }
    
    // 拼接结果
    std::string result = "Reasoning:\n";
    for (size_t i = 0; i < formatted_lines.size(); ++i) {
        result += formatted_lines[i];
        if (i < formatted_lines.size() - 1) {
            result += "\n";
        }
    }
    
    return result;
}

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
        // ✅ OpenClaw 对齐: 根据渠道动态控制 block streaming
        // 参考: openclaw-analysis/src/web/auto-reply/monitor/process-message.ts:455
        // 
        // 渠道策略:
        // - WhatsApp, Signal, iMessage: 禁用 (不支持消息更新)
        // - Telegram, Discord, Slack: 启用 (支持消息编辑)
        // - Web UI: 启用 (WebSocket 推送)
        bool is_streaming_channel = 
            (message.channel == "telegram") ||
            (message.channel == "discord") ||
            (message.channel == "slack") ||
            (message.channel == "websocket");
        
        if (!is_streaming_channel && config_.react_config.block_reply.enabled) {
            // 对于非 streaming 渠道，自动禁用 block streaming
            config_.react_config.block_reply.disable_streaming = true;
            spdlog::debug("[MessageProcessor] Block streaming disabled for channel: {}",
                message.channel);
        }
        
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
    
    // ========== 加载 Agent 工作空间和权限 (对齐 OpenClaw) ==========
    // 参考: openclaw-analysis/src/agents/agent-scope.ts
    
    // 1. 加载 Agent 配置
    auto agent_result = session_manager_->get_agent_config(context.agent_id);
    if (!agent_result.has_value()) {
        throw std::runtime_error("Agent config not found: " + context.agent_id);
    }
    
    auto agent_config = agent_result.value();
    
    // 2. 构建工作空间路径
    // 格式: {workspace_root}/{agent_id}
    if (!agent_config.workspace.empty()) {
        context.workspace_path = agent_config.workspace;
    } else {
        // 默认路径: ~/.hunterclaw/agents/{agent_id}
        std::string home = std::getenv("HOME") ? std::getenv("HOME") : "/tmp";
        context.workspace_path = home + "/.hunterclaw/agents/" + context.agent_id;
    }
    
    // 3. 判断是否为 owner (从配置读取)
    // owner 的判断逻辑:
    // - 配置文件指定的 owner_id
    // - 或者 agent 配置中的 creator
    std::string system_owner_id = "";  // TODO: 从全局配置读取
    
    context.is_owner = (context.user_id == system_owner_id) ||
                       (context.user_id == agent_config.creator);
    
    // 4. 默认授权 (具体权限检查在 check_permissions 中完成)
    context.authorized = true;
    
    return context;
}

bool MessageProcessor::check_permissions(MessageContext& context) {
    // ========== 完整权限检查 (对齐 OpenClaw) ==========
    // 参考: openclaw-analysis/src/auto-reply/permissions/permission-check.ts
    
    // 1. 基础授权检查
    if (!context.authorized) {
        context.denial_reason = "User not authorized";
        return false;
    }
    
    // 2. 加载 Agent 配置的权限策略
    auto agent_result = session_manager_->get_agent_config(context.agent_id);
    if (!agent_result.has_value()) {
        context.denial_reason = "Agent config not found";
        return false;
    }
    
    auto agent_config = agent_result.value();
    
    // 3. 白名单检查 (allowFrom)
    // 如果配置了白名单,用户必须在白名单中
    if (!agent_config.permissions.allow_from.empty()) {
        bool in_whitelist = false;
        
        for (const auto& allowed_id : agent_config.permissions.allow_from) {
            // 支持通配符 "*" (允许所有)
            if (allowed_id == "*") {
                in_whitelist = true;
                break;
            }
            
            // 完全匹配
            if (allowed_id == context.user_id) {
                in_whitelist = true;
                break;
            }
            
            // 前缀匹配 (例如: "user_*" 匹配 "user_123")
            if (allowed_id.back() == '*') {
                std::string prefix = allowed_id.substr(0, allowed_id.size() - 1);
                if (context.user_id.substr(0, prefix.size()) == prefix) {
                    in_whitelist = true;
                    break;
                }
            }
        }
        
        if (!in_whitelist) {
            context.denial_reason = "User not in allowFrom whitelist";
            return false;
        }
    }
    
    // 4. 黑名单检查 (denyFrom)
    for (const auto& denied_id : agent_config.permissions.deny_from) {
        if (denied_id == context.user_id) {
            context.denial_reason = "User in denyFrom blacklist";
            return false;
        }
        
        // 前缀匹配黑名单
        if (denied_id.back() == '*') {
            std::string prefix = denied_id.substr(0, denied_id.size() - 1);
            if (context.user_id.substr(0, prefix.size()) == prefix) {
                context.denial_reason = "User matches denyFrom pattern: " + denied_id;
                return false;
            }
        }
    }
    
    // 5. 群组策略检查 (groupPolicy)
    if (context.message.chat_type == channels::ChatType::Group) {
        std::string group_policy = agent_config.permissions.group_policy;
        
        if (group_policy == "deny") {
            // 拒绝所有群组消息
            context.denial_reason = "Group messages denied by policy";
            return false;
        } else if (group_policy == "mention_only") {
            // 只响应 @提及 (需要检查消息中是否包含 bot mention)
            // TODO: 从 InboundMessage 获取 mentions
            bool is_mentioned = false;  // context.message.mentions.contains(bot_id)
            
            if (!is_mentioned) {
                context.denial_reason = "Group messages require @mention";
                return false;
            }
        }
        // "allow" 策略: 允许所有群组消息 (默认行为)
    }
    
    // 6. Owner-only 工具权限检查
    // 在实际工具调用时再检查 (MessageProcessor 这里只做基础权限)
    // 参考: tools::ToolManager::check_tool_permission()
    
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
    // ========== 发送 ACK 反应 (对齐 OpenClaw) ==========
    // 参考: openclaw-analysis/src/auto-reply/reply/ack-reaction.ts
    
    try {
        // 构造 ACK 反应请求
        queue::DeliveryRequest delivery;
        delivery.channel = context.message.channel;
        delivery.to = context.message.chat_id;  // 发送到对话
        
        // 构造反应 payload
        queue::OutboundPayload payload;
        payload.type = queue::OutboundPayload::Type::Reaction;  // 反应类型
        payload.reaction_emoji = config_.ack_emoji;  // 👀 或其他 emoji
        payload.target_message_id = context.message.message_id;  // 目标消息
        
        // 投递到队列 (立即发送,低优先级)
        delivery_queue_->enqueue(delivery, {payload}, queue::Priority::Low);
        
        spdlog::debug("[MessageProcessor] ACK reaction sent: {} to message {}",
            config_.ack_emoji, context.message.message_id);
        
    } catch (const std::exception& e) {
        // ACK 发送失败不影响主流程
        spdlog::warn("[MessageProcessor] Failed to send ACK reaction: {}", e.what());
    }
}

std::vector<llm::LLMMessage> MessageProcessor::load_history(const MessageContext& context) {
    std::vector<llm::LLMMessage> history;
    
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
        llm::LLMMessage msg;
        msg.role = session.messages[i].role;
        msg.content = session.messages[i].content;
        history.push_back(msg);
    }
    
    return history;
}

ReactResult MessageProcessor::execute_react_with_loop_detection(
    const MessageContext& context,
    const std::vector<llm::LLMMessage>& history
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
    
    // ✅ 严格对齐 OpenClaw: 配置推理流回调 (thinking 输出)
    // 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts:556-586
    callbacks.on_reasoning_stream = [this, context](const std::string& reasoning) {
        // 1. 检查模式
        if (config_.reasoning_mode != ReactConfig::ReasoningMode::Stream) {
            return;
        }
        
        // 2. 格式化 (OpenClaw 风格)
        std::string formatted = format_reasoning_message(reasoning);
        if (formatted.empty()) {
            return;
        }
        
        // 3. 获取 session 的上次推理
        std::string session_key = context.message.sender_id + ":" + context.message.chat_id;
        std::string& last_reasoning = last_reasoning_by_session_[session_key];
        
        // 4. 去重检查
        if (formatted == last_reasoning) {
            return;
        }
        
        // 5. 计算增量
        std::string delta;
        if (!last_reasoning.empty() && formatted.find(last_reasoning) == 0) {
            // 新文本是旧文本的前缀扩展 → 只发送增量
            delta = formatted.substr(last_reasoning.length());
        } else {
            // 非前缀或首次 → 发送全量
            delta = formatted;
        }
        
        last_reasoning = formatted;
        
        // 6. 投递到队列 (只发送增量!)
        if (!delta.empty()) {
            queue::DeliveryRequest delivery;
            delivery.channel = context.message.channel;
            delivery.to = context.message.sender_id;
            
            // 如果是群组消息，发送到群组
            if (context.message.chat_type == channels::ChatType::Group) {
                delivery.to = context.message.chat_id;
            }
            
            queue::OutboundPayload payload;
            payload.text = delta;  // ✅ 只发送增量!
            payload.metadata["type"] = "reasoning_stream";
            payload.metadata["is_delta"] = "true";
            
            delivery_queue_->enqueue(delivery, {payload});
        }
    };
    
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
    // ✅ OpenClaw 对齐: 根据渠道动态控制 block streaming
    // 参考: openclaw-analysis/src/web/auto-reply/monitor/process-message.ts:405-410
    callbacks.on_block_reply = [this, context](const std::string& block) {
        // 1. 检查是否禁用 block streaming
        // 用于 WhatsApp, Signal 等不支持消息更新的渠道
        if (config_.react_config.block_reply.disable_streaming) {
            spdlog::trace("[MessageProcessor] Block reply suppressed: disable_streaming=true");
            return;
        }
        
        // 2. 检查是否启用 block reply
        if (!config_.react_config.block_reply.enabled) {
            spdlog::trace("[MessageProcessor] Block reply disabled in config");
            return;
        }
        
        // 3. 正常投递 block reply
        queue::DeliveryRequest delivery;
        delivery.channel = context.message.channel;
        delivery.to = context.message.sender_id;
        
        queue::OutboundPayload payload;
        payload.text = block;
        
        delivery_queue_->enqueue(delivery, {payload});
        spdlog::debug("[MessageProcessor] Block reply delivered: {} chars", block.length());
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
    
    // ========== 处理媒体 URL (对齐 OpenClaw) ==========
    // 参考: openclaw-analysis/src/auto-reply/reply/media-extraction.ts
    
    // 1. 从 ReactResult 提取媒体附件
    // 媒体来源:
    // - 工具调用返回的图片 URL (例如: image_gen, web_screenshot)
    // - Markdown 中的图片 URL (![alt](url))
    // - 视频 URL (.mp4, .webm 等)
    
    std::vector<queue::MediaAttachment> media_attachments;
    
    // 解析 Markdown 图片语法: ![alt](url)
    std::regex img_regex(R"(\!\[.*?\]\((https?://[^\)]+)\))");
    std::smatch match;
    std::string text = result.final_response;
    
    auto search_start = text.cbegin();
    while (std::regex_search(search_start, text.cend(), match, img_regex)) {
        std::string url = match[1].str();
        
        queue::MediaAttachment media;
        media.type = queue::MediaAttachment::Type::Image;
        media.url = url;
        media_attachments.push_back(media);
        
        search_start = match.suffix().first;
    }
    
    // 2. 从 ReactResult metadata 提取媒体 (工具返回的媒体)
    if (result.metadata.contains("media") && result.metadata["media"].is_array()) {
        for (const auto& media_json : result.metadata["media"]) {
            queue::MediaAttachment media;
            
            std::string type_str = media_json.value("type", "image");
            if (type_str == "image") {
                media.type = queue::MediaAttachment::Type::Image;
            } else if (type_str == "video") {
                media.type = queue::MediaAttachment::Type::Video;
            } else if (type_str == "audio") {
                media.type = queue::MediaAttachment::Type::Audio;
            } else if (type_str == "file") {
                media.type = queue::MediaAttachment::Type::File;
            }
            
            media.url = media_json.value("url", "");
            media.mime_type = media_json.value("mime_type", "");
            
            if (!media.url.empty()) {
                media_attachments.push_back(media);
            }
        }
    }
    
    // 3. 添加媒体附件到 payload
    if (!media_attachments.empty()) {
        payload.media = media_attachments;
        
        spdlog::info("[MessageProcessor] Delivering {} media attachments",
            media_attachments.size());
    }
    
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
    const std::vector<llm::LLMMessage>& history,
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
            // ========== 切换 LLM 客户端的模型 (对齐 OpenClaw) ==========
            // 参考: openclaw-analysis/src/llm/client-fallback.ts
            
            spdlog::info("[MessageProcessor] Attempting fallback to model: {}", fallback_model);
            
            // 方案 1: 动态切换模型 (如果 LLMClient 支持)
            if (llm_client_->supports_model_switch()) {
                llm_client_->set_model(fallback_model);
            } else {
                // 方案 2: 创建新的 LLMClient (回退模型)
                // 复制原有配置但替换模型名
                auto fallback_client = llm_client_->clone_with_model(fallback_model);
                if (!fallback_client) {
                    throw std::runtime_error("Failed to create fallback client");
                }
                
                // 临时替换 LLMClient
                auto original_client = llm_client_;
                llm_client_ = fallback_client;
                
                auto result = execute_react_with_loop_detection(context, history);
                
                // 恢复原 LLMClient
                llm_client_ = original_client;
                
                fallback_attempts.push_back("Fallback to " + fallback_model + " succeeded");
                return result;
            }
            
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
    // ========== 自动会话压缩 (对齐 OpenClaw) ==========
    // 参考: openclaw-analysis/src/sessions/compaction.ts
    
    try {
        // 1. 获取当前会话
        auto session_result = session_manager_->get_or_create_session(
            context.route.session_key,
            context.agent_id,
            sessions::SessionOptions{.sender_name = context.user_name}
        );
        
        if (!session_result.has_value()) {
            return false;
        }
        
        auto session = session_result.value();
        
        // 2. 检查是否需要压缩
        int64_t total_tokens = session.token_usage.total_tokens;
        
        if (total_tokens < config_.compaction_threshold_tokens) {
            return false;  // 未达到阈值,不需要压缩
        }
        
        spdlog::info("[MessageProcessor] Auto-compaction triggered: session={}, tokens={}/{}",
            context.route.session_key, total_tokens, config_.compaction_threshold_tokens);
        
        // 3. 压缩策略: 保留最近 N 条消息 + 生成摘要
        int keep_recent_messages = 10;  // 保留最近 10 条
        int compress_count = std::max(0, 
            static_cast<int>(session.messages.size()) - keep_recent_messages
        );
        
        if (compress_count < 5) {
            return false;  // 消息太少,不值得压缩
        }
        
        // 4. 提取要压缩的消息
        std::vector<sessions::SessionMessage> to_compress(
            session.messages.begin(),
            session.messages.begin() + compress_count
        );
        
        // 5. 生成摘要 (调用 LLM 压缩)
        std::string summary_prompt = "Please summarize the following conversation concisely:\n\n";
        for (const auto& msg : to_compress) {
            summary_prompt += msg.role + ": " + msg.content + "\n";
        }
        
        // TODO: 调用 LLM 生成摘要
        // auto summary_result = llm_client_->complete(summary_prompt);
        std::string summary = "[Summary of previous " + 
            std::to_string(compress_count) + " messages]";
        
        // 6. 创建摘要消息
        sessions::SessionMessage summary_msg;
        summary_msg.role = "system";
        summary_msg.content = summary;
        summary_msg.timestamp_ms = to_compress.back().timestamp_ms;
        
        // 7. 替换旧消息为摘要
        session.messages.erase(
            session.messages.begin(),
            session.messages.begin() + compress_count
        );
        session.messages.insert(session.messages.begin(), summary_msg);
        
        // 8. 更新会话
        session_manager_->update_session(session);
        
        spdlog::info("[MessageProcessor] Compaction complete: compressed {} messages → 1 summary",
            compress_count);
        
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("[MessageProcessor] Compaction failed: {}", e.what());
        return false;
    }
}

} // namespace react
} // namespace hunterclaw
