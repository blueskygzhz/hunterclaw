// Aligned with openclaw: src/auto-reply/reply/get-reply-fast-path.ts (295行)
// Fast path 优化 — 在测试/轻量环境下跳过重量级初始化
#pragma once

#include "hunterclaw/auto_reply/inbound_context.h"
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Config resolution (对齐 OC: resolveGetReplyConfig)
// ============================================================================

/// Resolve the effective reply config, applying merge-patch if override is provided.
/// 对齐 OC: resolveGetReplyConfig (get-reply-fast-path.ts:96-114)
struct ResolveGetReplyConfigParams {
    std::string base_config_json;
    std::optional<std::string> config_override_json;
    bool is_fast_test_env = false;
};

std::string resolve_get_reply_config(const ResolveGetReplyConfigParams& params);

// ============================================================================
// Fast test bootstrap/runtime detection
// 对齐 OC: shouldUseReplyFastTestBootstrap/Runtime
// ============================================================================

/// 对齐 OC: shouldUseReplyFastTestBootstrap (get-reply-fast-path.ts:116-125)
bool should_use_reply_fast_test_bootstrap(
    bool is_fast_test_env,
    bool is_complete_reply_config,
    bool uses_full_reply_runtime);

/// 对齐 OC: shouldUseReplyFastTestRuntime (get-reply-fast-path.ts:127-134)
bool should_use_reply_fast_test_runtime(
    bool is_fast_test_env,
    bool is_complete_reply_config,
    bool uses_full_reply_runtime);

// ============================================================================
// Fast directive execution gate
// 对齐 OC: shouldUseReplyFastDirectiveExecution (get-reply-fast-path.ts:136-152)
// ============================================================================

struct FastDirectiveExecutionParams {
    bool is_fast_test_bootstrap = false;
    bool is_group = false;
    bool is_heartbeat = false;
    bool reset_triggered = false;
    std::string trigger_body_normalized;
};

/// Determine if fast directive execution path can be used.
/// Returns false for groups, heartbeats, resets, or messages with slash commands.
bool should_use_reply_fast_directive_execution(const FastDirectiveExecutionParams& params);

// ============================================================================
// Fast reply command context (对齐 OC: buildFastReplyCommandContext)
// ============================================================================

struct FastReplyCommandContext {
    std::string surface;
    std::string channel;
    std::optional<std::string> channel_id;
    std::vector<std::string> owner_list;
    bool sender_is_owner = false;
    bool is_authorized_sender = false;
    std::optional<std::string> sender_id;
    std::string abort_key;
    std::string raw_body_normalized;
    std::string command_body_normalized;
    std::optional<std::string> from;
    std::optional<std::string> to;
};

/// Build a lightweight command context for fast-path command handling.
/// 对齐 OC: buildFastReplyCommandContext (get-reply-fast-path.ts:154-186)
FastReplyCommandContext build_fast_reply_command_context(
    const InboundContext& ctx,
    const std::string& config_json,
    const std::string& agent_id,
    const std::string& session_key,
    bool is_group,
    const std::string& trigger_body_normalized,
    bool command_authorized);

// ============================================================================
// Fast reply text commands gate
// 对齐 OC: shouldHandleFastReplyTextCommands (get-reply-fast-path.ts:188-193)
// ============================================================================

/// Whether text commands should be handled in the fast path.
/// Returns true if command_source is "native" or cfg.commands.text != false.
bool should_handle_fast_reply_text_commands(
    const std::string& config_json,
    const std::string& command_source);

// ============================================================================
// Fast reply session state (对齐 OC: initFastReplySessionState)
// ============================================================================

struct FastReplySessionState {
    std::string session_key;
    std::string session_id;
    bool is_new_session = false;
    bool reset_triggered = false;
    bool is_group = false;
    std::string body_stripped;
    std::string trigger_body_normalized;
    std::string store_path;
    std::string session_scope;
};

/// Initialize session state for the fast reply path.
/// 对齐 OC: initFastReplySessionState (get-reply-fast-path.ts:195-294)
FastReplySessionState init_fast_reply_session_state(
    const InboundContext& ctx,
    const std::string& config_json,
    const std::string& agent_id,
    bool command_authorized,
    const std::string& workspace_dir);

}  // namespace auto_reply
}  // namespace hunterclaw
