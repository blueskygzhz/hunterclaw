// ============================================================================
// outbound_policy.cc
// 严格对齐 OpenClaw: infra/outbound/outbound-policy.ts (224行)
//
// 跨上下文送信守卫系统:
//   - enforceCrossContextPolicy: 阻止未授权的跨频道/跨 provider 消息发送
//   - buildCrossContextDecoration: 构建 "[from {channel}]" 前缀
//   - shouldApplyCrossContextMarker: 判断是否需要标记
//   - applyCrossContextDecoration: 应用前缀/后缀
//
// CONTEXT_GUARDED_ACTIONS: send, poll, reply, sendWithEffect, sendAttachment,
//                          upload-file, thread-create, thread-reply, sticker
// ============================================================================

#include "hunterclaw/core/types.h"
#include <string>
#include <set>
#include <optional>
#include <stdexcept>
#include <algorithm>

namespace hunterclaw {
namespace outbound {

namespace {

// 对齐 outbound-policy.ts:20-30
const std::set<std::string> CONTEXT_GUARDED_ACTIONS = {
    "send", "poll", "reply", "sendWithEffect", "sendAttachment",
    "upload-file", "thread-create", "thread-reply", "sticker"
};

// 对齐 outbound-policy.ts:32-41
const std::set<std::string> CONTEXT_MARKER_ACTIONS = {
    "send", "poll", "reply", "sendWithEffect", "sendAttachment",
    "upload-file", "thread-reply", "sticker"
};

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// 对齐 outbound-policy.ts:43-68
std::optional<std::string> resolve_context_guard_target(
    const std::string& action,
    const std::string& to_param,
    const std::string& channel_id_param
) {
    if (!CONTEXT_GUARDED_ACTIONS.count(action)) return std::nullopt;

    if (action == "thread-reply" || action == "thread-create") {
        if (!channel_id_param.empty()) return channel_id_param;
        if (!to_param.empty()) return to_param;
        return std::nullopt;
    }
    if (!to_param.empty()) return to_param;
    if (!channel_id_param.empty()) return channel_id_param;
    return std::nullopt;
}

// 对齐 outbound-policy.ts:70-72
std::optional<std::string> normalize_target(
    const std::string& /*channel*/, const std::string& raw
) {
    auto trimmed = trim(raw);
    return trimmed.empty() ? std::nullopt : std::optional(to_lower(trimmed));
}

// 对齐 outbound-policy.ts:74-89
bool is_cross_context_target(
    const std::string& channel,
    const std::string& target,
    const std::optional<std::string>& current_channel_id
) {
    if (!current_channel_id || current_channel_id->empty()) return false;
    auto normalized_target = normalize_target(channel, target);
    auto normalized_current = normalize_target(channel, *current_channel_id);
    if (!normalized_target || !normalized_current) return false;
    return *normalized_target != *normalized_current;
}

} // namespace

// ============================================================================
// CrossContextDecoration (对齐 outbound-policy.ts:14-18)
// ============================================================================

struct CrossContextDecoration {
    std::string prefix;
    std::string suffix;
};

// ============================================================================
// enforceCrossContextPolicy (对齐 outbound-policy.ts:91-141)
// ============================================================================

void enforce_cross_context_policy(
    const std::string& channel,
    const std::string& action,
    const std::string& to_param,
    const std::string& channel_id_param,
    const std::optional<std::string>& current_channel_id,
    const std::optional<std::string>& current_channel_provider,
    bool allow_cross_context_send,
    bool allow_within_provider,
    bool allow_across_providers
) {
    if (!current_channel_id || current_channel_id->empty()) return;
    if (!CONTEXT_GUARDED_ACTIONS.count(action)) return;
    if (allow_cross_context_send) return;

    // Cross-provider check
    if (current_channel_provider && *current_channel_provider != channel) {
        if (!allow_across_providers) {
            throw std::runtime_error(
                "Cross-context messaging denied: action=" + action +
                " target provider \"" + channel + "\" while bound to \"" +
                *current_channel_provider + "\".");
        }
        return;
    }

    if (allow_within_provider) return;

    auto target = resolve_context_guard_target(action, to_param, channel_id_param);
    if (!target) return;

    if (!is_cross_context_target(channel, *target, current_channel_id)) return;

    throw std::runtime_error(
        "Cross-context messaging denied: action=" + action +
        " target=\"" + *target + "\" while bound to \"" +
        *current_channel_id + "\" (channel=" + channel + ").");
}

// ============================================================================
// buildCrossContextDecoration (对齐 outbound-policy.ts:143-198)
// ============================================================================

std::optional<CrossContextDecoration> build_cross_context_decoration(
    const std::string& channel,
    const std::string& target,
    const std::optional<std::string>& current_channel_id,
    const std::string& prefix_template,
    const std::string& suffix_template
) {
    if (!current_channel_id || current_channel_id->empty()) return std::nullopt;
    if (!is_cross_context_target(channel, target, current_channel_id)) return std::nullopt;

    std::string origin_label = *current_channel_id;

    // Apply templates
    auto replace_all = [](std::string s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return s;
    };

    std::string tmpl_prefix = prefix_template.empty() ? "[from {channel}] " : prefix_template;
    std::string tmpl_suffix = suffix_template;

    return CrossContextDecoration{
        replace_all(tmpl_prefix, "{channel}", origin_label),
        replace_all(tmpl_suffix, "{channel}", origin_label)
    };
}

// ============================================================================
// shouldApplyCrossContextMarker (对齐 outbound-policy.ts:200-202)
// ============================================================================

bool should_apply_cross_context_marker(const std::string& action) {
    return CONTEXT_MARKER_ACTIONS.count(action) > 0;
}

// ============================================================================
// applyCrossContextDecoration (对齐 outbound-policy.ts:204-224)
// ============================================================================

std::string apply_cross_context_decoration(
    const std::string& message,
    const CrossContextDecoration& decoration
) {
    return decoration.prefix + message + decoration.suffix;
}

} // namespace outbound
} // namespace hunterclaw
