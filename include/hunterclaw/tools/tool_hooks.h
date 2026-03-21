#pragma once
// ============================================================================
// tool_hooks.h - Plugin Hook System for Tool Calls
//
// Strictly aligned with OpenClaw:
//   - plugins/types.ts  (PluginHookName, PluginHookBeforeToolCallEvent, etc.)
//   - plugins/hooks.ts  (createHookRunner: runBeforeToolCall, runAfterToolCall,
//                        runToolResultPersist, runBeforeMessageWrite)
//
// OpenClaw Plugin Hook 体系（24 个 hook）：
//   void hooks    (并行 fire-and-forget): agent_end, llm_input, llm_output,
//                  before_compaction, after_compaction, before_reset,
//                  message_received, message_sent, after_tool_call,
//                  session_start, session_end, subagent_spawned,
//                  subagent_ended, gateway_start, gateway_stop
//   modifying hooks (串行，结果合并): before_model_resolve, before_prompt_build,
//                  before_agent_start, message_sending, before_tool_call,
//                  subagent_spawning, subagent_delivery_target
//   sync hooks    (同步，串行): tool_result_persist, before_message_write
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
// Manages before_tool_call / after_tool_call / tool_result_persist /
// before_message_write hooks.
//
// Aligned with OpenClaw createHookRunner() in plugins/hooks.ts:
//   - before_tool_call: sequential (modifying), priority-sorted, results merged
//   - after_tool_call:  parallel (void), priority-sorted
//   - tool_result_persist: sequential (sync), priority-sorted
//   - before_message_write: sequential (sync), priority-sorted
// ============================================================================

class ToolHookManager {
public:
    ToolHookManager() = default;
    ~ToolHookManager() = default;

    // ========== Registration ==========

    /// Register a before_tool_call hook
    /// @param plugin_id Plugin identifier
    /// @param handler   Handler function
    /// @param priority  Higher priority runs first (default 0)
    void register_before_tool_call(
        const std::string& plugin_id,
        BeforeToolCallHandler handler,
        int priority = 0
    );

    /// Register an after_tool_call hook
    void register_after_tool_call(
        const std::string& plugin_id,
        AfterToolCallHandler handler,
        int priority = 0
    );

    /// Register a tool_result_persist hook (sync)
    void register_tool_result_persist(
        const std::string& plugin_id,
        ToolResultPersistHandler handler,
        int priority = 0
    );

    /// Register a before_message_write hook (sync)
    void register_before_message_write(
        const std::string& plugin_id,
        BeforeMessageWriteHandler handler,
        int priority = 0
    );

    /// Unregister all hooks for a plugin
    void unregister_plugin(const std::string& plugin_id);

    /// Clear all hooks
    void clear_all_hooks();

    // ========== Execution ==========

    /// Run before_tool_call hooks (sequential, modifying)
    /// Aligned with: runBeforeToolCall() in plugins/hooks.ts
    ///
    /// Handlers run in priority order (higher first).
    /// Results are merged: params/block/blockReason from later handlers
    /// override earlier ones (first defined wins for params, last wins for block).
    ///
    /// @return merged result, or nullopt if no hooks registered
    std::optional<PluginHookBeforeToolCallResult> run_before_tool_call(
        const PluginHookBeforeToolCallEvent& event,
        const PluginHookToolContext& ctx
    );

    /// Run after_tool_call hooks (parallel/sequential void)
    /// Aligned with: runAfterToolCall() in plugins/hooks.ts
    /// All handlers are called; errors are caught and logged.
    void run_after_tool_call(
        const PluginHookAfterToolCallEvent& event,
        const PluginHookToolContext& ctx
    );

    /// Run tool_result_persist hooks (sync, sequential)
    /// Aligned with: runToolResultPersist() in plugins/hooks.ts
    ///
    /// Each handler may return { message } to replace the message passed
    /// to the next handler. Returns the final message (or nullopt if unchanged).
    std::optional<PluginHookToolResultPersistResult> run_tool_result_persist(
        const PluginHookToolResultPersistEvent& event,
        const PluginHookToolResultPersistContext& ctx
    );

    /// Run before_message_write hooks (sync, sequential)
    /// Aligned with: runBeforeMessageWrite() in plugins/hooks.ts
    ///
    /// If any handler returns { block: true }, returns immediately with block=true.
    /// If a handler returns { message }, the modified message replaces the original.
    std::optional<PluginHookBeforeMessageWriteResult> run_before_message_write(
        const PluginHookBeforeMessageWriteEvent& event,
        const std::string& agent_id = "",
        const std::string& session_key = ""
    );

    // ========== Query ==========

    bool has_before_tool_call_hooks() const;
    bool has_after_tool_call_hooks() const;
    bool has_tool_result_persist_hooks() const;
    bool has_before_message_write_hooks() const;

    size_t before_tool_call_count() const;
    size_t after_tool_call_count() const;

    std::vector<std::string> list_plugin_ids() const;

private:
    struct BeforeToolCallEntry {
        std::string plugin_id;
        BeforeToolCallHandler handler;
        int priority = 0;
    };
    struct AfterToolCallEntry {
        std::string plugin_id;
        AfterToolCallHandler handler;
        int priority = 0;
    };
    struct ToolResultPersistEntry {
        std::string plugin_id;
        ToolResultPersistHandler handler;
        int priority = 0;
    };
    struct BeforeMessageWriteEntry {
        std::string plugin_id;
        BeforeMessageWriteHandler handler;
        int priority = 0;
    };

    mutable std::mutex mutex_;
    std::vector<BeforeToolCallEntry>      before_tool_call_hooks_;
    std::vector<AfterToolCallEntry>       after_tool_call_hooks_;
    std::vector<ToolResultPersistEntry>   tool_result_persist_hooks_;
    std::vector<BeforeMessageWriteEntry>  before_message_write_hooks_;
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
/// Aligned with: bundled/command-logger pattern
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
