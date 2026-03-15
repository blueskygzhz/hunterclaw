#include "hunterclaw/auto_reply/delta_trigger.h"
#include "hunterclaw/memory/session_memory_provider.h"
#include "hunterclaw/core/logging.h"
#include <chrono>

namespace hunterclaw {
namespace auto_reply {

// ========================================
// DeltaTrigger Implementation
// 严格对齐 OpenClaw 的 delta 触发机制
// 参考: src/memory/manager-sync-ops.ts::updateSessionDelta
// ========================================

DeltaTrigger::DeltaTrigger(
    std::shared_ptr<memory::SessionMemoryProvider> session_provider
) : session_provider_(session_provider),
    logger_(core::Logger::get()) {}

core::Result<void> DeltaTrigger::trigger_on_message_batch(
    const std::string& session_key,
    const std::vector<Message>& messages
) {
    if (!session_provider_ || !session_provider_->config().enabled) {
        // Session memory 未启用,静默跳过
        return core::Ok();
    }
    
    logger_->debug("Delta trigger: processing {} messages for session {}",
                   messages.size(), session_key);
    
    // 转换为 SessionMessage 格式
    std::vector<memory::SessionMessage> session_messages;
    session_messages.reserve(messages.size());
    
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    
    for (const auto& msg : messages) {
        memory::SessionMessage session_msg;
        session_msg.timestamp = timestamp;
        session_msg.role = msg.role;
        session_msg.content = msg.content;  // Can be string or json
        session_msg.agent_id = msg.agent_id.value_or("");
        
        session_messages.push_back(session_msg);
    }
    
    // 触发 delta 更新 (对齐 OpenClaw)
    return session_provider_->trigger_delta_update(session_key, session_messages);
}

core::Result<void> DeltaTrigger::trigger_on_single_message(
    const std::string& session_key,
    const std::string& role,
    const json& content,
    const std::optional<std::string>& agent_id
) {
    Message msg;
    msg.role = role;
    msg.content = content;
    msg.agent_id = agent_id;
    
    return trigger_on_message_batch(session_key, {msg});
}

// ========================================
// Integration Hook: 在 auto-reply 流程中调用
// ========================================

core::Result<void> trigger_delta_after_reply(
    const std::string& session_key,
    const std::string& user_message,
    const std::string& assistant_message,
    std::shared_ptr<memory::SessionMemoryProvider> session_provider
) {
    if (!session_provider) {
        return core::Ok();
    }
    
    DeltaTrigger trigger(session_provider);
    
    // 对齐 OpenClaw: 批量触发 user + assistant 消息
    std::vector<Message> messages;
    
    Message user_msg;
    user_msg.role = "user";
    user_msg.content = user_message;
    messages.push_back(user_msg);
    
    Message assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = assistant_message;
    messages.push_back(assistant_msg);
    
    return trigger.trigger_on_message_batch(session_key, messages);
}

} // namespace auto_reply
} // namespace hunterclaw
