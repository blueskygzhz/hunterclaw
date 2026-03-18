#include "hunterclaw/hooks/hooks.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace hunterclaw {
namespace hooks {

// ============================================================================
// HookRegistry Implementation
// ============================================================================

HookRegistry& HookRegistry::instance() {
    static HookRegistry instance;
    return instance;
}

void HookRegistry::register_hook(const std::string& event_key, InternalHookHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    handlers_[event_key].push_back(handler);
    
    //spdlog::debug("[HookRegistry] Registered hook for event '{}'", event_key);
}

void HookRegistry::unregister_hook(const std::string& event_key, InternalHookHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = handlers_.find(event_key);
    if (it == handlers_.end()) {
        return;
    }
    
    // Note: In C++ we can't directly compare std::function objects
    // In practice, we would need a different approach (e.g., handle/ID based)
    // For now, we just clear all handlers for this event
    //spdlog::warn("[HookRegistry] unregister_hook not fully implemented - clearing all handlers for '{}'",
    //             event_key);
}

void HookRegistry::clear_hooks() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    handlers_.clear();
    
    //spdlog::debug("[HookRegistry] Cleared all hooks");
}

void HookRegistry::trigger_hook(const InternalHookEvent& event) {
    std::string event_key = get_event_key(event);
    
    std::vector<InternalHookHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = handlers_.find(event_key);
        if (it != handlers_.end()) {
            handlers = it->second;
        }
    }
    
    if (handlers.empty()) {
        //spdlog::trace("[HookRegistry] No hooks registered for event '{}'", event_key);
        return;
    }
    
    //spdlog::debug("[HookRegistry] Triggering {} hook(s) for event '{}'", 
//              handlers.size(), event_key);
    
    // Execute all handlers
    for (const auto& handler : handlers) {
        try {
            handler(event);
        } catch (const std::exception& e) {
            //spdlog::error("[HookRegistry] Hook handler failed for event '{}': {}",
            //              event_key, e.what());
        }
    }
}

std::vector<InternalHookHandler> HookRegistry::get_handlers(
    const std::string& event_key
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = handlers_.find(event_key);
    if (it == handlers_.end()) {
        return {};
    }
    
    return it->second;
}

std::string HookRegistry::get_event_key(const InternalHookEvent& event) {
    return get_event_key(event.type, event.action);
}

std::string HookRegistry::get_event_key(HookEventType type, const std::string& action) {
    std::string type_str;
    switch (type) {
        case HookEventType::COMMAND:
            type_str = "command";
            break;
        case HookEventType::SESSION:
            type_str = "session";
            break;
        case HookEventType::AGENT:
            type_str = "agent";
            break;
        case HookEventType::GATEWAY:
            type_str = "gateway";
            break;
        case HookEventType::MESSAGE:
            type_str = "message";
            break;
        default:
            type_str = "unknown";
            break;
    }
    
    return type_str + ":" + action;
}

// ============================================================================
// Event Creation Functions
// ============================================================================

AgentBootstrapEvent create_agent_bootstrap_event(const AgentBootstrapContext& context) {
    AgentBootstrapEvent event;
    event.bootstrap_context = context;
    
    // Populate context JSON
    event.context = core::JsonValue::object();
    event.context["workspaceDir"] = context.workspace_dir;
    
    auto bootstrap_files_arr = core::JsonValue::array();
    for (const auto& file : context.bootstrap_files) {
        bootstrap_files_arr.push_back(file);
    }
    event.context["bootstrapFiles"] = bootstrap_files_arr;
    
    if (context.session_key) {
        event.context["sessionKey"] = *context.session_key;
    }
    if (context.session_id) {
        event.context["sessionId"] = *context.session_id;
    }
    if (context.agent_id) {
        event.context["agentId"] = *context.agent_id;
    }
    
    return event;
}

GatewayStartupEvent create_gateway_startup_event(const GatewayStartupContext& context) {
    GatewayStartupEvent event;
    event.gateway_context = context;
    
    // Populate context JSON
    event.context = core::JsonValue::object();
    if (context.workspace_dir) {
        event.context["workspaceDir"] = *context.workspace_dir;
    }
    
    return event;
}

MessageReceivedEvent create_message_received_event(const MessageReceivedContext& context) {
    MessageReceivedEvent event;
    event.message_context = context;
    
    // Populate context JSON
    event.context = core::JsonValue::object();
    event.context["from"] = context.from;
    event.context["content"] = context.content;
    event.context["channelId"] = context.channel_id;
    
    if (context.timestamp) {
        event.context["timestamp"] = static_cast<int64_t>(*context.timestamp);
    }
    if (context.account_id) {
        event.context["accountId"] = *context.account_id;
    }
    if (context.conversation_id) {
        event.context["conversationId"] = *context.conversation_id;
    }
    if (context.message_id) {
        event.context["messageId"] = *context.message_id;
    }
    
    // Add metadata
    if (!context.metadata.empty()) {
        auto metadata_obj = core::JsonValue::object();
        for (const auto& [key, value] : context.metadata) {
            metadata_obj[key] = value;
        }
        event.context["metadata"] = metadata_obj;
    }
    
    return event;
}

MessageSentEvent create_message_sent_event(const MessageSentContext& context) {
    MessageSentEvent event;
    event.message_context = context;
    
    // Populate context JSON
    event.context = core::JsonValue::object();
    event.context["to"] = context.to;
    event.context["content"] = context.content;
    event.context["success"] = context.success;
    event.context["channelId"] = context.channel_id;
    
    if (context.error) {
        event.context["error"] = *context.error;
    }
    if (context.account_id) {
        event.context["accountId"] = *context.account_id;
    }
    if (context.conversation_id) {
        event.context["conversationId"] = *context.conversation_id;
    }
    if (context.message_id) {
        event.context["messageId"] = *context.message_id;
    }
    if (context.is_group) {
        event.context["isGroup"] = *context.is_group;
    }
    if (context.group_id) {
        event.context["groupId"] = *context.group_id;
    }
    
    return event;
}

MessageTranscribedEvent create_message_transcribed_event(const MessageTranscribedContext& context) {
    MessageTranscribedEvent event;
    event.transcribed_context = context;
    
    // Populate context JSON
    event.context = core::JsonValue::object();
    event.context["channelId"] = context.channel_id;
    event.context["transcript"] = context.transcript;
    
    if (context.from) {
        event.context["from"] = *context.from;
    }
    if (context.to) {
        event.context["to"] = *context.to;
    }
    if (context.body) {
        event.context["body"] = *context.body;
    }
    if (context.body_for_agent) {
        event.context["bodyForAgent"] = *context.body_for_agent;
    }
    if (context.timestamp) {
        event.context["timestamp"] = static_cast<int64_t>(*context.timestamp);
    }
    if (context.conversation_id) {
        event.context["conversationId"] = *context.conversation_id;
    }
    if (context.message_id) {
        event.context["messageId"] = *context.message_id;
    }
    if (context.sender_id) {
        event.context["senderId"] = *context.sender_id;
    }
    if (context.sender_name) {
        event.context["senderName"] = *context.sender_name;
    }
    if (context.sender_username) {
        event.context["senderUsername"] = *context.sender_username;
    }
    if (context.provider) {
        event.context["provider"] = *context.provider;
    }
    if (context.surface) {
        event.context["surface"] = *context.surface;
    }
    if (context.media_path) {
        event.context["mediaPath"] = *context.media_path;
    }
    if (context.media_type) {
        event.context["mediaType"] = *context.media_type;
    }
    
    return event;
}

MessagePreprocessedEvent create_message_preprocessed_event(const MessagePreprocessedContext& context) {
    MessagePreprocessedEvent event;
    event.preprocessed_context = context;
    
    // Populate context JSON
    event.context = core::JsonValue::object();
    event.context["channelId"] = context.channel_id;
    
    if (context.from) {
        event.context["from"] = *context.from;
    }
    if (context.to) {
        event.context["to"] = *context.to;
    }
    if (context.body) {
        event.context["body"] = *context.body;
    }
    if (context.body_for_agent) {
        event.context["bodyForAgent"] = *context.body_for_agent;
    }
    if (context.timestamp) {
        event.context["timestamp"] = static_cast<int64_t>(*context.timestamp);
    }
    if (context.conversation_id) {
        event.context["conversationId"] = *context.conversation_id;
    }
    if (context.message_id) {
        event.context["messageId"] = *context.message_id;
    }
    if (context.transcript) {
        event.context["transcript"] = *context.transcript;
    }
    if (context.is_group) {
        event.context["isGroup"] = *context.is_group;
    }
    if (context.group_id) {
        event.context["groupId"] = *context.group_id;
    }
    
    return event;
}

// ============================================================================
// Event Type Checking Functions
// ============================================================================

bool is_agent_bootstrap_event(const InternalHookEvent& event) {
    return event.type == HookEventType::AGENT && event.action == "bootstrap";
}

bool is_gateway_startup_event(const InternalHookEvent& event) {
    return event.type == HookEventType::GATEWAY && event.action == "startup";
}

bool is_message_sent_event(const InternalHookEvent& event) {
    return event.type == HookEventType::MESSAGE && event.action == "sent";
}

}  // namespace hooks
}  // namespace hunterclaw
