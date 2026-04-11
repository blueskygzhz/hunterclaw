#pragma once

#include "hunterclaw/config/config_system.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>

namespace hunterclaw {
namespace hooks {

// ============================================================================
// 🎯 OpenClaw 对齐: hooks/types.ts & hooks/internal-hooks.ts
// ============================================================================
// Hook 系统提供可扩展的事件驱动机制
// 支持 Agent 生命周期、消息处理、网关启动等事件
// 严格对齐 OpenClaw 的 hooks 实现
// ============================================================================

// ========================================================================
// Hook Metadata & Installation
// ========================================================================

/// Hook installation spec
/// 对齐: HookInstallSpec
struct HookInstallSpec {
    std::optional<std::string> id;
    std::string kind;  // "bundled" | "npm" | "git"
    std::optional<std::string> label;
    std::optional<std::string> package;
    std::optional<std::string> repository;
    std::vector<std::string> bins;
};

/// Hook metadata from HOOK.md frontmatter
/// 对齐: OpenClawHookMetadata
struct OpenClawHookMetadata {
    bool always = false;                    // Always invoke this hook
    std::optional<std::string> hook_key;    // Hook key override
    std::optional<std::string> emoji;       // Hook display emoji
    std::optional<std::string> homepage;    // Hook homepage URL
    std::vector<std::string> events;        // Events this hook handles
    std::string export_name = "default";    // Export name
    std::vector<std::string> os;            // OS requirements
    
    // Requirements
    struct {
        std::vector<std::string> bins;      // Required binaries
        std::vector<std::string> any_bins;  // Any of these binaries
        std::vector<std::string> env;       // Required env vars
        std::vector<std::string> config;    // Required config keys
    } requires;
    
    std::vector<HookInstallSpec> install;   // Installation specs
};

/// Hook invocation policy
/// 对齐: HookInvocationPolicy
struct HookInvocationPolicy {
    bool enabled = true;
};

/// Parsed frontmatter from HOOK.md
/// 对齐: ParsedHookFrontmatter
using ParsedHookFrontmatter = std::unordered_map<std::string, std::string>;

/// Hook source type
/// 对齐: HookSource
enum class HookSource {
    OPENCLAW_BUNDLED,   // openclaw-bundled
    OPENCLAW_MANAGED,   // openclaw-managed
    OPENCLAW_WORKSPACE, // openclaw-workspace
    OPENCLAW_PLUGIN     // openclaw-plugin
};

/// Hook definition
/// 对齐: Hook
struct Hook {
    std::string name;                // Hook name
    std::string description;         // Hook description
    HookSource source;               // Hook source
    std::optional<std::string> plugin_id;  // Plugin ID (if source = OPENCLAW_PLUGIN)
    std::string file_path;           // Path to HOOK.md
    std::string base_dir;            // Directory containing hook
    std::string handler_path;        // Path to handler module
};

/// Hook entry (Hook + metadata)
/// 对齐: HookEntry
struct HookEntry {
    Hook hook;
    ParsedHookFrontmatter frontmatter;
    std::optional<OpenClawHookMetadata> metadata;
    std::optional<HookInvocationPolicy> invocation;
};

/// Hook eligibility context
/// 对齐: HookEligibilityContext
struct HookEligibilityContext {
    struct {
        std::vector<std::string> platforms;
        std::function<bool(const std::string&)> has_bin;
        std::function<bool(const std::vector<std::string>&)> has_any_bin;
        std::optional<std::string> note;
    } remote;
};

/// Hook snapshot (for caching)
/// 对齐: HookSnapshot
struct HookSnapshot {
    std::vector<std::pair<std::string, std::vector<std::string>>> hooks;  // name -> events
    std::vector<Hook> resolved_hooks;
    int version = 1;
};

// ========================================================================
// Internal Hook Events
// ========================================================================

/// Hook event type
/// 对齐: InternalHookEventType
enum class HookEventType {
    COMMAND,   // Command processing
    SESSION,   // Session lifecycle
    AGENT,     // Agent lifecycle
    GATEWAY,   // Gateway events
    MESSAGE    // Message events
};

/// Base internal hook event
/// 对齐: InternalHookEvent (internal-hooks.ts:175-188)
struct InternalHookEvent {
    HookEventType type;
    std::string action;
    std::string session_key;                    // 对齐: sessionKey — OC 所有 hook 都携带
    core::JsonValue context;                    // Context data as JSON
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now(); // 对齐: timestamp
    std::vector<std::string> messages;          // 对齐: messages — hooks can push reply messages
    
    virtual ~InternalHookEvent() = default;
};

// ========================================================================
// Agent Events
// ========================================================================

/// Agent bootstrap context
/// 对齐: AgentBootstrapHookContext
struct AgentBootstrapContext {
    std::string workspace_dir;
    std::vector<std::string> bootstrap_files;  // List of bootstrap file paths
    std::optional<core::Config> config;
    std::optional<std::string> session_key;
    std::optional<std::string> session_id;
    std::optional<std::string> agent_id;
};

/// Agent bootstrap event
/// 对齐: AgentBootstrapHookEvent
struct AgentBootstrapEvent : public InternalHookEvent {
    AgentBootstrapContext bootstrap_context;
    
    AgentBootstrapEvent() {
        type = HookEventType::AGENT;
        action = "bootstrap";
    }
};

// ========================================================================
// Gateway Events
// ========================================================================

/// Gateway startup context
/// 对齐: GatewayStartupHookContext
struct GatewayStartupContext {
    std::optional<core::Config> config;
    std::optional<std::string> workspace_dir;
};

/// Gateway startup event
/// 对齐: GatewayStartupHookEvent
struct GatewayStartupEvent : public InternalHookEvent {
    GatewayStartupContext gateway_context;
    
    GatewayStartupEvent() {
        type = HookEventType::GATEWAY;
        action = "startup";
    }
};

// ========================================================================
// Message Events
// ========================================================================

/// Message received context
/// 对齐: MessageReceivedHookContext
struct MessageReceivedContext {
    std::string from;                           // Sender identifier
    std::string content;                        // Message content
    std::optional<int64_t> timestamp;           // Unix timestamp
    std::string channel_id;                     // Channel identifier
    std::optional<std::string> account_id;      // Provider account ID
    std::optional<std::string> conversation_id; // Conversation/chat ID
    std::optional<std::string> message_id;      // Message ID
    std::unordered_map<std::string, core::JsonValue> metadata;  // Extra metadata
};

/// Message received event
/// 对齐: MessageReceivedHookEvent
struct MessageReceivedEvent : public InternalHookEvent {
    MessageReceivedContext message_context;
    
    MessageReceivedEvent() {
        type = HookEventType::MESSAGE;
        action = "received";
    }
};

/// Message sent context
/// 对齐: MessageSentHookContext
struct MessageSentContext {
    std::string to;                             // Recipient identifier
    std::string content;                        // Message content
    bool success;                               // Send success
    std::optional<std::string> error;           // Error message
    std::string channel_id;                     // Channel identifier
    std::optional<std::string> account_id;      // Provider account ID
    std::optional<std::string> conversation_id; // Conversation/chat ID
    std::optional<std::string> message_id;      // Message ID
    std::optional<bool> is_group;               // Group/channel context
    std::optional<std::string> group_id;        // Group identifier
};

/// Message sent event
/// 对齐: MessageSentHookEvent
struct MessageSentEvent : public InternalHookEvent {
    MessageSentContext message_context;
    
    MessageSentEvent() {
        type = HookEventType::MESSAGE;
        action = "sent";
    }
};

/// Message transcribed context (after audio transcription)
/// 对齐: MessageTranscribedHookContext
struct MessageTranscribedContext {
    std::optional<std::string> from;
    std::optional<std::string> to;
    std::optional<std::string> body;            // Original body
    std::optional<std::string> body_for_agent;  // Enriched body
    std::optional<int64_t> timestamp;
    std::string channel_id;
    std::optional<std::string> conversation_id;
    std::optional<std::string> message_id;
    std::optional<std::string> sender_id;
    std::optional<std::string> sender_name;
    std::optional<std::string> sender_username;
    std::optional<std::string> provider;
    std::optional<std::string> surface;
    std::optional<std::string> media_path;
    std::optional<std::string> media_type;
    std::string transcript;                     // Transcribed text
};

/// Message transcribed event
/// 对齐: MessageTranscribedHookEvent
struct MessageTranscribedEvent : public InternalHookEvent {
    MessageTranscribedContext transcribed_context;
    
    MessageTranscribedEvent() {
        type = HookEventType::MESSAGE;
        action = "transcribed";
    }
};

/// Message preprocessed context (after preprocessing)
/// 对齐: MessagePreprocessedHookContext
struct MessagePreprocessedContext {
    std::optional<std::string> from;
    std::optional<std::string> to;
    std::optional<std::string> body;
    std::optional<std::string> body_for_agent;
    std::optional<int64_t> timestamp;
    std::string channel_id;
    std::optional<std::string> conversation_id;
    std::optional<std::string> message_id;
    std::optional<std::string> transcript;
    std::optional<bool> is_group;
    std::optional<std::string> group_id;
};

/// Message preprocessed event
/// 对齐: MessagePreprocessedHookEvent
struct MessagePreprocessedEvent : public InternalHookEvent {
    MessagePreprocessedContext preprocessed_context;
    
    MessagePreprocessedEvent() {
        type = HookEventType::MESSAGE;
        action = "preprocessed";
    }
};

// ========================================================================
// Session Events
// ========================================================================

/// Session patch context (对齐: SessionPatchHookContext)
struct SessionPatchContext {
    core::JsonValue session_entry;  // SessionEntry as JSON
    core::JsonValue patch;          // SessionsPatchParams as JSON
    std::optional<core::Config> config;
};

/// Session patch event (对齐: SessionPatchHookEvent)
struct SessionPatchEvent : public InternalHookEvent {
    SessionPatchContext patch_context;
    
    SessionPatchEvent() {
        type = HookEventType::SESSION;
        action = "patch";
    }
};

// ========================================================================
// Hook Handler
// ========================================================================

/// Hook handler function type
/// 对齐: InternalHookHandler = (event: InternalHookEvent) => Promise<void> | void
using InternalHookHandler = std::function<void(const InternalHookEvent&)>;

// ========================================================================
// Hook Registry & Manager
// ========================================================================

/// Hook registry for managing internal hooks
/// 对齐: internal-hooks.ts 的全局注册表
class HookRegistry {
public:
    static HookRegistry& instance();
    
    /// Register a hook handler
    /// 对齐: registerInternalHook(eventKey, handler)
    void register_hook(const std::string& event_key, InternalHookHandler handler);
    
    /// Unregister a hook handler
    /// 对齐: unregisterInternalHook(eventKey, handler)
    void unregister_hook(const std::string& event_key, InternalHookHandler handler);
    
    /// Clear all hooks
    /// 对齐: clearInternalHooks()
    void clear_hooks();
    
    /// Trigger a hook event
    /// 对齐: triggerInternalHook(event)
    void trigger_hook(const InternalHookEvent& event);
    
    /// Get handlers for event key
    std::vector<InternalHookHandler> get_handlers(const std::string& event_key) const;
    
    /// Check if any listeners registered for type:action
    /// 对齐: hasInternalHookListeners(type, action)
    bool has_listeners(HookEventType type, const std::string& action) const;
    
    /// Get all registered event keys (debugging)
    /// 对齐: getRegisteredEventKeys()
    std::vector<std::string> get_registered_event_keys() const;
    
    /// Get event key from event
    /// 对齐: createInternalHookEvent() 的 eventKey 生成逻辑
    static std::string get_event_key(const InternalHookEvent& event);
    static std::string get_event_key(HookEventType type, const std::string& action);
    
private:
    HookRegistry() = default;
    ~HookRegistry() = default;
    HookRegistry(const HookRegistry&) = delete;
    HookRegistry& operator=(const HookRegistry&) = delete;
    
    std::unordered_map<std::string, std::vector<InternalHookHandler>> handlers_;
    mutable std::mutex mutex_;
};

// ========================================================================
// Utility Functions
// ========================================================================

/// Create agent bootstrap event
/// 对齐: createInternalHookEvent({ type: "agent", action: "bootstrap", ... })
AgentBootstrapEvent create_agent_bootstrap_event(const AgentBootstrapContext& context);

/// Create gateway startup event
/// 对齐: createInternalHookEvent({ type: "gateway", action: "startup", ... })
GatewayStartupEvent create_gateway_startup_event(const GatewayStartupContext& context);

/// Create message received event
/// 对齐: createInternalHookEvent({ type: "message", action: "received", ... })
MessageReceivedEvent create_message_received_event(const MessageReceivedContext& context);

/// Create message sent event
/// 对齐: createInternalHookEvent({ type: "message", action: "sent", ... })
MessageSentEvent create_message_sent_event(const MessageSentContext& context);

/// Create message transcribed event
/// 对齐: createInternalHookEvent({ type: "message", action: "transcribed", ... })
MessageTranscribedEvent create_message_transcribed_event(const MessageTranscribedContext& context);

/// Create message preprocessed event
/// 对齐: createInternalHookEvent({ type: "message", action: "preprocessed", ... })
MessagePreprocessedEvent create_message_preprocessed_event(const MessagePreprocessedContext& context);

/// Create session patch event
/// 对齐: createInternalHookEvent({ type: "session", action: "patch", ... })
SessionPatchEvent create_session_patch_event(const SessionPatchContext& context);

/// Create internal hook event (generic factory)
/// 对齐: createInternalHookEvent(type, action, sessionKey, context)
InternalHookEvent create_internal_hook_event(
    HookEventType type,
    const std::string& action,
    const std::string& session_key,
    const core::JsonValue& context = core::JsonValue::object());

/// Check if event is agent bootstrap event
/// 对齐: isAgentBootstrapEvent(event)
bool is_agent_bootstrap_event(const InternalHookEvent& event);

/// Check if event is gateway startup event
/// 对齐: isGatewayStartupEvent(event)
bool is_gateway_startup_event(const InternalHookEvent& event);

/// Check if event is message received event
/// 对齐: isMessageReceivedEvent(event)
bool is_message_received_event(const InternalHookEvent& event);

/// Check if event is message sent event
/// 对齐: isMessageSentEvent(event)
bool is_message_sent_event(const InternalHookEvent& event);

/// Check if event is message transcribed event
/// 对齐: isMessageTranscribedEvent(event)
bool is_message_transcribed_event(const InternalHookEvent& event);

/// Check if event is message preprocessed event
/// 对齐: isMessagePreprocessedEvent(event)
bool is_message_preprocessed_event(const InternalHookEvent& event);

/// Check if event is session patch event
/// 对齐: isSessionPatchEvent(event)
bool is_session_patch_event(const InternalHookEvent& event);

}  // namespace hooks
}  // namespace hunterclaw
