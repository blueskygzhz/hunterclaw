// ============================================================================
// tool_hooks.cc - Plugin Hook Runner Implementation
//
// Strictly aligned with OpenClaw plugins/hooks.ts createHookRunner():
//
// void hooks (parallel/sequential, fire-and-forget):
//   agent_end, llm_input, llm_output, before_compaction, after_compaction,
//   before_reset, message_received, message_sent, after_tool_call,
//   session_start, session_end, subagent_spawned, subagent_ended,
//   gateway_start, gateway_stop
//
// modifying hooks (sequential, results merged):
//   before_model_resolve  — merge: first defined wins (modelOverride/providerOverride)
//   before_prompt_build   — merge: systemPrompt last wins; context strings concatenated
//   before_agent_start    — merge: combines both above
//   message_sending       — merge: content/cancel last defined wins
//   before_tool_call      — merge: params/block/blockReason first defined wins
//   subagent_spawning     — merge: error status wins; threadBindingReady OR-ed
//   subagent_delivery_target — merge: first defined origin wins
//
// sync hooks (synchronous, sequential):
//   tool_result_persist   — each handler may replace message
//   before_message_write  — block short-circuits; message replaced per handler
// ============================================================================

#include "hunterclaw/tools/tool_hooks.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <fstream>

namespace hunterclaw {
namespace tools {

namespace {
    auto log() {
        static auto logger = spdlog::get("tools.hooks");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("tools.hooks");
        }
        return logger;
    }

    // Helper: concat two optional strings with newline separator
    // Aligned with OpenClaw concatOptionalTextSegments
    std::optional<std::string> concat_optional(
        const std::optional<std::string>& left,
        const std::optional<std::string>& right)
    {
        if (!left && !right) return std::nullopt;
        if (!left)  return right;
        if (!right) return left;
        return *left + "\n" + *right;
    }
} // anonymous namespace

// ============================================================================
// plugin_hook_name_to_string — inline implementation (no dependency on plugins lib)
// ============================================================================

std::string plugin_hook_name_to_string(PluginHookName name) {
    switch (name) {
        case PluginHookName::BEFORE_MODEL_RESOLVE:       return "before_model_resolve";
        case PluginHookName::BEFORE_PROMPT_BUILD:        return "before_prompt_build";
        case PluginHookName::BEFORE_AGENT_START:         return "before_agent_start";
        case PluginHookName::BEFORE_AGENT_REPLY:         return "before_agent_reply";
        case PluginHookName::LLM_INPUT:                  return "llm_input";
        case PluginHookName::LLM_OUTPUT:                 return "llm_output";
        case PluginHookName::AGENT_END:                  return "agent_end";
        case PluginHookName::BEFORE_COMPACTION:          return "before_compaction";
        case PluginHookName::AFTER_COMPACTION:           return "after_compaction";
        case PluginHookName::BEFORE_RESET:               return "before_reset";
        case PluginHookName::MESSAGE_RECEIVED:           return "message_received";
        case PluginHookName::MESSAGE_SENDING:            return "message_sending";
        case PluginHookName::MESSAGE_SENT:               return "message_sent";
        case PluginHookName::BEFORE_TOOL_CALL:           return "before_tool_call";
        case PluginHookName::AFTER_TOOL_CALL:            return "after_tool_call";
        case PluginHookName::TOOL_RESULT_PERSIST:        return "tool_result_persist";
        case PluginHookName::BEFORE_MESSAGE_WRITE:       return "before_message_write";
        case PluginHookName::SESSION_START:              return "session_start";
        case PluginHookName::SESSION_END:                return "session_end";
        case PluginHookName::SUBAGENT_SPAWNING:          return "subagent_spawning";
        case PluginHookName::SUBAGENT_DELIVERY_TARGET:   return "subagent_delivery_target";
        case PluginHookName::SUBAGENT_SPAWNED:           return "subagent_spawned";
        case PluginHookName::SUBAGENT_ENDED:             return "subagent_ended";
        case PluginHookName::GATEWAY_START:              return "gateway_start";
        case PluginHookName::GATEWAY_STOP:               return "gateway_stop";
        default:                                         return "unknown";
    }
}

// ============================================================================
// Registration helpers (macro to reduce boilerplate)
// ============================================================================

#define IMPL_REGISTER(method_name, vec_name, EntryType, HandlerType)          \
void ToolHookManager::method_name(                                             \
    const std::string& plugin_id, HandlerType handler, int priority)          \
{                                                                              \
    std::lock_guard<std::mutex> lock(mutex_);                                  \
    vec_name.push_back({plugin_id, std::move(handler), priority});             \
    sort_by_priority(vec_name);                                                \
    log()->debug("[hooks] registered {} hook from plugin '{}'",                \
        #method_name, plugin_id);                                              \
}

// Agent hooks
IMPL_REGISTER(register_before_model_resolve, before_model_resolve_hooks_, BeforeModelResolveEntry, BeforeModelResolveHandler)
IMPL_REGISTER(register_before_prompt_build,  before_prompt_build_hooks_,  BeforePromptBuildEntry,  BeforePromptBuildHandler)
IMPL_REGISTER(register_before_agent_start,   before_agent_start_hooks_,   BeforeAgentStartEntry,   BeforeAgentStartHandler)
IMPL_REGISTER(register_before_agent_reply,   before_agent_reply_hooks_,   BeforeAgentReplyEntry,   BeforeAgentReplyHandler)
IMPL_REGISTER(register_llm_input,            llm_input_hooks_,            LlmInputEntry,           LlmInputHandler)
IMPL_REGISTER(register_llm_output,           llm_output_hooks_,           LlmOutputEntry,          LlmOutputHandler)
IMPL_REGISTER(register_agent_end,            agent_end_hooks_,            AgentEndEntry,           AgentEndHandler)
IMPL_REGISTER(register_before_compaction,    before_compaction_hooks_,    BeforeCompactionEntry,   BeforeCompactionHandler)
IMPL_REGISTER(register_after_compaction,     after_compaction_hooks_,     AfterCompactionEntry,    AfterCompactionHandler)
IMPL_REGISTER(register_before_reset,         before_reset_hooks_,         BeforeResetEntry,        BeforeResetHandler)
// Message hooks
IMPL_REGISTER(register_message_received,     message_received_hooks_,     MessageReceivedEntry,    MessageReceivedHandler)
IMPL_REGISTER(register_message_sending,      message_sending_hooks_,      MessageSendingEntry,     MessageSendingHandler)
IMPL_REGISTER(register_message_sent,         message_sent_hooks_,         MessageSentEntry,        MessageSentHandler)
// Tool hooks
IMPL_REGISTER(register_before_tool_call,     before_tool_call_hooks_,     BeforeToolCallEntry,     BeforeToolCallHandler)
IMPL_REGISTER(register_after_tool_call,      after_tool_call_hooks_,      AfterToolCallEntry,      AfterToolCallHandler)
IMPL_REGISTER(register_tool_result_persist,  tool_result_persist_hooks_,  ToolResultPersistEntry,  ToolResultPersistHandler)
// Message write hooks
IMPL_REGISTER(register_before_message_write, before_message_write_hooks_, BeforeMessageWriteEntry, BeforeMessageWriteHandler)
// Session hooks
IMPL_REGISTER(register_session_start,        session_start_hooks_,        SessionStartEntry,       SessionStartHandler)
IMPL_REGISTER(register_session_end,          session_end_hooks_,          SessionEndEntry,         SessionEndHandler)
IMPL_REGISTER(register_subagent_spawning,    subagent_spawning_hooks_,    SubagentSpawningEntry,   SubagentSpawningHandler)
IMPL_REGISTER(register_subagent_delivery_target, subagent_delivery_target_hooks_, SubagentDeliveryTargetEntry, SubagentDeliveryTargetHandler)
IMPL_REGISTER(register_subagent_spawned,     subagent_spawned_hooks_,     SubagentSpawnedEntry,    SubagentSpawnedHandler)
IMPL_REGISTER(register_subagent_ended,       subagent_ended_hooks_,       SubagentEndedEntry,      SubagentEndedHandler)
// Gateway hooks
IMPL_REGISTER(register_gateway_start,        gateway_start_hooks_,        GatewayStartEntry,       GatewayStartHandler)
IMPL_REGISTER(register_gateway_stop,         gateway_stop_hooks_,         GatewayStopEntry,        GatewayStopHandler)

#undef IMPL_REGISTER

// ============================================================================
// unregister_plugin / clear_all_hooks
// ============================================================================

void ToolHookManager::unregister_plugin(const std::string& plugin_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    remove_plugin(before_model_resolve_hooks_,     plugin_id);
    remove_plugin(before_prompt_build_hooks_,      plugin_id);
    remove_plugin(before_agent_start_hooks_,       plugin_id);
    remove_plugin(llm_input_hooks_,                plugin_id);
    remove_plugin(llm_output_hooks_,               plugin_id);
    remove_plugin(agent_end_hooks_,                plugin_id);
    remove_plugin(before_compaction_hooks_,        plugin_id);
    remove_plugin(after_compaction_hooks_,         plugin_id);
    remove_plugin(before_reset_hooks_,             plugin_id);
    remove_plugin(message_received_hooks_,         plugin_id);
    remove_plugin(message_sending_hooks_,          plugin_id);
    remove_plugin(message_sent_hooks_,             plugin_id);
    remove_plugin(before_tool_call_hooks_,         plugin_id);
    remove_plugin(after_tool_call_hooks_,          plugin_id);
    remove_plugin(tool_result_persist_hooks_,      plugin_id);
    remove_plugin(before_message_write_hooks_,     plugin_id);
    remove_plugin(session_start_hooks_,            plugin_id);
    remove_plugin(session_end_hooks_,              plugin_id);
    remove_plugin(subagent_spawning_hooks_,        plugin_id);
    remove_plugin(subagent_delivery_target_hooks_, plugin_id);
    remove_plugin(subagent_spawned_hooks_,         plugin_id);
    remove_plugin(subagent_ended_hooks_,           plugin_id);
    remove_plugin(gateway_start_hooks_,            plugin_id);
    remove_plugin(gateway_stop_hooks_,             plugin_id);
    log()->debug("[hooks] unregistered all hooks for plugin '{}'", plugin_id);
}

void ToolHookManager::clear_all_hooks() {
    std::lock_guard<std::mutex> lock(mutex_);
    before_model_resolve_hooks_.clear();
    before_prompt_build_hooks_.clear();
    before_agent_start_hooks_.clear();
    llm_input_hooks_.clear();
    llm_output_hooks_.clear();
    agent_end_hooks_.clear();
    before_compaction_hooks_.clear();
    after_compaction_hooks_.clear();
    before_reset_hooks_.clear();
    message_received_hooks_.clear();
    message_sending_hooks_.clear();
    message_sent_hooks_.clear();
    before_tool_call_hooks_.clear();
    after_tool_call_hooks_.clear();
    tool_result_persist_hooks_.clear();
    before_message_write_hooks_.clear();
    session_start_hooks_.clear();
    session_end_hooks_.clear();
    subagent_spawning_hooks_.clear();
    subagent_delivery_target_hooks_.clear();
    subagent_spawned_hooks_.clear();
    subagent_ended_hooks_.clear();
    gateway_start_hooks_.clear();
    gateway_stop_hooks_.clear();
    log()->debug("[hooks] cleared all hooks");
}

// ============================================================================
// Void hook helper (aligned with OpenClaw runVoidHook)
// All handlers called sequentially; errors caught and logged.
// ============================================================================

#define IMPL_VOID_HOOK(run_method, vec_name, EventType, CtxType, hook_name_str) \
void ToolHookManager::run_method(const EventType& event, const CtxType& ctx)    \
{                                                                                \
    std::vector<decltype(vec_name)::value_type> hooks;                           \
    { std::lock_guard<std::mutex> lock(mutex_); hooks = vec_name; }              \
    if (hooks.empty()) return;                                                   \
    log()->debug("[hooks] running " hook_name_str " ({} handlers)", hooks.size()); \
    for (const auto& entry : hooks) {                                            \
        try { entry.handler(event, ctx); }                                       \
        catch (const std::exception& e) {                                        \
            log()->error("[hooks] " hook_name_str " handler from '{}' failed: {}", \
                entry.plugin_id, e.what());                                      \
        }                                                                        \
    }                                                                            \
}

// Agent void hooks
IMPL_VOID_HOOK(run_llm_input,          llm_input_hooks_,         PluginHookLlmInputEvent,         PluginHookAgentContext,   "llm_input")
IMPL_VOID_HOOK(run_llm_output,         llm_output_hooks_,        PluginHookLlmOutputEvent,        PluginHookAgentContext,   "llm_output")
IMPL_VOID_HOOK(run_agent_end,          agent_end_hooks_,         PluginHookAgentEndEvent,         PluginHookAgentContext,   "agent_end")
IMPL_VOID_HOOK(run_before_compaction,  before_compaction_hooks_, PluginHookBeforeCompactionEvent, PluginHookAgentContext,   "before_compaction")
IMPL_VOID_HOOK(run_after_compaction,   after_compaction_hooks_,  PluginHookAfterCompactionEvent,  PluginHookAgentContext,   "after_compaction")
IMPL_VOID_HOOK(run_before_reset,       before_reset_hooks_,      PluginHookBeforeResetEvent,      PluginHookAgentContext,   "before_reset")
// Message void hooks
IMPL_VOID_HOOK(run_message_received,   message_received_hooks_,  PluginHookMessageReceivedEvent,  PluginHookMessageContext, "message_received")
IMPL_VOID_HOOK(run_message_sent,       message_sent_hooks_,      PluginHookMessageSentEvent,      PluginHookMessageContext, "message_sent")
// Tool void hooks
IMPL_VOID_HOOK(run_after_tool_call,    after_tool_call_hooks_,   PluginHookAfterToolCallEvent,    PluginHookToolContext,    "after_tool_call")
// Session void hooks
IMPL_VOID_HOOK(run_session_start,      session_start_hooks_,     PluginHookSessionStartEvent,     PluginHookSessionContext, "session_start")
IMPL_VOID_HOOK(run_session_end,        session_end_hooks_,       PluginHookSessionEndEvent,       PluginHookSessionContext, "session_end")
IMPL_VOID_HOOK(run_subagent_spawned,   subagent_spawned_hooks_,  PluginHookSubagentSpawnedEvent,  PluginHookSubagentContext,"subagent_spawned")
IMPL_VOID_HOOK(run_subagent_ended,     subagent_ended_hooks_,    PluginHookSubagentEndedEvent,    PluginHookSubagentContext,"subagent_ended")
// Gateway void hooks
IMPL_VOID_HOOK(run_gateway_start,      gateway_start_hooks_,     PluginHookGatewayStartEvent,     PluginHookGatewayContext, "gateway_start")
IMPL_VOID_HOOK(run_gateway_stop,       gateway_stop_hooks_,      PluginHookGatewayStopEvent,      PluginHookGatewayContext, "gateway_stop")

#undef IMPL_VOID_HOOK

// ============================================================================
// run_before_model_resolve (modifying hook)
//
// Aligned with OpenClaw mergeBeforeModelResolve:
//   modelOverride:    acc?.modelOverride ?? next.modelOverride  (first defined wins)
//   providerOverride: acc?.providerOverride ?? next.providerOverride
// ============================================================================

std::optional<PluginHookBeforeModelResolveResult> ToolHookManager::run_before_model_resolve(
    const PluginHookBeforeModelResolveEvent& event,
    const PluginHookAgentContext& ctx)
{
    std::vector<BeforeModelResolveEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = before_model_resolve_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running before_model_resolve ({} handlers, sequential)", hooks.size());

    std::optional<PluginHookBeforeModelResolveResult> accumulated;

    for (const auto& entry : hooks) {
        try {
            auto result = entry.handler(event, ctx);
            if (!result) continue;
            if (!accumulated) {
                accumulated = result;
            } else {
                // Aligned with OpenClaw mergeBeforeModelResolve:
                // Keep the first defined override so higher-priority hooks win.
                if (!accumulated->model_override && result->model_override)
                    accumulated->model_override = result->model_override;
                if (!accumulated->provider_override && result->provider_override)
                    accumulated->provider_override = result->provider_override;
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] before_model_resolve handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }
    return accumulated;
}

// ============================================================================
// run_before_prompt_build (modifying hook)
//
// Aligned with OpenClaw mergeBeforePromptBuild:
//   systemPrompt:          next.systemPrompt ?? acc?.systemPrompt  (last wins)
//   prependContext:        concat(acc, next)
//   prependSystemContext:  concat(acc, next)
//   appendSystemContext:   concat(acc, next)
// ============================================================================

std::optional<PluginHookBeforePromptBuildResult> ToolHookManager::run_before_prompt_build(
    const PluginHookBeforePromptBuildEvent& event,
    const PluginHookAgentContext& ctx)
{
    std::vector<BeforePromptBuildEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = before_prompt_build_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running before_prompt_build ({} handlers, sequential)", hooks.size());

    std::optional<PluginHookBeforePromptBuildResult> accumulated;

    for (const auto& entry : hooks) {
        try {
            auto result = entry.handler(event, ctx);
            if (!result) continue;
            if (!accumulated) {
                accumulated = result;
            } else {
                // Aligned with OpenClaw mergeBeforePromptBuild:
                // systemPrompt: next wins (last defined)
                if (result->system_prompt)
                    accumulated->system_prompt = result->system_prompt;
                // context strings: concatenated
                accumulated->prepend_context        = concat_optional(accumulated->prepend_context,        result->prepend_context);
                accumulated->prepend_system_context = concat_optional(accumulated->prepend_system_context, result->prepend_system_context);
                accumulated->append_system_context  = concat_optional(accumulated->append_system_context,  result->append_system_context);
                accumulated->append_context         = concat_optional(accumulated->append_context,         result->append_context);
                // toolsAllow: intersect across hooks (OpenClaw mergeBeforePromptBuild
                // narrows the allowed set; presence of any list constrains the run).
                if (result->tools_allow) {
                    if (!accumulated->tools_allow) {
                        accumulated->tools_allow = result->tools_allow;
                    } else {
                        std::vector<std::string> intersection;
                        for (const auto& name : *accumulated->tools_allow) {
                            if (std::find(result->tools_allow->begin(),
                                          result->tools_allow->end(),
                                          name) != result->tools_allow->end()) {
                                intersection.push_back(name);
                            }
                        }
                        accumulated->tools_allow = std::move(intersection);
                    }
                }
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] before_prompt_build handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }
    return accumulated;
}

// ============================================================================
// run_before_agent_start (modifying hook)
//
// Aligned with OpenClaw runBeforeAgentStart:
//   Combines mergeBeforePromptBuild + mergeBeforeModelResolve
// ============================================================================

std::optional<PluginHookBeforeAgentStartResult> ToolHookManager::run_before_agent_start(
    const PluginHookBeforeAgentStartEvent& event,
    const PluginHookAgentContext& ctx)
{
    std::vector<BeforeAgentStartEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = before_agent_start_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running before_agent_start ({} handlers, sequential)", hooks.size());

    std::optional<PluginHookBeforeAgentStartResult> accumulated;

    for (const auto& entry : hooks) {
        try {
            auto result = entry.handler(event, ctx);
            if (!result) continue;
            if (!accumulated) {
                accumulated = result;
            } else {
                // Aligned with OpenClaw: { ...mergeBeforePromptBuild, ...mergeBeforeModelResolve }
                // prompt build fields
                if (result->system_prompt)
                    accumulated->system_prompt = result->system_prompt;
                accumulated->prepend_context        = concat_optional(accumulated->prepend_context,        result->prepend_context);
                accumulated->prepend_system_context = concat_optional(accumulated->prepend_system_context, result->prepend_system_context);
                accumulated->append_system_context  = concat_optional(accumulated->append_system_context,  result->append_system_context);
                // model resolve fields (first defined wins)
                if (!accumulated->model_override && result->model_override)
                    accumulated->model_override = result->model_override;
                if (!accumulated->provider_override && result->provider_override)
                    accumulated->provider_override = result->provider_override;
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] before_agent_start handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }
    return accumulated;
}

// ============================================================================
// run_before_agent_reply (claiming hook)
//
// Aligned with OpenClaw runBeforeAgentReply = runClaimingHook:
//   Iterate handlers in registration/priority order. The FIRST handler that
//   returns { handled: true } claims the turn; its result is returned and no
//   further handlers run. Handlers returning nullopt or handled==false are
//   skipped. Handler exceptions are logged and skipped (never claim on throw).
// ============================================================================

std::optional<PluginHookBeforeAgentReplyResult> ToolHookManager::run_before_agent_reply(
    const PluginHookBeforeAgentReplyEvent& event,
    const PluginHookAgentContext& ctx)
{
    std::vector<BeforeAgentReplyEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = before_agent_reply_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running before_agent_reply ({} handlers, claiming)", hooks.size());

    for (const auto& entry : hooks) {
        try {
            auto result = entry.handler(event, ctx);
            if (result && result->handled) {
                log()->info("[hooks] before_agent_reply claimed by '{}'{}",
                    entry.plugin_id,
                    result->reason ? (" reason=" + *result->reason) : std::string{});
                return result;  // first claim wins — short-circuit
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] before_agent_reply handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }
    return std::nullopt;  // no handler claimed the turn
}

// ============================================================================
// run_message_sending (modifying hook)
//
// Aligned with OpenClaw runMessageSending merge:
//   content: next.content ?? acc?.content  (last defined wins)
//   cancel:  next.cancel  ?? acc?.cancel
// ============================================================================

std::optional<PluginHookMessageSendingResult> ToolHookManager::run_message_sending(
    const PluginHookMessageSendingEvent& event,
    const PluginHookMessageContext& ctx)
{
    std::vector<MessageSendingEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = message_sending_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running message_sending ({} handlers, sequential)", hooks.size());

    std::optional<PluginHookMessageSendingResult> accumulated;

    for (const auto& entry : hooks) {
        try {
            auto result = entry.handler(event, ctx);
            if (!result) continue;
            if (!accumulated) {
                accumulated = result;
            } else {
                // Aligned with OpenClaw: content/cancel last defined wins
                if (result->content)  accumulated->content = result->content;
                if (result->cancel)   accumulated->cancel  = result->cancel;
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] message_sending handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }
    return accumulated;
}

// ============================================================================
// run_before_tool_call (modifying hook)
//
// Aligned with OpenClaw runModifyingHook("before_tool_call", ...):
//   params:      next.params ?? acc?.params      (first defined wins)
//   block:       next.block ?? acc?.block
//   blockReason: next.blockReason ?? acc?.blockReason
// ============================================================================

std::optional<PluginHookBeforeToolCallResult> ToolHookManager::run_before_tool_call(
    const PluginHookBeforeToolCallEvent& event,
    const PluginHookToolContext& ctx)
{
    std::vector<BeforeToolCallEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = before_tool_call_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running before_tool_call ({} handlers, sequential)", hooks.size());

    std::optional<PluginHookBeforeToolCallResult> accumulated;

    for (const auto& entry : hooks) {
        try {
            auto result = entry.handler(event, ctx);
            if (!result) continue;
            if (!accumulated) {
                accumulated = result;
            } else {
                // Aligned with OpenClaw merge: first defined wins
                if (!accumulated->params_json  && result->params_json)  accumulated->params_json  = result->params_json;
                if (!accumulated->block        && result->block)         accumulated->block        = result->block;
                if (!accumulated->block_reason && result->block_reason)  accumulated->block_reason = result->block_reason;
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] before_tool_call handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }
    return accumulated;
}

// ============================================================================
// run_subagent_spawning (modifying hook)
//
// Aligned with OpenClaw mergeSubagentSpawningResult:
//   if acc.status == "error" → return acc (error wins)
//   if next.status == "error" → return next
//   otherwise: status="ok", threadBindingReady = acc || next
// ============================================================================

std::optional<PluginHookSubagentSpawningResult> ToolHookManager::run_subagent_spawning(
    const PluginHookSubagentSpawningEvent& event,
    const PluginHookSubagentContext& ctx)
{
    std::vector<SubagentSpawningEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = subagent_spawning_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running subagent_spawning ({} handlers, sequential)", hooks.size());

    std::optional<PluginHookSubagentSpawningResult> accumulated;

    for (const auto& entry : hooks) {
        try {
            auto result = entry.handler(event, ctx);
            if (!result) continue;
            if (!accumulated) {
                accumulated = result;
            } else {
                // Aligned with OpenClaw mergeSubagentSpawningResult:
                // error wins; threadBindingReady OR-ed
                if (!accumulated->ok) {
                    // error wins — keep accumulated
                } else if (!result->ok) {
                    accumulated = result;
                } else {
                    // both ok: OR threadBindingReady
                    accumulated->thread_binding_ready =
                        accumulated->thread_binding_ready || result->thread_binding_ready;
                }
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] subagent_spawning handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }
    return accumulated;
}

// ============================================================================
// run_subagent_delivery_target (modifying hook)
//
// Aligned with OpenClaw mergeSubagentDeliveryTargetResult:
//   if acc.origin → return acc (first defined wins)
//   otherwise → return next
// ============================================================================

std::optional<PluginHookSubagentDeliveryTargetResult> ToolHookManager::run_subagent_delivery_target(
    const PluginHookSubagentDeliveryTargetEvent& event,
    const PluginHookSubagentContext& ctx)
{
    std::vector<SubagentDeliveryTargetEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = subagent_delivery_target_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running subagent_delivery_target ({} handlers, sequential)", hooks.size());

    std::optional<PluginHookSubagentDeliveryTargetResult> accumulated;

    for (const auto& entry : hooks) {
        try {
            auto result = entry.handler(event, ctx);
            if (!result) continue;
            if (!accumulated) {
                accumulated = result;
            } else {
                // Aligned with OpenClaw mergeSubagentDeliveryTargetResult:
                // first defined origin wins
                if (!accumulated->origin && result->origin)
                    accumulated->origin = result->origin;
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] subagent_delivery_target handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }
    return accumulated;
}

// ============================================================================
// run_tool_result_persist (sync hook)
//
// Aligned with OpenClaw runToolResultPersist():
//   - Synchronous, sequential
//   - Each handler may return { message } to replace the current message
//   - Guards against async handlers (this hook is sync-only)
//   - Errors caught and logged
// ============================================================================

std::optional<PluginHookToolResultPersistResult> ToolHookManager::run_tool_result_persist(
    const PluginHookToolResultPersistEvent& event,
    const PluginHookToolResultPersistContext& ctx)
{
    std::vector<ToolResultPersistEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = tool_result_persist_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running tool_result_persist ({} handlers)", hooks.size());

    // Aligned with OpenClaw: let current = event.message_json;
    std::string current = event.message_json;
    bool modified = false;

    for (const auto& entry : hooks) {
        try {
            PluginHookToolResultPersistEvent current_event = event;
            current_event.message_json = current;

            auto result = entry.handler(current_event, ctx);

            if (result && result->message_json) {
                // Aligned with OpenClaw: const next = out?.message; if (next) { current = next; }
                current = *result->message_json;
                modified = true;
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] tool_result_persist handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }

    if (modified) {
        PluginHookToolResultPersistResult result;
        result.message_json = current;
        return result;
    }
    return std::nullopt;
}

// ============================================================================
// run_before_message_write (sync hook)
//
// Aligned with OpenClaw runBeforeMessageWrite():
//   - Synchronous, sequential
//   - If any handler returns { block: true }, return immediately with block=true
//   - If handler returns { message }, use it for subsequent handlers
//   - If message was modified, return { message: current }
//   - Errors caught and logged
// ============================================================================

std::optional<PluginHookBeforeMessageWriteResult> ToolHookManager::run_before_message_write(
    const PluginHookBeforeMessageWriteEvent& event,
    const std::string& agent_id,
    const std::string& session_key)
{
    std::vector<BeforeMessageWriteEntry> hooks;
    { std::lock_guard<std::mutex> lock(mutex_); hooks = before_message_write_hooks_; }
    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running before_message_write ({} handlers)", hooks.size());

    // Aligned with OpenClaw: let current = event.message_json;
    std::string current = event.message_json;
    bool modified = false;

    for (const auto& entry : hooks) {
        try {
            PluginHookBeforeMessageWriteEvent current_event = event;
            current_event.message_json = current;

            auto result = entry.handler(current_event, agent_id, session_key);

            if (result) {
                // Aligned with OpenClaw: if (result?.block) { return { block: true }; }
                if (result->block) {
                    PluginHookBeforeMessageWriteResult blocked;
                    blocked.block = true;
                    return blocked;
                }
                // Aligned with OpenClaw: if (result?.message) { current = result.message_json; }
                if (result->message_json) {
                    current = *result->message_json;
                    modified = true;
                }
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] before_message_write handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }

    // Aligned with OpenClaw: if (current !== event.message) { return { message: current }; }
    if (modified) {
        PluginHookBeforeMessageWriteResult result;
        result.block        = false;
        result.message_json = current;
        return result;
    }
    return std::nullopt;
}

// ============================================================================
// Query methods
// ============================================================================

bool ToolHookManager::has_hooks(PluginHookName name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (name) {
        case PluginHookName::BEFORE_MODEL_RESOLVE:     return !before_model_resolve_hooks_.empty();
        case PluginHookName::BEFORE_PROMPT_BUILD:      return !before_prompt_build_hooks_.empty();
        case PluginHookName::BEFORE_AGENT_START:       return !before_agent_start_hooks_.empty();
        case PluginHookName::BEFORE_AGENT_REPLY:       return !before_agent_reply_hooks_.empty();
        case PluginHookName::LLM_INPUT:                return !llm_input_hooks_.empty();
        case PluginHookName::LLM_OUTPUT:               return !llm_output_hooks_.empty();
        case PluginHookName::AGENT_END:                return !agent_end_hooks_.empty();
        case PluginHookName::BEFORE_COMPACTION:        return !before_compaction_hooks_.empty();
        case PluginHookName::AFTER_COMPACTION:         return !after_compaction_hooks_.empty();
        case PluginHookName::BEFORE_RESET:             return !before_reset_hooks_.empty();
        case PluginHookName::MESSAGE_RECEIVED:         return !message_received_hooks_.empty();
        case PluginHookName::MESSAGE_SENDING:          return !message_sending_hooks_.empty();
        case PluginHookName::MESSAGE_SENT:             return !message_sent_hooks_.empty();
        case PluginHookName::BEFORE_TOOL_CALL:         return !before_tool_call_hooks_.empty();
        case PluginHookName::AFTER_TOOL_CALL:          return !after_tool_call_hooks_.empty();
        case PluginHookName::TOOL_RESULT_PERSIST:      return !tool_result_persist_hooks_.empty();
        case PluginHookName::BEFORE_MESSAGE_WRITE:     return !before_message_write_hooks_.empty();
        case PluginHookName::SESSION_START:            return !session_start_hooks_.empty();
        case PluginHookName::SESSION_END:              return !session_end_hooks_.empty();
        case PluginHookName::SUBAGENT_SPAWNING:        return !subagent_spawning_hooks_.empty();
        case PluginHookName::SUBAGENT_DELIVERY_TARGET: return !subagent_delivery_target_hooks_.empty();
        case PluginHookName::SUBAGENT_SPAWNED:         return !subagent_spawned_hooks_.empty();
        case PluginHookName::SUBAGENT_ENDED:           return !subagent_ended_hooks_.empty();
        case PluginHookName::GATEWAY_START:            return !gateway_start_hooks_.empty();
        case PluginHookName::GATEWAY_STOP:             return !gateway_stop_hooks_.empty();
    }
    return false;
}

size_t ToolHookManager::hook_count(PluginHookName name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (name) {
        case PluginHookName::BEFORE_MODEL_RESOLVE:     return before_model_resolve_hooks_.size();
        case PluginHookName::BEFORE_PROMPT_BUILD:      return before_prompt_build_hooks_.size();
        case PluginHookName::BEFORE_AGENT_START:       return before_agent_start_hooks_.size();
        case PluginHookName::BEFORE_AGENT_REPLY:       return before_agent_reply_hooks_.size();
        case PluginHookName::LLM_INPUT:                return llm_input_hooks_.size();
        case PluginHookName::LLM_OUTPUT:               return llm_output_hooks_.size();
        case PluginHookName::AGENT_END:                return agent_end_hooks_.size();
        case PluginHookName::BEFORE_COMPACTION:        return before_compaction_hooks_.size();
        case PluginHookName::AFTER_COMPACTION:         return after_compaction_hooks_.size();
        case PluginHookName::BEFORE_RESET:             return before_reset_hooks_.size();
        case PluginHookName::MESSAGE_RECEIVED:         return message_received_hooks_.size();
        case PluginHookName::MESSAGE_SENDING:          return message_sending_hooks_.size();
        case PluginHookName::MESSAGE_SENT:             return message_sent_hooks_.size();
        case PluginHookName::BEFORE_TOOL_CALL:         return before_tool_call_hooks_.size();
        case PluginHookName::AFTER_TOOL_CALL:          return after_tool_call_hooks_.size();
        case PluginHookName::TOOL_RESULT_PERSIST:      return tool_result_persist_hooks_.size();
        case PluginHookName::BEFORE_MESSAGE_WRITE:     return before_message_write_hooks_.size();
        case PluginHookName::SESSION_START:            return session_start_hooks_.size();
        case PluginHookName::SESSION_END:              return session_end_hooks_.size();
        case PluginHookName::SUBAGENT_SPAWNING:        return subagent_spawning_hooks_.size();
        case PluginHookName::SUBAGENT_DELIVERY_TARGET: return subagent_delivery_target_hooks_.size();
        case PluginHookName::SUBAGENT_SPAWNED:         return subagent_spawned_hooks_.size();
        case PluginHookName::SUBAGENT_ENDED:           return subagent_ended_hooks_.size();
        case PluginHookName::GATEWAY_START:            return gateway_start_hooks_.size();
        case PluginHookName::GATEWAY_STOP:             return gateway_stop_hooks_.size();
    }
    return 0;
}

bool ToolHookManager::has_before_tool_call_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !before_tool_call_hooks_.empty();
}

bool ToolHookManager::has_after_tool_call_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !after_tool_call_hooks_.empty();
}

bool ToolHookManager::has_tool_result_persist_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !tool_result_persist_hooks_.empty();
}

bool ToolHookManager::has_before_message_write_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !before_message_write_hooks_.empty();
}

size_t ToolHookManager::before_tool_call_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return before_tool_call_hooks_.size();
}

size_t ToolHookManager::after_tool_call_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return after_tool_call_hooks_.size();
}

std::vector<std::string> ToolHookManager::list_plugin_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    auto add_ids = [&ids](const auto& vec) {
        for (const auto& e : vec) {
            if (std::find(ids.begin(), ids.end(), e.plugin_id) == ids.end())
                ids.push_back(e.plugin_id);
        }
    };
    add_ids(before_model_resolve_hooks_);
    add_ids(before_prompt_build_hooks_);
    add_ids(before_agent_start_hooks_);
    add_ids(llm_input_hooks_);
    add_ids(llm_output_hooks_);
    add_ids(agent_end_hooks_);
    add_ids(before_compaction_hooks_);
    add_ids(after_compaction_hooks_);
    add_ids(before_reset_hooks_);
    add_ids(message_received_hooks_);
    add_ids(message_sending_hooks_);
    add_ids(message_sent_hooks_);
    add_ids(before_tool_call_hooks_);
    add_ids(after_tool_call_hooks_);
    add_ids(tool_result_persist_hooks_);
    add_ids(before_message_write_hooks_);
    add_ids(session_start_hooks_);
    add_ids(session_end_hooks_);
    add_ids(subagent_spawning_hooks_);
    add_ids(subagent_delivery_target_hooks_);
    add_ids(subagent_spawned_hooks_);
    add_ids(subagent_ended_hooks_);
    add_ids(gateway_start_hooks_);
    add_ids(gateway_stop_hooks_);
    return ids;
}

// ============================================================================
// Built-in hooks (aligned with OpenClaw bundled hooks pattern)
// ============================================================================

namespace builtin_hooks {

BeforeToolCallHandler create_param_validation_hook() {
    return [](const PluginHookBeforeToolCallEvent& event,
              const PluginHookToolContext& /*ctx*/)
        -> std::optional<PluginHookBeforeToolCallResult>
    {
        // params_json is a JSON string; basic validation via string check
        const std::string& params = event.params_json;
        if (params.empty() || params.front() != '{') {
            PluginHookBeforeToolCallResult result;
            result.block = true;
            result.block_reason = "Tool parameters must be a JSON object";
            return result;
        }
        if (event.tool_name == "exec") {
            if (params.find("\"command\"") == std::string::npos) {
                PluginHookBeforeToolCallResult result;
                result.block = true;
                result.block_reason = "exec tool requires 'command' parameter";
                return result;
            }
        }
        if (event.tool_name == "read_file" || event.tool_name == "write_file") {
            if (params.find("\"path\"") == std::string::npos) {
                PluginHookBeforeToolCallResult result;
                result.block = true;
                result.block_reason = event.tool_name + " requires 'path' parameter";
                return result;
            }
        }
        return std::nullopt;
    };
}

AfterToolCallHandler create_logging_hook() {
    return [](const PluginHookAfterToolCallEvent& event,
              const PluginHookToolContext& /*ctx*/)
    {
        auto logger = spdlog::get("tools.hooks");
        if (!logger) return;
        if (!event.error || event.error->empty()) {
            logger->info("[hooks] tool '{}' completed in {:.2f}ms",
                event.tool_name, event.duration_ms.value_or(0.0));
        } else {
            logger->error("[hooks] tool '{}' failed after {:.2f}ms: {}",
                event.tool_name, event.duration_ms.value_or(0.0), *event.error);
        }
    };
}

AfterToolCallHandler create_performance_monitoring_hook() {
    struct ToolStats {
        int call_count = 0;
        double total_duration_ms = 0.0;
        double min_duration_ms = std::numeric_limits<double>::max();
        double max_duration_ms = 0.0;
    };
    static std::map<std::string, ToolStats> stats_map;
    static std::mutex stats_mutex;

    return [](const PluginHookAfterToolCallEvent& event,
              const PluginHookToolContext& /*ctx*/)
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        auto& stats = stats_map[event.tool_name];
        double dur = event.duration_ms.value_or(0.0);
        stats.call_count++;
        stats.total_duration_ms += dur;
        stats.min_duration_ms = std::min(stats.min_duration_ms, dur);
        stats.max_duration_ms = std::max(stats.max_duration_ms, dur);
        if (stats.call_count % 100 == 0) {
            double avg = stats.total_duration_ms / stats.call_count;
            auto logger = spdlog::get("tools.hooks");
            if (logger) {
                logger->info("[hooks] tool '{}' stats: {} calls, avg={:.2f}ms, min={:.2f}ms, max={:.2f}ms",
                    event.tool_name, stats.call_count, avg,
                    stats.min_duration_ms, stats.max_duration_ms);
            }
        }
    };
}

AfterToolCallHandler create_error_collection_hook() {
    struct ErrorStats { int total_calls = 0; int error_count = 0; };
    static std::map<std::string, ErrorStats> error_map;
    static std::mutex error_mutex;

    return [](const PluginHookAfterToolCallEvent& event,
              const PluginHookToolContext& /*ctx*/)
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        auto& stats = error_map[event.tool_name];
        stats.total_calls++;
        if (event.error && !event.error->empty()) {
            stats.error_count++;
            double error_rate = static_cast<double>(stats.error_count) / stats.total_calls;
            if (error_rate > 0.5 && stats.total_calls >= 10) {
                auto logger = spdlog::get("tools.hooks");
                if (logger) {
                    logger->warn("[hooks] tool '{}' has high error rate: {:.1f}% ({}/{})",
                        event.tool_name, error_rate * 100,
                        stats.error_count, stats.total_calls);
                }
            }
        }
    };
}

AfterToolCallHandler create_audit_hook(const AuditHookConfig& config) {
    return [config](const PluginHookAfterToolCallEvent& event,
                    const PluginHookToolContext& /*ctx*/)
    {
        bool is_sensitive = std::find(
            config.sensitive_tools.begin(), config.sensitive_tools.end(),
            event.tool_name) != config.sensitive_tools.end();
        if (!is_sensitive) return;
        std::ofstream audit_log(config.audit_log_path, std::ios::app);
        if (audit_log.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto ts  = std::chrono::system_clock::to_time_t(now);
            audit_log << "[" << std::ctime(&ts) << "] "
                      << "tool=" << event.tool_name
                      << " duration=" << event.duration_ms.value_or(0.0) << "ms"
                      << " error=" << event.error.value_or("none") << "\n";
        }
    };
}

BeforeToolCallHandler create_rate_limit_hook(const RateLimitConfig& config) {
    struct RateLimitState {
        std::map<std::string, int> tool_call_counts;
        int64_t window_start_minute = 0;
        int total_calls_in_window = 0;
    };
    static RateLimitState state;
    static std::mutex rate_mutex;

    return [config](const PluginHookBeforeToolCallEvent& event,
                    const PluginHookToolContext& /*ctx*/)
        -> std::optional<PluginHookBeforeToolCallResult>
    {
        std::lock_guard<std::mutex> lock(rate_mutex);
        auto now_ns = std::chrono::system_clock::now().time_since_epoch().count();
        int64_t current_minute = now_ns / 60000000000LL;
        if (current_minute != state.window_start_minute) {
            state.window_start_minute = current_minute;
            state.total_calls_in_window = 0;
            state.tool_call_counts.clear();
        }
        if (state.total_calls_in_window >= config.max_calls_per_minute) {
            PluginHookBeforeToolCallResult result;
            result.block = true;
            result.block_reason = "Global rate limit exceeded: " +
                std::to_string(config.max_calls_per_minute) + " calls/min";
            return result;
        }
        int tool_calls = state.tool_call_counts[event.tool_name];
        if (tool_calls >= config.max_calls_per_tool_per_minute) {
            PluginHookBeforeToolCallResult result;
            result.block = true;
            result.block_reason = "Tool '" + event.tool_name + "' rate limit exceeded: " +
                std::to_string(config.max_calls_per_tool_per_minute) + " calls/min";
            return result;
        }
        state.total_calls_in_window++;
        state.tool_call_counts[event.tool_name]++;
        return std::nullopt;
    };
}

} // namespace builtin_hooks

// ============================================================================
// Param normalization (aligned with OpenClaw)
// ============================================================================

std::string to_camel_case(const std::string& snake_case) {
    std::string result;
    bool capitalize_next = false;
    for (char c : snake_case) {
        if (c == '_') {
            capitalize_next = true;
        } else if (capitalize_next) {
            result += static_cast<char>(std::toupper(c));
            capitalize_next = false;
        } else {
            result += c;
        }
    }
    return result;
}

std::string to_snake_case(const std::string& camel_case) {
    std::string result;
    for (size_t i = 0; i < camel_case.size(); ++i) {
        char c = camel_case[i];
        if (std::isupper(c)) {
            if (i > 0) result += '_';
            result += static_cast<char>(std::tolower(c));
        } else {
            result += c;
        }
    }
    return result;
}

core::JsonValue normalize_tool_params(
    const core::JsonValue& params,
    const ParamNormalizationConfig& config)
{
    if (!params.is_object()) return params;

    core::JsonValue normalized = core::JsonValue::object();

    for (auto it = params.begin(); it != params.end(); ++it) {
        std::string key = it->first;
        core::JsonValue value = *it->second;

        if (config.snake_to_camel) {
            key = to_camel_case(key);
        } else if (config.camel_to_snake) {
            key = to_snake_case(key);
        }

        if (config.trim_strings && value.is_string()) {
            std::string str = value.get<std::string>();
            size_t start = str.find_first_not_of(" \t\n\r");
            size_t end   = str.find_last_not_of(" \t\n\r");
            if (start != std::string::npos && end != std::string::npos) {
                str = str.substr(start, end - start + 1);
            } else {
                str.clear();
            }
            value = core::JsonValue(str);
        }

        if (config.remove_nulls && value.is_null()) continue;

        if (value.is_object()) {
            value = normalize_tool_params(value, config);
        }

        normalized[key] = value;
    }

    return normalized;
}

// ============================================================================
// Global ToolHookManager
// Aligned with OpenClaw getGlobalHookRunner() / setGlobalHookRunner()
// ============================================================================

namespace {
    // Process-wide hook manager pointer (set by set_global_hook_manager)
    std::atomic<ToolHookManager*> g_hook_manager{nullptr};
    // Lazily-initialized owned instance (used by require_global_hook_manager)
    std::unique_ptr<ToolHookManager> g_owned_hook_manager;
    std::once_flag g_hook_manager_init_flag;
}

ToolHookManager* get_global_hook_manager() {
    return g_hook_manager.load(std::memory_order_acquire);
}

void set_global_hook_manager(ToolHookManager* mgr) {
    g_hook_manager.store(mgr, std::memory_order_release);
}

ToolHookManager& require_global_hook_manager() {
    std::call_once(g_hook_manager_init_flag, []() {
        if (!g_hook_manager.load(std::memory_order_acquire)) {
            g_owned_hook_manager = std::make_unique<ToolHookManager>();
            g_hook_manager.store(g_owned_hook_manager.get(), std::memory_order_release);
        }
    });
    return *g_hook_manager.load(std::memory_order_acquire);
}

} // namespace tools
} // namespace hunterclaw
