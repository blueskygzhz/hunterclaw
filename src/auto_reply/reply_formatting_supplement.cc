// HunterClaw – reply_formatting_supplement.cc
// 严格对齐 OC auto-reply/reply/ 回复格式化 & 路由子系统:
//   response-prefix-template.ts (3.22KB) — 响应前缀模板插值
//   prompt-prelude.ts (1.94KB) — Prompt 前导构建
//   body.ts (1.76KB) — 会话提示体构建
//   origin-routing.ts (786B) — 来源路由解析
//   channel-context.ts (1.43KB) — 渠道上下文
//   command-gates.ts (2.96KB) — 命令权限门控
//   followup-delivery.ts (2.88KB) — 跟进投递载荷
//   followup-runner.ts (16.25KB) — 跟进运行器
// Total OC: ~31KB

#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/auto_reply/tokens.h"
#include "hunterclaw/auto_reply/heartbeat.h"
#include "hunterclaw/auto_reply/media_note.h"
#include "hunterclaw/auto_reply/inbound_context.h"
#include "hunterclaw/config/config_system.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace hunterclaw::auto_reply {

namespace {
std::string trim_str(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}
std::string lower_str(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}
}  // namespace

// ═══════════════════════════════════════════════════════════════
// Response Prefix Template (对齐 OC: response-prefix-template.ts)
// ═══════════════════════════════════════════════════════════════

struct ResponsePrefixContext {
    std::string model;
    std::string model_full;
    std::string provider;
    std::string thinking_level;
    std::string identity_name;
};

// 对齐 interpolateResponsePrefix (response-prefix-template.ts:31-78)
std::string interpolate_response_prefix(
    const std::string& template_str,
    const ResponsePrefixContext& context) {
    if (template_str.empty()) return "";

    static const std::regex var_re("\\{([a-zA-Z][a-zA-Z0-9.]*)\\}");
    std::string result = template_str;

    // Build lookup map
    std::map<std::string, std::string> vars;
    vars["model"] = context.model;
    vars["modelfull"] = context.model_full;
    vars["provider"] = context.provider;
    vars["thinkinglevel"] = context.thinking_level;
    vars["identityname"] = context.identity_name;

    std::sregex_iterator it(result.begin(), result.end(), var_re);
    std::sregex_iterator end;
    std::string output;
    size_t last_pos = 0;

    for (; it != end; ++it) {
        output += result.substr(last_pos, it->position() - last_pos);
        std::string key = lower_str((*it)[1].str());
        // Normalize dotted keys: "thinking.level" → "thinkinglevel"
        key.erase(std::remove(key.begin(), key.end(), '.'), key.end());
        auto found = vars.find(key);
        if (found != vars.end() && !found->second.empty()) {
            output += found->second;
        } else {
            output += it->str();  // Keep unresolved as literal
        }
        last_pos = it->position() + it->length();
    }
    output += result.substr(last_pos);
    return output;
}

// 对齐 shouldApplyResponsePrefix (response-prefix-template.ts:80-90)
bool should_apply_response_prefix(const std::string& prefix, const std::string& text) {
    if (prefix.empty() || text.empty()) return false;
    // Don't apply if text already starts with the prefix
    return text.find(prefix) != 0;
}

// ═══════════════════════════════════════════════════════════════
// Origin Routing (对齐 OC: origin-routing.ts:4-27)
// ═══════════════════════════════════════════════════════════════

std::optional<std::string> resolve_origin_message_provider(
    const std::string& originating_channel,
    const std::string& provider) {
    std::string oc = lower_str(trim_str(originating_channel));
    if (!oc.empty()) return oc;
    std::string prov = lower_str(trim_str(provider));
    if (!prov.empty()) return prov;
    return std::nullopt;
}

std::optional<std::string> resolve_origin_message_to(
    const std::string& originating_to,
    const std::string& to) {
    if (!originating_to.empty()) return originating_to;
    if (!to.empty()) return to;
    return std::nullopt;
}

std::optional<std::string> resolve_origin_account_id(
    const std::string& originating_account_id,
    const std::string& account_id) {
    if (!originating_account_id.empty()) return originating_account_id;
    if (!account_id.empty()) return account_id;
    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// Channel Context (对齐 OC: channel-context.ts:8-54)
// ═══════════════════════════════════════════════════════════════

struct ChannelContextResult {
    std::string surface;
    std::string provider;
    std::string account_id;
};

ChannelContextResult resolve_channel_context(
    const InboundContext& ctx,
    const std::string& command_channel = "") {
    ChannelContextResult result;
    result.surface = lower_str(trim_str(
        !ctx.surface.empty() ? ctx.surface
        : !command_channel.empty() ? command_channel
        : ctx.provider));
    result.provider = lower_str(trim_str(
        !ctx.provider.empty() ? ctx.provider : result.surface));
    result.account_id = trim_str(ctx.account_id);
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Command Gates (对齐 OC: command-gates.ts:7-101)
// ═══════════════════════════════════════════════════════════════

struct CommandGateResult {
    bool should_continue = true;
    std::optional<std::string> reply_text;
};

CommandGateResult reject_unauthorized_command(
    bool is_authorized_sender,
    const std::string& sender_id,
    const std::string& command_label,
    const std::string& command_source) {
    if (is_authorized_sender) {
        return {true, std::nullopt};
    }
    // Native commands get an error text response
    if (command_source == "native") {
        return {false, "You are not authorized to use this command."};
    }
    // Text commands are silently ignored
    return {false, std::nullopt};
}

CommandGateResult reject_disabled_command(
    bool is_enabled,
    const std::string& command_label,
    const std::string& command_source) {
    if (is_enabled) {
        return {true, std::nullopt};
    }
    if (command_source == "native") {
        return {false, command_label + " is disabled."};
    }
    return {false, std::nullopt};
}

// ═══════════════════════════════════════════════════════════════
// Prompt Prelude (对齐 OC: prompt-prelude.ts:5-48)
// ═══════════════════════════════════════════════════════════════

static const std::string REPLY_MEDIA_HINT =
    "To send an image back, prefer the message tool (media/path/filePath). "
    "If you must inline, use MEDIA:https://example.com/image.jpg (spaces ok, "
    "quote if needed) or a safe relative path like MEDIA:./image.jpg. Avoid "
    "absolute paths (MEDIA:/...) and ~ paths - they are blocked for security. "
    "Keep caption in the text body.";

struct PromptPreludeResult {
    std::optional<std::string> media_note;
    std::optional<std::string> media_reply_hint;
    std::string prefixed_command_body;
    std::string queued_body;
};

PromptPreludeResult build_reply_prompt_bodies(
    const InboundContext& ctx,
    const std::string& effective_base_body,
    const std::string& prefixed_body,
    const std::optional<std::string>& thread_context_note = std::nullopt,
    const std::vector<std::string>& system_event_blocks = {}) {

    PromptPreludeResult result;

    // Combine system event blocks
    std::string events_block;
    for (const auto& block : system_event_blocks) {
        if (!block.empty()) {
            if (!events_block.empty()) events_block += "\n";
            events_block += block;
        }
    }

    auto prepend_events = [&](const std::string& body) -> std::string {
        return events_block.empty() ? body : events_block + "\n\n" + body;
    };

    // Build media note
    if (!ctx.media_paths.empty()) {
        result.media_note = build_inbound_media_note(
            ctx.media_paths, ctx.media_urls, ctx.media_types);
        if (result.media_note) {
            result.media_reply_hint = REPLY_MEDIA_HINT;
        }
    }

    result.prefixed_command_body = prepend_events(prefixed_body);
    if (thread_context_note && !thread_context_note->empty()) {
        result.prefixed_command_body = *thread_context_note + "\n\n" + result.prefixed_command_body;
    }

    result.queued_body = prepend_events(effective_base_body);
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Body (对齐 OC: body.ts:13-54)
// ═══════════════════════════════════════════════════════════════

std::string apply_session_hints(
    const std::string& base_body,
    bool aborted_last_run) {
    if (!aborted_last_run) return base_body;
    static const std::string abort_hint =
        "Note: The previous agent run was aborted by the user. "
        "Resume carefully or ask for clarification.";
    return abort_hint + "\n\n" + base_body;
}

// ═══════════════════════════════════════════════════════════════
// Followup Delivery (对齐 OC: followup-delivery.ts:19-85)
// ═══════════════════════════════════════════════════════════════

struct FollowupDeliveryParams {
    std::vector<ReplyPayload> payloads;
    std::string message_provider;
    std::string originating_account_id;
    std::string originating_to;
    std::string config_json;
};

std::vector<ReplyPayload> resolve_followup_delivery_payloads(
    const FollowupDeliveryParams& params) {
    std::vector<ReplyPayload> result;
    std::set<std::string> seen_media;

    for (const auto& payload : params.payloads) {
        // Skip empty payloads
        if (!payload.text && !payload.media_url && payload.media_urls.empty()) {
            continue;
        }

        // Strip heartbeat token
        if (payload.text) {
            auto stripped = Heartbeat::strip_heartbeat_token(
                *payload.text, Heartbeat::StripMode::Message);
            if (stripped.should_skip) continue;
        }

        // Deduplicate media
        ReplyPayload filtered = payload;
        if (filtered.media_url) {
            if (seen_media.count(*filtered.media_url)) {
                filtered.media_url.reset();
            } else {
                seen_media.insert(*filtered.media_url);
            }
        }

        result.push_back(std::move(filtered));
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Followup Runner (对齐 OC: followup-runner.ts 核心逻辑)
// 注意: 完整的 followup-runner 逻辑已在 message_dispatcher.cc 的
//       followup drain 管线中实现 (对齐 OC scheduleFollowupDrain)。
//       此处提供辅助结构和配置解析。
// ═══════════════════════════════════════════════════════════════

struct FollowupRunConfig {
    std::string provider;
    std::string model;
    int context_window_tokens = 128000;
    bool should_run_preflight_compaction = false;
    std::string auth_profile;
};

FollowupRunConfig resolve_followup_run_config(
    const std::string& config_json,
    const std::string& agent_id,
    const std::string& session_key) {
    FollowupRunConfig result;
    try {
        auto cfg = nlohmann::json::parse(config_json);
        auto defaults = cfg.value(nlohmann::json::json_pointer("/agents/defaults"), nlohmann::json::object());
        result.provider = defaults.value("provider", "anthropic");
        result.model = defaults.value("model", "claude-sonnet-4-20250514");
    } catch (...) {
        result.provider = "anthropic";
        result.model = "claude-sonnet-4-20250514";
    }
    return result;
}

}  // namespace hunterclaw::auto_reply
