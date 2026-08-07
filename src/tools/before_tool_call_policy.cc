// ============================================================================
// before_tool_call_policy.cc - Unified before_tool_call policy chain
//
// Strict port of OpenClaw runBeforeToolCallHook() and its execution wiring.
// See before_tool_call_policy.h for the alignment map.
// ============================================================================

#include "hunterclaw/tools/before_tool_call_policy.h"

#include "hunterclaw/tools/tool_hooks.h"
#include "hunterclaw/security/dangerous_tools.h"
#include "hunterclaw/core/logger.h"

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace tools {

namespace {

std::shared_ptr<spdlog::logger> log() {
    return core::get_logger("before_tool_call");
}

// OpenClaw: mergeParamsWithApprovalOverrides / hook params merge.
// first-defined-wins already handled inside the hook runner; here we simply
// replace when the hook supplied a new params object.
core::JsonValue apply_hook_params(const core::JsonValue& current,
                                  const std::optional<std::string>& params_json) {
    if (!params_json.has_value()) return current;
    auto parsed = core::JsonValue::parse(*params_json);
    if (parsed.ok && parsed.value) return std::move(*parsed.value);
    return current;
}

}  // namespace

// ── build_blocked_tool_result ───────────────────────────────────────────────
ToolResult build_blocked_tool_result(const std::string& reason,
                                     const std::string& denied,
                                     const std::string& tool_call_id) {
    ToolResult result;
    result.success = false;
    result.error = reason;

    // OpenClaw buildBlockedToolResult(): content carries the reason text,
    // details carries the structured blocked marker.
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = reason;
    result.content.push_back(std::move(content));

    core::JsonValue details = core::JsonValue::object();
    details["status"] = std::string("blocked");
    details["deniedReason"] = denied;
    details["reason"] = reason;
    if (!tool_call_id.empty()) details["toolCallId"] = tool_call_id;
    result.details = std::move(details);
    return result;
}

// ── is_pre_execution_blocked_result ─────────────────────────────────────────
bool is_pre_execution_blocked_result(const ToolResult& result) {
    if (result.success) return false;
    if (!result.details.is_object()) return false;
    if (!result.details.contains("status")) return false;
    const auto& status = result.details["status"];
    return status.is_string() && status.as_string() == "blocked";
}

// ── validate_tool_execution_params ──────────────────────────────────────────
std::string validate_tool_execution_params(const Tool& tool,
                                            const core::JsonValue& params) {
    Tool::Schema schema = tool.parameters();
    if (schema.required.empty()) return "";
    if (!params.is_object()) {
        // OpenClaw treats non-object params where an object is expected as invalid
        // only when required fields exist.
        return "invalid params: expected object with required fields";
    }
    for (const auto& key : schema.required) {
        if (!params.contains(key)) {
            return "missing required parameter: " + key;
        }
    }
    return "";
}

// ── run_before_tool_call_policy ─────────────────────────────────────────────
HookOutcome run_before_tool_call_policy(const BeforeToolCallContext& ctx,
                                        const core::JsonValue& original_params) {
    // ── Stage 0: owner-only gate ─────────────────────────────────────────────
    // 🎯 对齐 OpenClaw: senderIsOwner === false 时对GATEWAY_OWNER_ONLY_CORE_TOOLS
    // (以及 ownerOnly:true 的工具) 注入 deny。非 owner 调用 owner-only 工具 => veto。
    if (!ctx.sender_is_owner) {
        const bool owner_only =
            ctx.tool_is_owner_only ||
            security::is_gateway_owner_only_core_tool(ctx.tool_name);
        if (owner_only) {
            std::string reason =
                "Tool '" + ctx.tool_name + "' requires owner identity";
            log()->warn("Tool '{}' denied: owner-only, sender is not owner",
                        ctx.tool_name);
            return HookOutcome::veto(reason, denied_reason::kOwnerOnly,
                                     original_params);
        }
    }

    // ── Stage 1: tool-loop admission ─────────────────────────────────────────
    // OpenClaw policy.ts:131-144 admitSingleToolCallLoop -> critical => veto{tool-loop}.
    // In HunterClaw, loop detection/record is performed by the caller (react_engine
    // owns LoopDetectionState) to avoid a tools<->react link cycle. Callers therefore
    // pass loop_state == nullptr; this stage is intentionally a no-op here.
    (void)ctx.loop_state;

    // ── Stage 2: before_tool_call hook runner ────────────────────────────────
    core::JsonValue effective_params = original_params;
    if (ctx.hook_manager && ctx.hook_manager->has_before_tool_call_hooks()) {
        try {
            PluginHookBeforeToolCallEvent event;
            event.tool_name = ctx.tool_name;
            event.params_json = effective_params.to_string();
            if (!ctx.run_id.empty()) event.run_id = ctx.run_id;
            if (!ctx.tool_call_id.empty()) event.tool_call_id = ctx.tool_call_id;

            PluginHookToolContext tool_ctx;
            tool_ctx.tool_name = ctx.tool_name;
            if (!ctx.agent_id.empty()) tool_ctx.agent_id = ctx.agent_id;
            if (!ctx.session_key.empty()) tool_ctx.session_key = ctx.session_key;
            if (!ctx.session_id.empty()) tool_ctx.session_id = ctx.session_id;
            if (!ctx.run_id.empty()) tool_ctx.run_id = ctx.run_id;
            if (!ctx.tool_call_id.empty()) tool_ctx.tool_call_id = ctx.tool_call_id;

            auto hook_result = ctx.hook_manager->run_before_tool_call(event, tool_ctx);
            if (hook_result.has_value()) {
                // OpenClaw policy.ts:331-339 hookResult?.block -> veto{plugin-before-tool-call}
                if (hook_result->block) {
                    std::string reason = hook_result->block_reason.value_or(
                        "Tool call blocked by plugin hook");
                    log()->warn("Tool '{}' vetoed by before_tool_call hook: {}",
                                ctx.tool_name, reason);
                    return HookOutcome::veto(reason, denied_reason::kPluginBeforeToolCall,
                                             effective_params);
                }
                // OpenClaw policy.ts:365-367 hookResult?.params -> merge.
                effective_params = apply_hook_params(effective_params, hook_result->params_json);
            }
        } catch (const std::exception& e) {
            // OpenClaw policy.ts:387-398: hook threw -> failure{disposition}.
            // Without an abort signal we classify as generic "error".
            log()->error("before_tool_call hook failed: tool={} error={}",
                         ctx.tool_name, e.what());
            return HookOutcome::failure(
                "Tool call blocked because before_tool_call hook failed",
                disposition::kError,
                denied_reason::kPluginBeforeToolCall,
                original_params);
        }
    }

    // ── Stage 3: post-hook cancellation check ────────────────────────────────
    // OpenClaw wrapper.ts: signal.throwIfAborted() after hook, before execute.
    if (ctx.aborted && ctx.aborted->load()) {
        return HookOutcome::failure(
            "Tool call cancelled before execution",
            disposition::kCancelled,
            denied_reason::kPluginBeforeToolCall,
            effective_params);
    }

    return HookOutcome::pass(std::move(effective_params));
}

}  // namespace tools
}  // namespace hunterclaw
