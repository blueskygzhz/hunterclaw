#pragma once
// ============================================================================
// tool_hooks.h - Tool-layer Hook Manager
//
// All PluginHook* Event/Context/Result types are defined in:
//   include/hunterclaw/plugins/plugin_system.h
//   (aligned with OpenClaw src/plugins/types.ts)
//
// This file provides:
//   - ToolHookManager: executes all 24 plugin lifecycle hooks
//     (aligned with OpenClaw src/plugins/hooks.ts createHookRunner)
//   - Typed handler aliases (using plugins:: types)
//   - Built-in hooks (param validation, logging, rate limiting, etc.)
//   - Param normalization utilities
//   - Legacy compatibility aliases
//
// Hook execution modes (aligned with OpenClaw createHookRunner):
//   void hooks    (sequential, fire-and-forget):
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

#include "hunterclaw/plugins/plugin_system.h"
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
// Import PluginHook* types from plugins namespace
// All types are defined in plugins/plugin_system.h (aligned with OpenClaw
// src/plugins/types.ts). Bring them into tools namespace for convenience.
// ============================================================================

using plugins::PluginHookName;

// Agent context & events
using plugins::PluginHookAgentContext;
using plugins::PluginHookBeforeModelResolveEvent;
using plugins::PluginHookBeforeModelResolveResult;
using plugins::PluginHookBeforePromptBuildEvent;
using plugins::PluginHookBeforePromptBuildResult;
using plugins::PluginHookBeforeAgentStartEvent;
using plugins::PluginHookBeforeAgentStartResult;
using plugins::PluginHookBeforeAgentReplyEvent;
using plugins::PluginHookBeforeAgentReplyResult;
using plugins::PluginHookLlmInputEvent;
using plugins::PluginHookLlmOutputEvent;
using plugins::PluginHookLlmOutputUsage;
using plugins::PluginHookAgentEndEvent;
using plugins::PluginHookBeforeCompactionEvent;
using plugins::PluginHookAfterCompactionEvent;
using plugins::PluginHookBeforeResetEvent;

// Message context & events
using plugins::PluginHookMessageContext;
using plugins::PluginHookMessageReceivedEvent;
using plugins::PluginHookMessageSendingEvent;
using plugins::PluginHookMessageSendingResult;
using plugins::PluginHookMessageSentEvent;

// Tool context & events
using plugins::PluginHookToolContext;
using plugins::PluginHookBeforeToolCallEvent;
using plugins::PluginHookBeforeToolCallResult;
using plugins::PluginHookAfterToolCallEvent;
using plugins::PluginHookToolResultPersistContext;
using plugins::PluginHookToolResultPersistEvent;
using plugins::PluginHookToolResultPersistResult;
using plugins::PluginHookBeforeMessageWriteEvent;
using plugins::PluginHookBeforeMessageWriteContext;
using plugins::PluginHookBeforeMessageWriteResult;

// Session context & events
using plugins::PluginHookSessionContext;
using plugins::PluginHookSessionStartEvent;
using plugins::PluginHookSessionEndEvent;

// Subagent context & events
using plugins::PluginHookSubagentContext;
using plugins::PluginHookSubagentRequester;
using plugins::PluginHookSubagentSpawnBase;
using plugins::PluginHookSubagentSpawningEvent;
using plugins::PluginHookSubagentSpawningResult;
using plugins::PluginHookSubagentDeliveryTargetEvent;
using plugins::PluginHookSubagentDeliveryTargetResult;
using plugins::PluginHookSubagentSpawnedEvent;
using plugins::PluginHookSubagentEndedEvent;

// Gateway context & events
using plugins::PluginHookGatewayContext;
using plugins::PluginHookGatewayStartEvent;
using plugins::PluginHookGatewayStopEvent;

// ============================================================================
// Typed handler aliases
// Aligned with OpenClaw PluginHookHandlerMap in plugins/types.ts
// ============================================================================

// Agent hooks
using BeforeModelResolveHandler = std::function<
    std::optional<PluginHookBeforeModelResolveResult>(
        const PluginHookBeforeModelResolveEvent& event,
        const PluginHookAgentContext& ctx)>;

using BeforePromptBuildHandler = std::function<
    std::optional<PluginHookBeforePromptBuildResult>(
        const PluginHookBeforePromptBuildEvent& event,
        const PluginHookAgentContext& ctx)>;

using BeforeAgentStartHandler = std::function<
    std::optional<PluginHookBeforeAgentStartResult>(
        const PluginHookBeforeAgentStartEvent& event,
        const PluginHookAgentContext& ctx)>;

/// Claiming hook (first {handled:true} wins). Aligned with
/// PluginHookHandlerMap["before_agent_reply"].
using BeforeAgentReplyHandler = std::function<
    std::optional<PluginHookBeforeAgentReplyResult>(
        const PluginHookBeforeAgentReplyEvent& event,
        const PluginHookAgentContext& ctx)>;

using LlmInputHandler = std::function<
    void(const PluginHookLlmInputEvent& event,
         const PluginHookAgentContext& ctx)>;

using LlmOutputHandler = std::function<
    void(const PluginHookLlmOutputEvent& event,
         const PluginHookAgentContext& ctx)>;

using AgentEndHandler = std::function<
    void(const PluginHookAgentEndEvent& event,
         const PluginHookAgentContext& ctx)>;

using BeforeCompactionHandler = std::function<
    void(const PluginHookBeforeCompactionEvent& event,
         const PluginHookAgentContext& ctx)>;

using AfterCompactionHandler = std::function<
    void(const PluginHookAfterCompactionEvent& event,
         const PluginHookAgentContext& ctx)>;

using BeforeResetHandler = std::function<
    void(const PluginHookBeforeResetEvent& event,
         const PluginHookAgentContext& ctx)>;

// Message hooks
using MessageReceivedHandler = std::function<
    void(const PluginHookMessageReceivedEvent& event,
         const PluginHookMessageContext& ctx)>;

using MessageSendingHandler = std::function<
    std::optional<PluginHookMessageSendingResult>(
        const PluginHookMessageSendingEvent& event,
        const PluginHookMessageContext& ctx)>;

using MessageSentHandler = std::function<
    void(const PluginHookMessageSentEvent& event,
         const PluginHookMessageContext& ctx)>;

// Tool hooks
/// Runs sequentially (modifying hook). Returns optional result.
/// Aligned with: PluginHookHandlerMap["before_tool_call"]
using BeforeToolCallHandler = std::function<
    std::optional<PluginHookBeforeToolCallResult>(
        const PluginHookBeforeToolCallEvent& event,
        const PluginHookToolContext& ctx)>;

/// Runs in parallel (void hook). No return value.
/// Aligned with: PluginHookHandlerMap["after_tool_call"]
using AfterToolCallHandler = std::function<
    void(const PluginHookAfterToolCallEvent& event,
         const PluginHookToolContext& ctx)>;

/// Synchronous. Aligned with: PluginHookHandlerMap["tool_result_persist"]
using ToolResultPersistHandler = std::function<
    std::optional<PluginHookToolResultPersistResult>(
        const PluginHookToolResultPersistEvent& event,
        const PluginHookToolResultPersistContext& ctx)>;

/// Synchronous. Aligned with: PluginHookHandlerMap["before_message_write"]
using BeforeMessageWriteHandler = std::function<
    std::optional<PluginHookBeforeMessageWriteResult>(
        const PluginHookBeforeMessageWriteEvent& event,
        const std::string& agent_id,
        const std::string& session_key)>;

// Session hooks
using SessionStartHandler = std::function<
    void(const PluginHookSessionStartEvent& event,
         const PluginHookSessionContext& ctx)>;

using SessionEndHandler = std::function<
    void(const PluginHookSessionEndEvent& event,
         const PluginHookSessionContext& ctx)>;

// Subagent hooks
using SubagentSpawningHandler = std::function<
    std::optional<PluginHookSubagentSpawningResult>(
        const PluginHookSubagentSpawningEvent& event,
        const PluginHookSubagentContext& ctx)>;

using SubagentDeliveryTargetHandler = std::function<
    std::optional<PluginHookSubagentDeliveryTargetResult>(
        const PluginHookSubagentDeliveryTargetEvent& event,
        const PluginHookSubagentContext& ctx)>;

using SubagentSpawnedHandler = std::function<
    void(const PluginHookSubagentSpawnedEvent& event,
         const PluginHookSubagentContext& ctx)>;

using SubagentEndedHandler = std::function<
    void(const PluginHookSubagentEndedEvent& event,
         const PluginHookSubagentContext& ctx)>;

// Gateway hooks
using GatewayStartHandler = std::function<
    void(const PluginHookGatewayStartEvent& event,
         const PluginHookGatewayContext& ctx)>;

using GatewayStopHandler = std::function<
    void(const PluginHookGatewayStopEvent& event,
         const PluginHookGatewayContext& ctx)>;

// ============================================================================
// ToolHookManager
//
// Manages all 24 plugin lifecycle hooks.
//
// Aligned with OpenClaw createHookRunner() in plugins/hooks.ts:
//   void hooks:      sequential (fire-and-forget), all handlers called
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
    void register_before_agent_reply(const std::string& plugin_id, BeforeAgentReplyHandler handler, int priority = 0);
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

    /// Run before_agent_reply hooks (claiming: first {handled:true} wins).
    /// Aligned with: runBeforeAgentReply() = runClaimingHook in plugins/hooks.ts
    /// Returns the claiming result, or nullopt when no handler claimed the turn.
    std::optional<PluginHookBeforeAgentReplyResult> run_before_agent_reply(
        const PluginHookBeforeAgentReplyEvent& event,
        const PluginHookAgentContext& ctx);

    /// Run llm_input hooks (void, sequential)
    void run_llm_input(const PluginHookLlmInputEvent& event, const PluginHookAgentContext& ctx);

    /// Run llm_output hooks (void, sequential)
    void run_llm_output(const PluginHookLlmOutputEvent& event, const PluginHookAgentContext& ctx);

    /// Run agent_end hooks (void, sequential)
    void run_agent_end(const PluginHookAgentEndEvent& event, const PluginHookAgentContext& ctx);

    /// Run before_compaction hooks (void, sequential)
    void run_before_compaction(const PluginHookBeforeCompactionEvent& event, const PluginHookAgentContext& ctx);

    /// Run after_compaction hooks (void, sequential)
    void run_after_compaction(const PluginHookAfterCompactionEvent& event, const PluginHookAgentContext& ctx);

    /// Run before_reset hooks (void, sequential)
    void run_before_reset(const PluginHookBeforeResetEvent& event, const PluginHookAgentContext& ctx);

    // ========== Execution: Message Hooks ==========

    /// Run message_received hooks (void, sequential)
    void run_message_received(const PluginHookMessageReceivedEvent& event, const PluginHookMessageContext& ctx);

    /// Run message_sending hooks (sequential, modifying)
    /// Aligned with: runMessageSending() in plugins/hooks.ts
    /// Merge: content/cancel — last defined wins
    std::optional<PluginHookMessageSendingResult> run_message_sending(
        const PluginHookMessageSendingEvent& event,
        const PluginHookMessageContext& ctx);

    /// Run message_sent hooks (void, sequential)
    void run_message_sent(const PluginHookMessageSentEvent& event, const PluginHookMessageContext& ctx);

    // ========== Execution: Tool Hooks ==========

    /// Run before_tool_call hooks (sequential, modifying)
    /// Aligned with: runBeforeToolCall() in plugins/hooks.ts
    std::optional<PluginHookBeforeToolCallResult> run_before_tool_call(
        const PluginHookBeforeToolCallEvent& event,
        const PluginHookToolContext& ctx);

    /// Run after_tool_call hooks (void, sequential)
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

    /// Run session_start hooks (void, sequential)
    void run_session_start(const PluginHookSessionStartEvent& event, const PluginHookSessionContext& ctx);

    /// Run session_end hooks (void, sequential)
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

    /// Run subagent_spawned hooks (void, sequential)
    void run_subagent_spawned(const PluginHookSubagentSpawnedEvent& event, const PluginHookSubagentContext& ctx);

    /// Run subagent_ended hooks (void, sequential)
    void run_subagent_ended(const PluginHookSubagentEndedEvent& event, const PluginHookSubagentContext& ctx);

    // ========== Execution: Gateway Hooks ==========

    /// Run gateway_start hooks (void, sequential)
    void run_gateway_start(const PluginHookGatewayStartEvent& event, const PluginHookGatewayContext& ctx);

    /// Run gateway_stop hooks (void, sequential)
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
    struct BeforeAgentReplyEntry    { std::string plugin_id; BeforeAgentReplyHandler    handler; int priority = 0; };
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
    std::vector<BeforeAgentReplyEntry>       before_agent_reply_hooks_;
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
        out.blocked = r.block;
        out.reason  = r.block_reason.value_or("");
        if (r.params_json) {
            auto parsed = core::JsonValue::parse(*r.params_json);
            out.params = (parsed.ok && parsed.value) ? std::move(*parsed.value) : original_params;
        } else {
            out.params = original_params;
        }
        return out;
    }
};

/// Legacy BeforeToolCallHook — wraps new BeforeToolCallHandler
using BeforeToolCallHook = std::function<
    BeforeHookOutcome(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolContext& ctx)>;

/// Legacy AfterToolCallHook — wraps new AfterToolCallHandler
using AfterToolCallHook = std::function<
    void(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolResult& result,
        double duration_ms,
        const std::string& error)>;

// ============================================================================
// Built-in hooks (aligned with OpenClaw bundled hooks pattern)
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
    const ParamNormalizationConfig& config = {});

std::string to_camel_case(const std::string& snake_case);
std::string to_snake_case(const std::string& camel_case);

// ============================================================================
// Global ToolHookManager
//
// Aligned with OpenClaw getGlobalHookRunner() / setGlobalHookRunner()
// in plugins/hooks.ts.
//
// Usage:
//   auto* mgr = tools::get_global_hook_manager();
//   if (mgr && mgr->has_before_tool_call_hooks()) {
//       auto result = mgr->run_before_tool_call(event, ctx);
//   }
// ============================================================================

/// Get the process-wide ToolHookManager (may be null if not initialized).
/// Aligned with: getGlobalHookRunner() in OpenClaw plugins/hooks.ts
ToolHookManager* get_global_hook_manager();

/// Set the process-wide ToolHookManager.
/// Aligned with: setGlobalHookRunner() in OpenClaw plugins/hooks.ts
void set_global_hook_manager(ToolHookManager* mgr);

/// Get-or-create a lazily initialized global ToolHookManager.
/// Returns a stable reference valid for the process lifetime.
ToolHookManager& require_global_hook_manager();

} // namespace tools
} // namespace hunterclaw
