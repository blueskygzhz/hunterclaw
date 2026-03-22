#pragma once
// ============================================================================
// tool_hooks.h - Plugin Hook System
//
// Strictly aligned with OpenClaw:
//   - plugins/types.ts  (all PluginHook* Event/Context/Result types)
//   - plugins/hooks.ts  (createHookRunner: all 24 hook runners)
//
// Hook execution modes (aligned with OpenClaw):
//   void hooks    (parallel fire-and-forget):
//     agent_end, llm_input, llm_output, before_compaction, after_compaction,
//     before_reset, message_received, message_sent, after_tool_call,
//     session_start, session_end, subagent_spawned, subagent_ended,
//     gateway_start, gateway_stop
//   modifying hooks (sequential, results merged):
//     before_model_resolve, before_prompt_build, before_agent_start,
//     message_sending, before_tool_call, subagent_spawning,
//     subagent_delivery_target
//   sync hooks (synchronous, sequential):
//     tool_result_persist, before_message_write
// ============================================================================

#include "hunterclaw/core/types.h"
#include "hunterclaw/tools/tool.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace hunterclaw {
namespace tools {

// ============================================================================
// PluginHookName (aligned with OpenClaw PLUGIN_HOOK_NAMES)
// ============================================================================

enum class PluginHookName {
    // Agent hooks
    BEFORE_MODEL_RESOLVE,
    BEFORE_PROMPT_BUILD,
    BEFORE_AGENT_START,
    LLM_INPUT,
    LLM_OUTPUT,
    AGENT_END,
    BEFORE_COMPACTION,
    AFTER_COMPACTION,
    BEFORE_RESET,
    // Message hooks
    MESSAGE_RECEIVED,
    MESSAGE_SENDING,
    MESSAGE_SENT,
    // Tool hooks
    BEFORE_TOOL_CALL,
    AFTER_TOOL_CALL,
    TOOL_RESULT_PERSIST,
    // Message write hooks
    BEFORE_MESSAGE_WRITE,
    // Session hooks
    SESSION_START,
    SESSION_END,
    SUBAGENT_SPAWNING,
    SUBAGENT_DELIVERY_TARGET,
    SUBAGENT_SPAWNED,
    SUBAGENT_ENDED,
    // Gateway hooks
    GATEWAY_START,
    GATEWAY_STOP,
};

std::string plugin_hook_name_to_string(PluginHookName name);

// ============================================================================
// Agent Context (aligned with OpenClaw PluginHookAgentContext)
// ============================================================================

/// Shared context across all agent hooks
/// Aligned with: PluginHookAgentContext in plugins/types.ts
struct PluginHookAgentContext {
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    /// Ephemeral session UUID — regenerated on /new and /reset
    std::optional<std::string> session_id;
    std::optional<std::string> workspace_dir;
    std::optional<std::string> message_provider;
    /// What initiated this agent run: "user", "heartbeat", "cron", or "memory"
    std::optional<std::string> trigger;
    /// Channel identifier (e.g. "telegram", "discord", "whatsapp")
    std::optional<std::string> channel_id;
};

// ============================================================================
// before_model_resolve hook (aligned with OpenClaw)
// ============================================================================

/// Event for before_model_resolve hook
/// Aligned with: PluginHookBeforeModelResolveEvent in plugins/types.ts
struct PluginHookBeforeModelResolveEvent {
    /// User prompt for this run. No session messages are available yet.
    std::string prompt;
};

/// Result from before_model_resolve hook
/// Aligned with: PluginHookBeforeModelResolveResult in plugins/types.ts
struct PluginHookBeforeModelResolveResult {
    /// Override the model for this agent run. E.g. "llama3.3:8b"
    std::optional<std::string> model_override;
    /// Override the provider for this agent run. E.g. "ollama"
    std::optional<std::string> provider_override;
};

using BeforeModelResolveHandler = std::function<
    std::optional<PluginHookBeforeModelResolveResult>(
        const PluginHookBeforeModelResolveEvent& event,
        const PluginHookAgentContext& ctx
    )
>;

// ============================================================================
// before_prompt_build hook (aligned with OpenClaw)
// ============================================================================

/// Event for before_prompt_build hook
/// Aligned with: PluginHookBeforePromptBuildEvent in plugins/types.ts
struct PluginHookBeforePromptBuildEvent {
    std::string prompt;
    /// Session messages prepared for this run
    std::vector<core::JsonValue> messages;
};

/// Result from before_prompt_build hook
/// Aligned with: PluginHookBeforePromptBuildResult in plugins/types.ts
struct PluginHookBeforePromptBuildResult {
    std::optional<std::string> system_prompt;
    std::optional<std::string> prepend_context;
    /// Prepended to agent system prompt (for prompt caching)
    std::optional<std::string> prepend_system_context;
    /// Appended to agent system prompt (for prompt caching)
    std::optional<std::string> append_system_context;
};

using BeforePromptBuildHandler = std::function<
    std::optional<PluginHookBeforePromptBuildResult>(
        const PluginHookBeforePromptBuildEvent& event,
        const PluginHookAgentContext& ctx
    )
>;

// ============================================================================
// before_agent_start hook (aligned with OpenClaw)
// Legacy compatibility: combines before_model_resolve + before_prompt_build
// ============================================================================

/// Event for before_agent_start hook
/// Aligned with: PluginHookBeforeAgentStartEvent in plugins/types.ts
struct PluginHookBeforeAgentStartEvent {
    std::string prompt;
    /// Optional because legacy hook can run in pre-session phase
    std::optional<std::vector<core::JsonValue>> messages;
};

/// Result from before_agent_start hook (combines both phases)
/// Aligned with: PluginHookBeforeAgentStartResult in plugins/types.ts
struct PluginHookBeforeAgentStartResult {
    // From before_prompt_build
    std::optional<std::string> system_prompt;
    std::optional<std::string> prepend_context;
    std::optional<std::string> prepend_system_context;
    std::optional<std::string> append_system_context;
    // From before_model_resolve
    std::optional<std::string> model_override;
    std::optional<std::string> provider_override;
};

using BeforeAgentStartHandler = std::function<
    std::optional<PluginHookBeforeAgentStartResult>(
        const PluginHookBeforeAgentStartEvent& event,
        const PluginHookAgentContext& ctx
    )
>;

// ============================================================================
// llm_input hook (aligned with OpenClaw)
// ============================================================================

/// Event for llm_input hook
/// Aligned with: PluginHookLlmInputEvent in plugins/types.ts
struct PluginHookLlmInputEvent {
    std::string run_id;
    std::string session_id;
    std::string provider;
    std::string model;
    std::optional<std::string> system_prompt;
    std::string prompt;
    std::vector<core::JsonValue> history_messages;
    int images_count = 0;
};

using LlmInputHandler = std::function<
    void(
        const PluginHookLlmInputEvent& event,
        const PluginHookAgentContext& ctx
    )
>;

// ============================================================================
// llm_output hook (aligned with OpenClaw)
// ============================================================================

/// Event for llm_output hook
/// Aligned with: PluginHookLlmOutputEvent in plugins/types.ts
struct PluginHookLlmOutputEvent {
    std::string run_id;
    std::string session_id;
    std::string provider;
    std::string model;
    std::vector<std::string> assistant_texts;
    std::optional<core::JsonValue> last_assistant;
    struct Usage {
        std::optional<int> input;
        std::optional<int> output;
        std::optional<int> cache_read;
        std::optional<int> cache_write;
        std::optional<int> total;
    };
    std::optional<Usage> usage;
};

using LlmOutputHandler = std::function<
    void(
        const PluginHookLlmOutputEvent& event,
        const PluginHookAgentContext& ctx
    )
>;

// ============================================================================
// agent_end hook (aligned with OpenClaw)
// ============================================================================

/// Event for agent_end hook
/// Aligned with: PluginHookAgentEndEvent in plugins/types.ts
struct PluginHookAgentEndEvent {
    std::vector<core::JsonValue> messages;
    bool success = false;
    std::optional<std::string> error;
    std::optional<double> duration_ms;
};

using AgentEndHandler = std::function<
    void(
        const PluginHookAgentEndEvent& event,
        const PluginHookAgentContext& ctx
    )
>;

// ============================================================================
// before_compaction hook (aligned with OpenClaw)
// ============================================================================

/// Event for before_compaction hook
/// Aligned with: PluginHookBeforeCompactionEvent in plugins/types.ts
struct PluginHookBeforeCompactionEvent {
    /// Total messages in the session before any truncation or compaction
    int message_count = 0;
    /// Messages being fed to the compaction LLM (after history-limit truncation)
    std::optional<int> compacting_count;
    std::optional<int> token_count;
    std::optional<std::vector<core::JsonValue>> messages;
    /// Path to the session JSONL transcript
    std::optional<std::string> session_file;
};

using BeforeCompactionHandler = std::function<
    void(
        const PluginHookBeforeCompactionEvent& event,
        const PluginHookAgentContext& ctx
    )
>;

// ============================================================================
// after_compaction hook (aligned with OpenClaw)
// ============================================================================

/// Event for after_compaction hook
/// Aligned with: PluginHookAfterCompactionEvent in plugins/types.ts
struct PluginHookAfterCompactionEvent {
    int message_count = 0;
    std::optional<int> token_count;
    int compacted_count = 0;
    /// Path to the session JSONL transcript
    std::optional<std::string> session_file;
};

using AfterCompactionHandler = std::function<
    void(
        const PluginHookAfterCompactionEvent& event,
        const PluginHookAgentContext& ctx
    )
>;

// ============================================================================
// before_reset hook (aligned with OpenClaw)
// Fired when /new or /reset clears a session, before messages are lost.
// ============================================================================

/// Event for before_reset hook
/// Aligned with: PluginHookBeforeResetEvent in plugins/types.ts
struct PluginHookBeforeResetEvent {
    std::optional<std::string> session_file;
    std::optional<std::vector<core::JsonValue>> messages;
    std::optional<std::string> reason;
};

using BeforeResetHandler = std::function<
    void(
        const PluginHookBeforeResetEvent& event,
        const PluginHookAgentContext& ctx
    )
>;

// ============================================================================
// Message Context (aligned with OpenClaw PluginHookMessageContext)
// ============================================================================

/// Shared context across message hooks
/// Aligned with: PluginHookMessageContext in plugins/types.ts
struct PluginHookMessageContext {
    std::string channel_id;
    std::optional<std::string> account_id;
    std::optional<std::string> conversation_id;
};

// ============================================================================
// message_received hook (aligned with OpenClaw)
// ============================================================================

/// Event for message_received hook
/// Aligned with: PluginHookMessageReceivedEvent in plugins/types.ts
struct PluginHookMessageReceivedEvent {
    std::string from;
    std::string content;
    std::optional<int64_t> timestamp;
    std::optional<core::JsonValue> metadata;
};

using MessageReceivedHandler = std::function<
    void(
        const PluginHookMessageReceivedEvent& event,
        const PluginHookMessageContext& ctx
    )
>;

// ============================================================================
// message_sending hook (aligned with OpenClaw)
// ============================================================================

/// Event for message_sending hook
/// Aligned with: PluginHookMessageSendingEvent in plugins/types.ts
struct PluginHookMessageSendingEvent {
    std::string to;
    std::string content;
    std::optional<core::JsonValue> metadata;
};

/// Result from message_sending hook
/// Aligned with: PluginHookMessageSendingResult in plugins/types.ts
struct PluginHookMessageSendingResult {
    std::optional<std::string> content;
    std::optional<bool> cancel;
};

using MessageSendingHandler = std::function<
    std::optional<PluginHookMessageSendingResult>(
        const PluginHookMessageSendingEvent& event,
        const PluginHookMessageContext& ctx
    )
>;

// ============================================================================
// message_sent hook (aligned with OpenClaw)
// ============================================================================

/// Event for message_sent hook
/// Aligned with: PluginHookMessageSentEvent in plugins/types.ts
struct PluginHookMessageSentEvent {
    std::string to;
    std::string content;
    bool success = false;
    std::optional<std::string> error;
};

using MessageSentHandler = std::function<
    void(
        const PluginHookMessageSentEvent& event,
        const PluginHookMessageContext& ctx
    )
>;

// ============================================================================
// PluginHookToolContext (aligned with OpenClaw PluginHookToolContext)
// ============================================================================

/// Tool context passed to before_tool_call / after_tool_call hooks
/// Aligned with: PluginHookToolContext in plugins/types.ts
struct PluginHookToolContext {
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    /// Ephemeral session UUID — regenerated on /new and /reset
    std::optional<std::string> session_id;
    /// Stable run identifier for this agent invocation
    std::optional<std::string> run_id;
    std::string tool_name;
    /// Provider-specific tool call ID when available
    std::optional<std::string> tool_call_id;
};

// ============================================================================
// before_tool_call hook (aligned with OpenClaw)
// ============================================================================

/// Event for before_tool_call hook
/// Aligned with: PluginHookBeforeToolCallEvent in plugins/types.ts
struct PluginHookBeforeToolCallEvent {
    std::string tool_name;
    core::JsonValue params;  // Record<string, unknown>
    /// Stable run identifier for this agent invocation
    std::optional<std::string> run_id;
    /// Provider-specific tool call ID when available
    std::optional<std::string> tool_call_id;
};

/// Result from before_tool_call hook
/// Aligned with: PluginHookBeforeToolCallResult in plugins/types.ts
struct PluginHookBeforeToolCallResult {
    /// Modified params (if any handler wants to override)
    std::optional<core::JsonValue> params;
    /// If true, the tool call is blocked
    std::optional<bool> block;
    /// Reason for blocking (shown to the agent)
    std::optional<std::string> block_reason;
};

/// before_tool_call handler type
/// Aligned with: PluginHookHandlerMap["before_tool_call"]
/// Runs sequentially (modifying hook). Returns optional result.
using BeforeToolCallHandler = std::function<
    std::optional<PluginHookBeforeToolCallResult>(
        const PluginHookBeforeToolCallEvent& event,
        const PluginHookToolContext& ctx
    )
>;

// ============================================================================
// after_tool_call hook (aligned with OpenClaw)
// ============================================================================

/// Event for after_tool_call hook
/// Aligned with: PluginHookAfterToolCallEvent in plugins/types.ts
struct PluginHookAfterToolCallEvent {
    std::string tool_name;
    core::JsonValue params;
    std::optional<std::string> run_id;
    std::optional<std::string> tool_call_id;
    /// Tool result (serialized as JSON)
    std::optional<core::JsonValue> result;
    /// Error message if tool failed
    std::optional<std::string> error;
    /// Duration in milliseconds
    std::optional<double> duration_ms;
};

/// after_tool_call handler type
/// Aligned with: PluginHookHandlerMap["after_tool_call"]
/// Runs in parallel (void hook). No return value.
using AfterToolCallHandler = std::function<
    void(
        const PluginHookAfterToolCallEvent& event,
        const PluginHookToolContext& ctx
    )
>;

// ============================================================================
// tool_result_persist hook (aligned with OpenClaw)
// Intentionally synchronous — runs on hot path where session transcripts
// are appended synchronously.
// ============================================================================

/// Context for tool_result_persist hook
/// Aligned with: PluginHookToolResultPersistContext in plugins/types.ts
struct PluginHookToolResultPersistContext {
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<std::string> tool_name;
    std::optional<std::string> tool_call_id;
};

/// Event for tool_result_persist hook
/// Aligned with: PluginHookToolResultPersistEvent in plugins/types.ts
struct PluginHookToolResultPersistEvent {
    std::optional<std::string> tool_name;
    std::optional<std::string> tool_call_id;
    /// The toolResult message about to be written to the session transcript
    core::JsonValue message;
    /// True when the tool result was synthesized by a guard/repair step
    bool is_synthetic = false;
};

/// Result from tool_result_persist hook
/// Aligned with: PluginHookToolResultPersistResult in plugins/types.ts
struct PluginHookToolResultPersistResult {
    /// Modified message (if any handler wants to override)
    std::optional<core::JsonValue> message;
};

/// tool_result_persist handler type (SYNCHRONOUS)
/// Aligned with: PluginHookHandlerMap["tool_result_persist"]
using ToolResultPersistHandler = std::function<
    std::optional<PluginHookToolResultPersistResult>(
        const PluginHookToolResultPersistEvent& event,
        const PluginHookToolResultPersistContext& ctx
    )
>;

// ============================================================================
// before_message_write hook (aligned with OpenClaw)
// Intentionally synchronous — runs on hot path where session transcripts
// are appended synchronously.
// ============================================================================

/// Event for before_message_write hook
/// Aligned with: PluginHookBeforeMessageWriteEvent in plugins/types.ts
struct PluginHookBeforeMessageWriteEvent {
    core::JsonValue message;
    std::optional<std::string> session_key;
    std::optional<std::string> agent_id;
};

/// Result from before_message_write hook
/// Aligned with: PluginHookBeforeMessageWriteResult in plugins/types.ts
struct PluginHookBeforeMessageWriteResult {
    /// If true, message is NOT written to JSONL
    bool block = false;
    /// Optional: modified message to write instead
    std::optional<core::JsonValue> message;
};

/// before_message_write handler type (SYNCHRONOUS)
/// Aligned with: PluginHookHandlerMap["before_message_write"]
using BeforeMessageWriteHandler = std::function<
    std::optional<PluginHookBeforeMessageWriteResult>(
        const PluginHookBeforeMessageWriteEvent& event,
        const std::string& agent_id,
        const std::string& session_key
    )
>;

// ============================================================================
// Session Context (aligned with OpenClaw PluginHookSessionContext)
// ============================================================================

/// Shared context across session hooks
/// Aligned with: PluginHookSessionContext in plugins/types.ts
struct PluginHookSessionContext {
    std::optional<std::string> agent_id;
    std::string session_id;
    std::optional<std::string> session_key;
};

// ============================================================================
// session_start hook (aligned with OpenClaw)
// ============================================================================

/// Event for session_start hook
/// Aligned with: PluginHookSessionStartEvent in plugins/types.ts
struct PluginHookSessionStartEvent {
    std::string session_id;
    std::optional<std::string> session_key;
    std::optional<std::string> resumed_from;
};

using SessionStartHandler = std::function<
    void(
        const PluginHookSessionStartEvent& event,
        const PluginHookSessionContext& ctx
    )
>;

// ============================================================================
// session_end hook (aligned with OpenClaw)
// ============================================================================

/// Event for session_end hook
/// Aligned with: PluginHookSessionEndEvent in plugins/types.ts
struct PluginHookSessionEndEvent {
    std::string session_id;
    std::optional<std::string> session_key;
    int message_count = 0;
    std::optional<double> duration_ms;
};

using SessionEndHandler = std::function<
    void(
        const PluginHookSessionEndEvent& event,
        const PluginHookSessionContext& ctx
    )
>;

// ============================================================================
// Subagent Context (aligned with OpenClaw PluginHookSubagentContext)
// ============================================================================

/// Shared context across subagent hooks
/// Aligned with: PluginHookSubagentContext in plugins/types.ts
struct PluginHookSubagentContext {
    std::optional<std::string> run_id;
    std::optional<std::string> child_session_key;
    std::optional<std::string> requester_session_key;
};

// ============================================================================
// subagent_spawning hook (aligned with OpenClaw)
// ============================================================================

/// Requester info for subagent spawn
struct PluginHookSubagentRequester {
    std::optional<std::string> channel;
    std::optional<std::string> account_id;
    std::optional<std::string> to;
    std::optional<std::string> thread_id;
};

/// Base fields shared by subagent_spawning and subagent_spawned
struct PluginHookSubagentSpawnBase {
    std::string child_session_key;
    std::string agent_id;
    std::optional<std::string> label;
    /// "run" or "session"
    std::string mode;
    std::optional<PluginHookSubagentRequester> requester;
    bool thread_requested = false;
};

/// Event for subagent_spawning hook
/// Aligned with: PluginHookSubagentSpawningEvent in plugins/types.ts
using PluginHookSubagentSpawningEvent = PluginHookSubagentSpawnBase;

/// Result from subagent_spawning hook
/// Aligned with: PluginHookSubagentSpawningResult in plugins/types.ts
struct PluginHookSubagentSpawningResult {
    /// "ok" or "error"
    std::string status = "ok";
    std::optional<bool> thread_binding_ready;
    std::optional<std::string> error;
};

using SubagentSpawningHandler = std::function<
    std::optional<PluginHookSubagentSpawningResult>(
        const PluginHookSubagentSpawningEvent& event,
        const PluginHookSubagentContext& ctx
    )
>;

// ============================================================================
// subagent_delivery_target hook (aligned with OpenClaw)
// ============================================================================

/// Origin info for subagent delivery target
struct PluginHookSubagentOrigin {
    std::optional<std::string> channel;
    std::optional<std::string> account_id;
    std::optional<std::string> to;
    std::optional<std::string> thread_id;
};

/// Event for subagent_delivery_target hook
/// Aligned with: PluginHookSubagentDeliveryTargetEvent in plugins/types.ts
struct PluginHookSubagentDeliveryTargetEvent {
    std::string child_session_key;
    std::string requester_session_key;
    std::optional<PluginHookSubagentOrigin> requester_origin;
    std::optional<std::string> child_run_id;
    /// "run" or "session"
    std::optional<std::string> spawn_mode;
    bool expects_completion_message = false;
};

/// Result from subagent_delivery_target hook
/// Aligned with: PluginHookSubagentDeliveryTargetResult in plugins/types.ts
struct PluginHookSubagentDeliveryTargetResult {
    std::optional<PluginHookSubagentOrigin> origin;
};

using SubagentDeliveryTargetHandler = std::function<
    std::optional<PluginHookSubagentDeliveryTargetResult>(
        const PluginHookSubagentDeliveryTargetEvent& event,
        const PluginHookSubagentContext& ctx
    )
>;

// ============================================================================
// subagent_spawned hook (aligned with OpenClaw)
// ============================================================================

/// Event for subagent_spawned hook
/// Aligned with: PluginHookSubagentSpawnedEvent in plugins/types.ts
struct PluginHookSubagentSpawnedEvent : public PluginHookSubagentSpawnBase {
    std::string run_id;
};

using SubagentSpawnedHandler = std::function<
    void(
        const PluginHookSubagentSpawnedEvent& event,
        const PluginHookSubagentContext& ctx
    )
>;

// ============================================================================
// subagent_ended hook (aligned with OpenClaw)
// ============================================================================

/// Event for subagent_ended hook
/// Aligned with: PluginHookSubagentEndedEvent in plugins/types.ts
struct PluginHookSubagentEndedEvent {
    std::string target_session_key;
    /// "subagent" or "acp"
    std::string target_kind;
    std::string reason;
    std::optional<bool> send_farewell;
    std::optional<std::string> account_id;
    std::optional<std::string> run_id;
    std::optional<int64_t> ended_at;
    /// "ok" | "error" | "timeout" | "killed" | "reset" | "deleted"
    std::optional<std::string> outcome;
    std::optional<std::string> error;
};

using SubagentEndedHandler = std::function<
    void(
        const PluginHookSubagentEndedEvent& event,
        const PluginHookSubagentContext& ctx
    )
>;

// ============================================================================
// Gateway Context (aligned with OpenClaw PluginHookGatewayContext)
// ============================================================================

/// Shared context across gateway hooks
/// Aligned with: PluginHookGatewayContext in plugins/types.ts
struct PluginHookGatewayContext {
    std::optional<int> port;
};

// ============================================================================
// gateway_start hook (aligned with OpenClaw)
// ============================================================================

/// Event for gateway_start hook
/// Aligned with: PluginHookGatewayStartEvent in plugins/types.ts
struct PluginHookGatewayStartEvent {
    int port = 0;
};

using GatewayStartHandler = std::function<
    void(
        const PluginHookGatewayStartEvent& event,
        const PluginHookGatewayContext& ctx
    )
>;

// ============================================================================
// gateway_stop hook (aligned with OpenClaw)
// ============================================================================

/// Event for gateway_stop hook
/// Aligned with: PluginHookGatewayStopEvent in plugins/types.ts
struct PluginHookGatewayStopEvent {
    std::optional<std::string> reason;
};

using GatewayStopHandler = std::function<
    void(
        const PluginHookGatewayStopEvent& event,
        const PluginHookGatewayContext& ctx
    )
>;

// ============================================================================
// Hook Registration Entry (aligned with OpenClaw PluginHookRegistration)
// ============================================================================

template<typename THandler>
struct PluginHookRegistration {
    std::string plugin_id;
    PluginHookName hook_name;
    THandler handler;
    int priority = 0;  // Higher priority runs first (aligned with OpenClaw)
    std::string source;
};

// ============================================================================
// ToolHookManager
//
// Manages all 24 plugin lifecycle hooks.
//
// Aligned with OpenClaw createHookRunner() in plugins/hooks.ts:
//   void hooks:      parallel (fire-and-forget), all handlers called
//   modifying hooks: sequential, priority-sorted, results merged
//   sync hooks:      synchronous, sequential, priority-sorted
// ============================================================================

class ToolHookManager {
public:
    ToolHookManager() = default;
    ~ToolHookManager() = default;

    // ========== Registration: Agent Hooks ==========

    void register_before_model_resolve(const std::string& plugin_id, BeforeModelResolveHandler handler, int priority = 0);
    void register_before_prompt_build(const std::string& plugin_id, BeforePromptBuildHandler handler, int priority = 0);
    void register_before_agent_start(const std::string& plugin_id, BeforeAgentStartHandler handler, int priority = 0);
    void register_llm_input(const std::string& plugin_id, LlmInputHandler handler, int priority = 0);
    void register_llm_output(const std::string& plugin_id, LlmOutputHandler handler, int priority = 0);
    void register_agent_end(const std::string& plugin_id, AgentEndHandler handler, int priority = 0);
    void register_before_compaction(const std::string& plugin_id, BeforeCompactionHandler handler, int priority = 0);
    void register_after_compaction(const std::string& plugin_id, AfterCompactionHandler handler, int priority = 0);
    void register_before_reset(const std::string& plugin_id, BeforeResetHandler handler, int priority = 0);

    // ========== Registration: Message Hooks ==========

    void register_message_received(const std::string& plugin_id, MessageReceivedHandler handler, int priority = 0);
    void register_message_sending(const std::string& plugin_id, MessageSendingHandler handler, int priority = 0);
    void register_message_sent(const std::string& plugin_id, MessageSentHandler handler, int priority = 0);

    // ========== Registration: Tool Hooks ==========

    void register_before_tool_call(const std::string& plugin_id, BeforeToolCallHandler handler, int priority = 0);
    void register_after_tool_call(const std::string& plugin_id, AfterToolCallHandler handler, int priority = 0);
    void register_tool_result_persist(const std::string& plugin_id, ToolResultPersistHandler handler, int priority = 0);

    // ========== Registration: Message Write Hooks ==========

    void register_before_message_write(const std::string& plugin_id, BeforeMessageWriteHandler handler, int priority = 0);

    // ========== Registration: Session Hooks ==========

    void register_session_start(const std::string& plugin_id, SessionStartHandler handler, int priority = 0);
    void register_session_end(const std::string& plugin_id, SessionEndHandler handler, int priority = 0);
    void register_subagent_spawning(const std::string& plugin_id, SubagentSpawningHandler handler, int priority = 0);
    void register_subagent_delivery_target(const std::string& plugin_id, SubagentDeliveryTargetHandler handler, int priority = 0);
    void register_subagent_spawned(const std::string& plugin_id, SubagentSpawnedHandler handler, int priority = 0);
    void register_subagent_ended(const std::string& plugin_id, SubagentEndedHandler handler, int priority = 0);

    // ========== Registration: Gateway Hooks ==========

    void register_gateway_start(const std::string& plugin_id, GatewayStartHandler handler, int priority = 0);
    void register_gateway_stop(const std::string& plugin_id, GatewayStopHandler handler, int priority = 0);

    // ========== Unregister ==========

    /// Unregister all hooks for a plugin
    void unregister_plugin(const std::string& plugin_id);

    /// Clear all hooks
    void clear_all_hooks();

    // ========== Execution: Agent Hooks ==========

    /// Run before_model_resolve hooks (sequential, modifying)
    /// Aligned with: runBeforeModelResolve() in plugins/hooks.ts
    /// Merge: modelOverride/providerOverride — first defined wins (higher priority)
    std::optional<PluginHookBeforeModelResolveResult> run_before_model_resolve(
        const PluginHookBeforeModelResolveEvent& event,
        const PluginHookAgentContext& ctx);

    /// Run before_prompt_build hooks (sequential, modifying)
    /// Aligned with: runBeforePromptBuild() in plugins/hooks.ts
    /// Merge: systemPrompt last wins; prependContext/appendContext concatenated
    std::optional<PluginHookBeforePromptBuildResult> run_before_prompt_build(
        const PluginHookBeforePromptBuildEvent& event,
        const PluginHookAgentContext& ctx);

    /// Run before_agent_start hooks (sequential, modifying)
    /// Aligned with: runBeforeAgentStart() in plugins/hooks.ts
    /// Legacy compatibility: combines model resolve + prompt build phases
    std::optional<PluginHookBeforeAgentStartResult> run_before_agent_start(
        const PluginHookBeforeAgentStartEvent& event,
        const PluginHookAgentContext& ctx);

    /// Run llm_input hooks (void, parallel)
    /// Aligned with: runLlmInput() in plugins/hooks.ts
    void run_llm_input(const PluginHookLlmInputEvent& event, const PluginHookAgentContext& ctx);

    /// Run llm_output hooks (void, parallel)
    /// Aligned with: runLlmOutput() in plugins/hooks.ts
    void run_llm_output(const PluginHookLlmOutputEvent& event, const PluginHookAgentContext& ctx);

    /// Run agent_end hooks (void, parallel)
    /// Aligned with: runAgentEnd() in plugins/hooks.ts
    void run_agent_end(const PluginHookAgentEndEvent& event, const PluginHookAgentContext& ctx);

    /// Run before_compaction hooks (void, parallel)
    /// Aligned with: runBeforeCompaction() in plugins/hooks.ts
    void run_before_compaction(const PluginHookBeforeCompactionEvent& event, const PluginHookAgentContext& ctx);

    /// Run after_compaction hooks (void, parallel)
    /// Aligned with: runAfterCompaction() in plugins/hooks.ts
    void run_after_compaction(const PluginHookAfterCompactionEvent& event, const PluginHookAgentContext& ctx);

    /// Run before_reset hooks (void, parallel)
    /// Aligned with: runBeforeReset() in plugins/hooks.ts
    void run_before_reset(const PluginHookBeforeResetEvent& event, const PluginHookAgentContext& ctx);

    // ========== Execution: Message Hooks ==========

    /// Run message_received hooks (void, parallel)
    /// Aligned with: runMessageReceived() in plugins/hooks.ts
    void run_message_received(const PluginHookMessageReceivedEvent& event, const PluginHookMessageContext& ctx);

    /// Run message_sending hooks (sequential, modifying)
    /// Aligned with: runMessageSending() in plugins/hooks.ts
    /// Merge: content/cancel — last defined wins
    std::optional<PluginHookMessageSendingResult> run_message_sending(
        const PluginHookMessageSendingEvent& event,
        const PluginHookMessageContext& ctx);

    /// Run message_sent hooks (void, parallel)
    /// Aligned with: runMessageSent() in plugins/hooks.ts
    void run_message_sent(const PluginHookMessageSentEvent& event, const PluginHookMessageContext& ctx);

    // ========== Execution: Tool Hooks ==========

    /// Run before_tool_call hooks (sequential, modifying)
    /// Aligned with: runBeforeToolCall() in plugins/hooks.ts
    std::optional<PluginHookBeforeToolCallResult> run_before_tool_call(
        const PluginHookBeforeToolCallEvent& event,
        const PluginHookToolContext& ctx);

    /// Run after_tool_call hooks (void, parallel)
    /// Aligned with: runAfterToolCall() in plugins/hooks.ts
    void run_after_tool_call(
        const PluginHookAfterToolCallEvent& event,
        const PluginHookToolContext& ctx);

    /// Run tool_result_persist hooks (sync, sequential)
    /// Aligned with: runToolResultPersist() in plugins/hooks.ts
    std::optional<PluginHookToolResultPersistResult> run_tool_result_persist(
        const PluginHookToolResultPersistEvent& event,
        const PluginHookToolResultPersistContext& ctx);

    // ========== Execution: Message Write Hooks ==========

    /// Run before_message_write hooks (sync, sequential)
    /// Aligned with: runBeforeMessageWrite() in plugins/hooks.ts
    std::optional<PluginHookBeforeMessageWriteResult> run_before_message_write(
        const PluginHookBeforeMessageWriteEvent& event,
        const std::string& agent_id = "",
        const std::string& session_key = "");

    // ========== Execution: Session Hooks ==========

    /// Run session_start hooks (void, parallel)
    /// Aligned with: runSessionStart() in plugins/hooks.ts
    void run_session_start(const PluginHookSessionStartEvent& event, const PluginHookSessionContext& ctx);

    /// Run session_end hooks (void, parallel)
    /// Aligned with: runSessionEnd() in plugins/hooks.ts
    void run_session_end(const PluginHookSessionEndEvent& event, const PluginHookSessionContext& ctx);

    /// Run subagent_spawning hooks (sequential, modifying)
    /// Aligned with: runSubagentSpawning() in plugins/hooks.ts
    /// Merge: error status wins; threadBindingReady OR-ed
    std::optional<PluginHookSubagentSpawningResult> run_subagent_spawning(
        const PluginHookSubagentSpawningEvent& event,
        const PluginHookSubagentContext& ctx);

    /// Run subagent_delivery_target hooks (sequential, modifying)
    /// Aligned with: runSubagentDeliveryTarget() in plugins/hooks.ts
    /// Merge: first defined origin wins
    std::optional<PluginHookSubagentDeliveryTargetResult> run_subagent_delivery_target(
        const PluginHookSubagentDeliveryTargetEvent& event,
        const PluginHookSubagentContext& ctx);

    /// Run subagent_spawned hooks (void, parallel)
    /// Aligned with: runSubagentSpawned() in plugins/hooks.ts
    void run_subagent_spawned(const PluginHookSubagentSpawnedEvent& event, const PluginHookSubagentContext& ctx);

    /// Run subagent_ended hooks (void, parallel)
    /// Aligned with: runSubagentEnded() in plugins/hooks.ts
    void run_subagent_ended(const PluginHookSubagentEndedEvent& event, const PluginHookSubagentContext& ctx);

    // ========== Execution: Gateway Hooks ==========

    /// Run gateway_start hooks (void, parallel)
    /// Aligned with: runGatewayStart() in plugins/hooks.ts
    void run_gateway_start(const PluginHookGatewayStartEvent& event, const PluginHookGatewayContext& ctx);

    /// Run gateway_stop hooks (void, parallel)
    /// Aligned with: runGatewayStop() in plugins/hooks.ts
    void run_gateway_stop(const PluginHookGatewayStopEvent& event, const PluginHookGatewayContext& ctx);

    // ========== Query ==========

    bool has_hooks(PluginHookName name) const;
    size_t hook_count(PluginHookName name) const;

    bool has_before_tool_call_hooks() const;
    bool has_after_tool_call_hooks() const;
    bool has_tool_result_persist_hooks() const;
    bool has_before_message_write_hooks() const;

    size_t before_tool_call_count() const;
    size_t after_tool_call_count() const;

    std::vector<std::string> list_plugin_ids() const;

private:
    // ---- Agent hook entries ----
    struct BeforeModelResolveEntry  { std::string plugin_id; BeforeModelResolveHandler  handler; int priority = 0; };
    struct BeforePromptBuildEntry   { std::string plugin_id; BeforePromptBuildHandler   handler; int priority = 0; };
    struct BeforeAgentStartEntry    { std::string plugin_id; BeforeAgentStartHandler    handler; int priority = 0; };
    struct LlmInputEntry            { std::string plugin_id; LlmInputHandler            handler; int priority = 0; };
    struct LlmOutputEntry           { std::string plugin_id; LlmOutputHandler           handler; int priority = 0; };
    struct AgentEndEntry            { std::string plugin_id; AgentEndHandler            handler; int priority = 0; };
    struct BeforeCompactionEntry    { std::string plugin_id; BeforeCompactionHandler    handler; int priority = 0; };
    struct AfterCompactionEntry     { std::string plugin_id; AfterCompactionHandler     handler; int priority = 0; };
    struct BeforeResetEntry         { std::string plugin_id; BeforeResetHandler         handler; int priority = 0; };
    // ---- Message hook entries ----
    struct MessageReceivedEntry     { std::string plugin_id; MessageReceivedHandler     handler; int priority = 0; };
    struct MessageSendingEntry      { std::string plugin_id; MessageSendingHandler      handler; int priority = 0; };
    struct MessageSentEntry         { std::string plugin_id; MessageSentHandler         handler; int priority = 0; };
    // ---- Tool hook entries ----
    struct BeforeToolCallEntry      { std::string plugin_id; BeforeToolCallHandler      handler; int priority = 0; };
    struct AfterToolCallEntry       { std::string plugin_id; AfterToolCallHandler       handler; int priority = 0; };
    struct ToolResultPersistEntry   { std::string plugin_id; ToolResultPersistHandler   handler; int priority = 0; };
    // ---- Message write hook entries ----
    struct BeforeMessageWriteEntry  { std::string plugin_id; BeforeMessageWriteHandler  handler; int priority = 0; };
    // ---- Session hook entries ----
    struct SessionStartEntry        { std::string plugin_id; SessionStartHandler        handler; int priority = 0; };
    struct SessionEndEntry          { std::string plugin_id; SessionEndHandler          handler; int priority = 0; };
    struct SubagentSpawningEntry    { std::string plugin_id; SubagentSpawningHandler    handler; int priority = 0; };
    struct SubagentDeliveryTargetEntry { std::string plugin_id; SubagentDeliveryTargetHandler handler; int priority = 0; };
    struct SubagentSpawnedEntry     { std::string plugin_id; SubagentSpawnedHandler     handler; int priority = 0; };
    struct SubagentEndedEntry       { std::string plugin_id; SubagentEndedHandler       handler; int priority = 0; };
    // ---- Gateway hook entries ----
    struct GatewayStartEntry        { std::string plugin_id; GatewayStartHandler        handler; int priority = 0; };
    struct GatewayStopEntry         { std::string plugin_id; GatewayStopHandler         handler; int priority = 0; };

    mutable std::mutex mutex_;

    // Agent hooks
    std::vector<BeforeModelResolveEntry>     before_model_resolve_hooks_;
    std::vector<BeforePromptBuildEntry>      before_prompt_build_hooks_;
    std::vector<BeforeAgentStartEntry>       before_agent_start_hooks_;
    std::vector<LlmInputEntry>               llm_input_hooks_;
    std::vector<LlmOutputEntry>              llm_output_hooks_;
    std::vector<AgentEndEntry>               agent_end_hooks_;
    std::vector<BeforeCompactionEntry>       before_compaction_hooks_;
    std::vector<AfterCompactionEntry>        after_compaction_hooks_;
    std::vector<BeforeResetEntry>            before_reset_hooks_;
    // Message hooks
    std::vector<MessageReceivedEntry>        message_received_hooks_;
    std::vector<MessageSendingEntry>         message_sending_hooks_;
    std::vector<MessageSentEntry>            message_sent_hooks_;
    // Tool hooks
    std::vector<BeforeToolCallEntry>         before_tool_call_hooks_;
    std::vector<AfterToolCallEntry>          after_tool_call_hooks_;
    std::vector<ToolResultPersistEntry>      tool_result_persist_hooks_;
    // Message write hooks
    std::vector<BeforeMessageWriteEntry>     before_message_write_hooks_;
    // Session hooks
    std::vector<SessionStartEntry>           session_start_hooks_;
    std::vector<SessionEndEntry>             session_end_hooks_;
    std::vector<SubagentSpawningEntry>       subagent_spawning_hooks_;
    std::vector<SubagentDeliveryTargetEntry> subagent_delivery_target_hooks_;
    std::vector<SubagentSpawnedEntry>        subagent_spawned_hooks_;
    std::vector<SubagentEndedEntry>          subagent_ended_hooks_;
    // Gateway hooks
    std::vector<GatewayStartEntry>           gateway_start_hooks_;
    std::vector<GatewayStopEntry>            gateway_stop_hooks_;

    // Helper: sort by priority descending (higher first)
    template<typename T>
    static void sort_by_priority(std::vector<T>& vec) {
        std::stable_sort(vec.begin(), vec.end(),
            [](const T& a, const T& b) { return a.priority > b.priority; });
    }

    // Helper: remove all entries for a plugin_id
    template<typename T>
    static void remove_plugin(std::vector<T>& vec, const std::string& plugin_id) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
            [&plugin_id](const T& e) { return e.plugin_id == plugin_id; }), vec.end());
    }
};

// ============================================================================
// Legacy compatibility aliases
// (kept for existing callers in exec.cc / tool_manager.cc)
// ============================================================================

/// Legacy BeforeHookOutcome — maps to PluginHookBeforeToolCallResult
struct BeforeHookOutcome {
    bool blocked = false;
    std::string reason;
    core::JsonValue params;

    /// Convert from PluginHookBeforeToolCallResult
    static BeforeHookOutcome from_plugin_result(
        const PluginHookBeforeToolCallResult& r,
        const core::JsonValue& original_params)
    {
        BeforeHookOutcome out;
        out.blocked = r.block.value_or(false);
        out.reason  = r.block_reason.value_or("");
        out.params  = r.params.value_or(original_params);
        return out;
    }
};

/// Legacy BeforeToolCallHook — wraps new BeforeToolCallHandler
using BeforeToolCallHook = std::function<
    BeforeHookOutcome(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolContext& ctx
    )
>;

/// Legacy AfterToolCallHook — wraps new AfterToolCallHandler
using AfterToolCallHook = std::function<
    void(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolResult& result,
        double duration_ms,
        const std::string& error
    )
>;

// ============================================================================
// Built-in hooks (aligned with OpenClaw bundled hooks)
// ============================================================================

namespace builtin_hooks {

/// Param validation hook (checks required params)
BeforeToolCallHandler create_param_validation_hook();

/// Logging hook (records all tool calls)
AfterToolCallHandler create_logging_hook();

/// Performance monitoring hook (tracks execution time)
AfterToolCallHandler create_performance_monitoring_hook();

/// Error collection hook (tracks error rates)
AfterToolCallHandler create_error_collection_hook();

/// Audit hook (logs sensitive tool calls)
struct AuditHookConfig {
    std::vector<std::string> sensitive_tools;
    std::string audit_log_path;
};
AfterToolCallHandler create_audit_hook(const AuditHookConfig& config);

/// Rate limit hook (limits tool call frequency)
struct RateLimitConfig {
    int max_calls_per_minute = 60;
    int max_calls_per_tool_per_minute = 10;
};
BeforeToolCallHandler create_rate_limit_hook(const RateLimitConfig& config);

} // namespace builtin_hooks

// ============================================================================
// Param normalization (aligned with OpenClaw)
// ============================================================================

struct ParamNormalizationConfig {
    bool snake_to_camel = true;
    bool camel_to_snake = false;
    bool trim_strings = true;
    bool remove_nulls = false;
};

core::JsonValue normalize_tool_params(
    const core::JsonValue& params,
    const ParamNormalizationConfig& config = {}
);

std::string to_camel_case(const std::string& snake_case);
std::string to_snake_case(const std::string& camel_case);

} // namespace tools
} // namespace hunterclaw
