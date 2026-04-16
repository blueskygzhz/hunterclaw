// Aligned with openclaw: src/auto-reply/reply/get-reply-fast-path.ts (295行)
// Fast path 优化 — 在测试/轻量环境下跳过重量级初始化

#include "hunterclaw/auto_reply/get_reply_fast_path.h"
#include "hunterclaw/auto_reply/command_detection.h"
#include "hunterclaw/sessions/session_key_utils.h"
#include "hunterclaw/config/config_system.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <string>

using json = nlohmann::json;

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Helpers
// ============================================================================

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// ============================================================================
// resolve_get_reply_config
// 对齐 OC: resolveGetReplyConfig (get-reply-fast-path.ts:96-114)
// ============================================================================

std::string resolve_get_reply_config(const ResolveGetReplyConfigParams& params) {
    if (!params.config_override_json.has_value()) {
        return params.base_config_json;
    }
    // In fast test env with complete reply config, use override directly
    if (params.is_fast_test_env) {
        return *params.config_override_json;
    }
    // Otherwise merge-patch override onto base
    try {
        auto base = json::parse(params.base_config_json);
        auto override_cfg = json::parse(*params.config_override_json);
        base.merge_patch(override_cfg);
        return base.dump();
    } catch (...) {
        return params.base_config_json;
    }
}

// ============================================================================
// Fast test bootstrap/runtime detection
// 对齐 OC: shouldUseReplyFastTestBootstrap (get-reply-fast-path.ts:116-125)
// ============================================================================

bool should_use_reply_fast_test_bootstrap(
    bool is_fast_test_env,
    bool is_complete_reply_config,
    bool uses_full_reply_runtime) {
    return is_fast_test_env && is_complete_reply_config && !uses_full_reply_runtime;
}

// 对齐 OC: shouldUseReplyFastTestRuntime (get-reply-fast-path.ts:127-134)
bool should_use_reply_fast_test_runtime(
    bool is_fast_test_env,
    bool is_complete_reply_config,
    bool uses_full_reply_runtime) {
    return is_fast_test_env && is_complete_reply_config && !uses_full_reply_runtime;
}

// ============================================================================
// should_use_reply_fast_directive_execution
// 对齐 OC: shouldUseReplyFastDirectiveExecution (get-reply-fast-path.ts:136-152)
// ============================================================================

bool should_use_reply_fast_directive_execution(const FastDirectiveExecutionParams& params) {
    if (!params.is_fast_test_bootstrap ||
        params.is_group ||
        params.is_heartbeat ||
        params.reset_triggered) {
        return false;
    }
    // No slash commands in the trigger body
    return params.trigger_body_normalized.find('/') == std::string::npos;
}

// ============================================================================
// build_fast_reply_command_context
// 对齐 OC: buildFastReplyCommandContext (get-reply-fast-path.ts:154-186)
// ============================================================================

FastReplyCommandContext build_fast_reply_command_context(
    const InboundContext& ctx,
    const std::string& /*config_json*/,
    const std::string& /*agent_id*/,
    const std::string& session_key,
    bool is_group,
    const std::string& trigger_body_normalized,
    bool command_authorized) {

    std::string surface = to_lower(trim(
        !ctx.surface.empty() ? ctx.surface : ctx.provider));
    std::string channel = to_lower(trim(
        !ctx.provider.empty() ? ctx.provider : surface));
    std::string from_str = trim(ctx.from);
    std::string to_str = trim(ctx.to);

    // Build command body normalized
    std::string body_for_commands = trigger_body_normalized;
    if (is_group) {
        // Strip mentions (simplified)
        body_for_commands = trigger_body_normalized;
    }
    std::string command_body_normalized =
        CommandDetection::normalize_command_body(body_for_commands);

    FastReplyCommandContext result;
    result.surface = surface;
    result.channel = channel;
    result.sender_is_owner = false;
    result.is_authorized_sender = command_authorized;
    result.sender_id = from_str.empty() ? std::nullopt
                                        : std::optional<std::string>(from_str);
    result.abort_key = !session_key.empty() ? session_key
                       : !from_str.empty() ? from_str
                       : to_str;
    result.raw_body_normalized = trigger_body_normalized;
    result.command_body_normalized = command_body_normalized;
    result.from = from_str.empty() ? std::nullopt
                                   : std::optional<std::string>(from_str);
    result.to = to_str.empty() ? std::nullopt
                               : std::optional<std::string>(to_str);
    return result;
}

// ============================================================================
// should_handle_fast_reply_text_commands
// 对齐 OC: shouldHandleFastReplyTextCommands (get-reply-fast-path.ts:188-193)
// ============================================================================

bool should_handle_fast_reply_text_commands(
    const std::string& config_json,
    const std::string& command_source) {
    if (command_source == "native") return true;
    // Check cfg.commands.text !== false
    try {
        auto cfg = json::parse(config_json);
        auto commands = cfg.value("commands", json::object());
        auto text_val = commands.value("text", true);
        return text_val != false;
    } catch (...) {
        return true;
    }
}

// ============================================================================
// init_fast_reply_session_state
// 对齐 OC: initFastReplySessionState (get-reply-fast-path.ts:195-294)
// ============================================================================

FastReplySessionState init_fast_reply_session_state(
    const InboundContext& ctx,
    const std::string& config_json,
    const std::string& agent_id,
    bool /*command_authorized*/,
    const std::string& /*workspace_dir*/) {

    FastReplySessionState result;

    // Resolve session scope
    try {
        auto cfg = json::parse(config_json);
        result.session_scope =
            cfg.value(json::json_pointer("/session/scope"), std::string("per-sender"));
    } catch (...) {
        result.session_scope = "per-sender";
    }

    // Resolve session key (simplified — full implementation requires sessions::resolve_session_key)
    result.session_key = ctx.session_key.empty()
        ? (ctx.from.empty() ? "default" : ctx.from)
        : ctx.session_key;

    // Resolve trigger body
    std::string command_source =
        !ctx.body_for_commands.empty() ? ctx.body_for_commands
        : !ctx.command_body.empty() ? ctx.command_body
        : !ctx.raw_body.empty() ? ctx.raw_body
        : ctx.body;
    result.trigger_body_normalized = trim(command_source);

    // Detect chat type
    std::string chat_type = to_lower(trim(ctx.chat_type));
    result.is_group = !chat_type.empty() && chat_type != "direct";

    // Check for /new or /reset
    std::regex reset_re(R"(^/(new|reset)(?:\s|$))", std::regex_constants::icase);
    std::smatch reset_match;
    result.reset_triggered = std::regex_search(
        result.trigger_body_normalized, reset_match, reset_re);

    // Session ID (simplified — generate inline)
    {
        static std::atomic<uint64_t> counter{0};
        std::ostringstream oss;
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        oss << std::hex << now << "-" << counter.fetch_add(1);
        result.session_id = oss.str();
    }
    result.is_new_session = result.reset_triggered;

    // Body stripped
    if (result.reset_triggered && reset_match[0].matched) {
        result.body_stripped = trim(
            result.trigger_body_normalized.substr(reset_match[0].length()));
    } else {
        result.body_stripped =
            !ctx.body_for_agent.empty() ? ctx.body_for_agent : ctx.body;
    }

    // Store path (simplified)
    result.store_path = "~/.hunterclaw/sessions";

    return result;
}

}  // namespace auto_reply
}  // namespace hunterclaw
