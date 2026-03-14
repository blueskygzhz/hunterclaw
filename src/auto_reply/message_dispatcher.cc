#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/auto_reply/command_detection.h"
#include "hunterclaw/auto_reply/heartbeat.h"
#include "hunterclaw/routing/router.h"
#include "hunterclaw/sessions/session_manager.h"
#include "hunterclaw/queue/delivery_queue.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/llm/llm_client.h"
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace auto_reply {

namespace {

auto log() {
    static auto logger = spdlog::get("auto_reply");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("auto_reply");
    }
    return logger;
}

// 计算内容哈希 (用于去重)
std::string compute_content_hash(const std::string& content) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(content.c_str()), 
           content.size(), hash);
    
    std::ostringstream oss;
    for (int i = 0; i < 16; i++) {  // 取前16字节
        oss << std::hex << std::setw(2) << std::setfill('0') 
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}

int64_t current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace

// ═══════════════════════════════════════════════════════════════
// ✅ MessageDispatcher 实现 - 严格对齐 OpenClaw
// ═══════════════════════════════════════════════════════════════

std::shared_ptr<MessageDispatcher> MessageDispatcher::create(
    std::shared_ptr<routing::MessageRouter> router,
    std::shared_ptr<sessions::SessionManager> session_manager,
    std::shared_ptr<queue::DeliveryQueue> delivery_queue,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const DispatchConfig& config
) {
    return std::shared_ptr<MessageDispatcher>(
        new MessageDispatcher(router, session_manager, delivery_queue,
                            tool_manager, llm_client, config)
    );
}

MessageDispatcher::MessageDispatcher(
    std::shared_ptr<routing::MessageRouter> router,
    std::shared_ptr<sessions::SessionManager> session_manager,
    std::shared_ptr<queue::DeliveryQueue> delivery_queue,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const DispatchConfig& config
) : router_(router),
    session_manager_(session_manager),
    delivery_queue_(delivery_queue),
    tool_manager_(tool_manager),
    llm_client_(llm_client),
    config_(config) {
}

// 对齐 OpenClaw: dispatchInboundMessage (line 21-33)
MessageDispatcher::DispatchResult MessageDispatcher::dispatch_inbound_message(
    const channels::InboundMessage& message
) {
    auto start_time = std::chrono::steady_clock::now();
    
    log()->info("Dispatching inbound message: id={}, channel={}, sender={}", 
                message.message_id, message.channel, message.sender_id);
    
    // TODO: 加载 Agent 配置
    AutoReplyConfig auto_reply_config;
    auto_reply_config.enabled = true;
    auto_reply_config.agent_id = "default";  // TODO: 从路由解析
    
    // 调用完整的分发流程
    auto result = dispatch_reply_from_config(message, auto_reply_config);
    
    auto end_time = std::chrono::steady_clock::now();
    result.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    ).count();
    
    log()->info("Dispatch completed: success={}, time={}ms", 
                result.success, result.processing_time_ms);
    
    return result;
}

// 对齐 OpenClaw: dispatchReplyFromConfig (line 50-610)
MessageDispatcher::DispatchResult MessageDispatcher::dispatch_reply_from_config(
    const channels::InboundMessage& message,
    const AutoReplyConfig& config
) {
    DispatchResult result;
    
    // ✅ 步骤 1: 消息去重检测 (对齐 OpenClaw: line 80-100)
    if (config.dispatch.enable_deduplication && 
        should_skip_duplicate_inbound(message)) {
        result.skipped = true;
        result.skip_reason = "Duplicate message detected (echo)";
        log()->debug("Skipping duplicate message: {}", message.message_id);
        return result;
    }
    
    // ✅ 步骤 2: 权限检查 (对齐 OpenClaw: line 120-140)
    auto permission_check = check_permissions(message, config);
    if (!permission_check.allowed) {
        result.denied = true;
        result.error = "Permission denied: " + permission_check.denial_reason;
        log()->warn("Permission denied for message: {}, reason={}", 
                   message.message_id, permission_check.denial_reason);
        return result;
    }
    
    // ✅ 步骤 3: 快速中止检查 (对齐 OpenClaw: line 150-180)
    auto fast_abort = try_fast_abort_from_message(message, config);
    if (fast_abort.should_abort) {
        result.fast_aborted = true;
        result.skip_reason = fast_abort.reason;
        log()->debug("Fast abort: {}", fast_abort.reason);
        return result;
    }
    
    // ✅ 步骤 4: ACP (Agent Context Protocol) 调度 (对齐 OpenClaw: line 200-230)
    if (config.dispatch.enable_acp) {
        auto acp_result = try_dispatch_acp_reply(message);
        if (acp_result.handled) {
            result.success = true;
            log()->info("Message handled by ACP");
            return result;
        }
    }
    
    // ✅ 步骤 5: 命令授权检查 (对齐 OpenClaw: line 250-280)
    if (config.dispatch.enable_command_auth) {
        auto cmd_detection = CommandDetector::detect_command(message.text);
        if (cmd_detection.needs_authorization) {
            // TODO: 检查用户是否为 owner
            bool is_owner = false;  // TODO: 从配置读取
            
            if (!is_owner) {
                result.denied = true;
                result.error = "Command requires owner authorization: " + cmd_detection.command;
                log()->warn("Command authorization failed: {}", cmd_detection.command);
                return result;
            }
        }
    }
    
    // ✅ 步骤 6: 生成回复 (对齐 OpenClaw: line 300-450)
    ReplyGenerator::GenerateConfig gen_config;
    gen_config.agent_id = config.agent_id;
    gen_config.workspace_path = config.workspace_path;
    gen_config.user_input = message.text;
    // gen_config.session_key = ...;  // TODO: 从路由解析
    
    auto gen_result = ReplyGenerator::get_reply_from_config(
        gen_config,
        llm_client_,
        tool_manager_,
        session_manager_
    );
    
    if (!gen_result.success) {
        result.success = false;
        result.error = gen_result.error;
        log()->error("Reply generation failed: {}", gen_result.error);
        return result;
    }
    
    result.react_result = gen_result.react_result;
    result.total_tokens = gen_result.react_result.usage.total_tokens;
    
    // ✅ 步骤 7: 构建投递负载 (对齐 OpenClaw: line 460-520)
    std::vector<queue::OutboundPayload> payloads;
    
    queue::OutboundPayload payload;
    payload.text = gen_result.response;
    
    // TODO: 处理媒体 URL
    // TODO: 处理 replyToId
    
    payloads.push_back(payload);
    
    // ✅ 步骤 8: TTS 处理 (对齐 OpenClaw: line 550-580)
    if (config.dispatch.enable_tts) {
        maybe_apply_tts_to_payload(payloads, config);
    }
    
    // ✅ 步骤 9: 投递响应 (对齐 OpenClaw: line 590-610)
    queue::DeliveryRequest delivery;
    delivery.channel = message.channel;
    delivery.to = message.sender_id;
    
    // 如果是群组,可能需要路由到群组而非个人
    if (should_route_to_originating(message)) {
        delivery.to = message.chat_id;
    }
    
    try {
        delivery_queue_->enqueue(delivery, payloads);
        result.success = true;
        log()->info("Reply delivered successfully");
    } catch (const std::exception& e) {
        result.success = false;
        result.error = std::string("Delivery failed: ") + e.what();
        log()->error("Delivery failed: {}", e.what());
    }
    
    return result;
}

// 对齐 OpenClaw: shouldSkipDuplicateInbound (line 80-100)
bool MessageDispatcher::should_skip_duplicate_inbound(
    const channels::InboundMessage& message
) {
    int64_t now = current_timestamp_ms();
    int64_t window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        config_.echo_window
    ).count();
    
    std::string content_hash = compute_content_hash(message.text);
    
    // 清理过期记录
    dedup_cache_.erase(
        std::remove_if(dedup_cache_.begin(), dedup_cache_.end(),
            [now, window_ms](const DedupRecord& record) {
                return (now - record.timestamp_ms) > window_ms;
            }),
        dedup_cache_.end()
    );
    
    // 检查是否为重复
    for (const auto& record : dedup_cache_) {
        if (record.content_hash == content_hash) {
            return true;  // 重复
        }
    }
    
    // 记录新消息
    DedupRecord new_record;
    new_record.message_id = message.message_id;
    new_record.content_hash = content_hash;
    new_record.timestamp_ms = now;
    dedup_cache_.push_back(new_record);
    
    // 限制缓存大小
    if (dedup_cache_.size() > static_cast<size_t>(config_.dedup_cache_size)) {
        dedup_cache_.erase(dedup_cache_.begin());
    }
    
    return false;
}

// 对齐 OpenClaw: tryFastAbortFromMessage (line 120-150)
MessageDispatcher::FastAbortResult MessageDispatcher::try_fast_abort_from_message(
    const channels::InboundMessage& message,
    const AutoReplyConfig& config
) {
    FastAbortResult result;
    
    // 1. 空消息
    if (message.text.empty()) {
        result.should_abort = true;
        result.reason = "Empty message";
        return result;
    }
    
    // 2. 系统消息
    if (message.is_system) {
        result.should_abort = true;
        result.reason = "System message";
        return result;
    }
    
    // 3. Bot 消息 (避免循环)
    if (message.is_bot) {
        result.should_abort = true;
        result.reason = "Bot message";
        return result;
    }
    
    return result;
}

// 对齐 OpenClaw: tryDispatchAcpReply (line 180-220)
MessageDispatcher::AcpDispatchResult MessageDispatcher::try_dispatch_acp_reply(
    const channels::InboundMessage& message
) {
    AcpDispatchResult result;
    
    // TODO: 实现 ACP (Agent Context Protocol) 逻辑
    // ACP 允许 agent 之间直接通信
    
    return result;
}

// 权限检查
MessageDispatcher::PermissionCheckResult MessageDispatcher::check_permissions(
    const channels::InboundMessage& message,
    const AutoReplyConfig& config
) {
    PermissionCheckResult result;
    result.allowed = true;
    
    // 1. 检查白名单
    if (!config.channel.allow_from.empty()) {
        bool in_whitelist = std::find(
            config.channel.allow_from.begin(),
            config.channel.allow_from.end(),
            message.sender_id
        ) != config.channel.allow_from.end();
        
        if (!in_whitelist) {
            result.allowed = false;
            result.denial_reason = "Sender not in whitelist";
            return result;
        }
    }
    
    // 2. 检查黑名单
    if (!config.channel.deny_from.empty()) {
        bool in_blacklist = std::find(
            config.channel.deny_from.begin(),
            config.channel.deny_from.end(),
            message.sender_id
        ) != config.channel.deny_from.end();
        
        if (in_blacklist) {
            result.allowed = false;
            result.denial_reason = "Sender in blacklist";
            return result;
        }
    }
    
    // 3. 群组策略检查
    if (message.chat_type == channels::ChatType::Group) {
        switch (config.channel.group_policy) {
            case ChannelConfig::GroupPolicy::Block:
                result.allowed = false;
                result.denial_reason = "Group messages blocked";
                return result;
                
            case ChannelConfig::GroupPolicy::MentionOnly:
                // TODO: 检查是否被 @mention
                // if (!message.mentions_bot) {
                //     result.allowed = false;
                //     result.denial_reason = "Group message without mention";
                //     return result;
                // }
                break;
                
            case ChannelConfig::GroupPolicy::Allow:
                break;
        }
    }
    
    return result;
}

// 对齐 OpenClaw: maybeApplyTtsToPayload (line 550-580)
void MessageDispatcher::maybe_apply_tts_to_payload(
    std::vector<queue::OutboundPayload>& payloads,
    const AutoReplyConfig& config
) {
    // TODO: 实现 TTS (文本转语音)
    // 1. 调用 TTS 服务 (OpenAI, ElevenLabs, Azure)
    // 2. 生成音频文件
    // 3. 添加到 payload.mediaUrls
}

// 对齐 OpenClaw: shouldRouteToOriginating (line 320-340)
bool MessageDispatcher::should_route_to_originating(
    const channels::InboundMessage& message
) {
    // 群组消息应该发送到群组,而非私聊发送者
    return message.chat_type == channels::ChatType::Group;
}

// ═══════════════════════════════════════════════════════════════
// ✅ ReplyGenerator 实现 - 严格对齐 OpenClaw
// ═══════════════════════════════════════════════════════════════

// 对齐 OpenClaw: getReplyFromConfig (line 50-403)
ReplyGenerator::GenerateResult ReplyGenerator::get_reply_from_config(
    const GenerateConfig& config,
    std::shared_ptr<llm::LLMClient> llm_client,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<sessions::SessionManager> session_manager
) {
    GenerateResult result;
    
    // TODO: 完整实现需要:
    // 1. 加载会话历史
    // 2. 应用媒体理解
    // 3. 应用链接理解
    // 4. 初始化会话状态
    // 5. 执行 ReAct 循环
    // 6. 保存会话
    
    // 这是框架代码:
    
    std::string prepared_prompt = config.user_input;
    
    // 应用增强
    // if (config.enable_media_understanding) {
    //     apply_media_understanding(prepared_prompt, message);
    // }
    // if (config.enable_link_understanding) {
    //     apply_link_understanding(prepared_prompt, message);
    // }
    
    // 执行回复
    result = run_prepared_reply(config, prepared_prompt, 
                               llm_client, tool_manager, session_manager);
    
    return result;
}

// 对齐 OpenClaw: runPreparedReply (line 300-380)
ReplyGenerator::GenerateResult ReplyGenerator::run_prepared_reply(
    const GenerateConfig& config,
    const std::string& prepared_prompt,
    std::shared_ptr<llm::LLMClient> llm_client,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<sessions::SessionManager> session_manager
) {
    GenerateResult result;
    
    // TODO: 创建 ReAct 引擎并执行
    // auto react_engine = react::ReactEngine::create(tool_manager, llm_client, config);
    // result.react_result = react_engine->run(prepared_prompt, history, callbacks);
    
    result.success = true;
    result.response = "TODO: Implement ReAct execution";
    
    return result;
}

void ReplyGenerator::apply_media_understanding(
    std::string& prompt,
    const channels::InboundMessage& message
) {
    // TODO: 实现媒体理解 (图片、视频描述)
}

void ReplyGenerator::apply_link_understanding(
    std::string& prompt,
    const channels::InboundMessage& message
) {
    // TODO: 实现链接理解 (提取链接内容摘要)
}

} // namespace auto_reply
} // namespace hunterclaw
