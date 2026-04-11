#include "hunterclaw/channels/conversation_binding_context.h"
#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/channels/registry.h"
#include "hunterclaw/core/utils.h"
#include <algorithm>

namespace hunterclaw {
namespace channels {

namespace {
    std::string to_lower_trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\n\r");
        std::string trimmed = s.substr(start, end - start + 1);
        std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), ::tolower);
        return trimmed;
    }

    std::optional<std::string> normalize_optional(const std::optional<std::string>& s) {
        if (!s || s->empty()) return std::nullopt;
        auto start = s->find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return std::nullopt;
        auto end = s->find_last_not_of(" \t\n\r");
        return s->substr(start, end - start + 1);
    }

    // 对齐 OC: CANONICAL_TARGET_PREFIXES
    const std::vector<std::string> CANONICAL_TARGET_PREFIXES = {
        "user:", "channel:", "conversation:", "group:", "room:", "dm:", "spaces/"
    };

    bool has_canonical_prefix(const std::string& lower) {
        for (const auto& prefix : CANONICAL_TARGET_PREFIXES) {
            if (lower.size() >= prefix.size() && lower.substr(0, prefix.size()) == prefix) {
                return true;
            }
        }
        return false;
    }

    // 对齐 OC: resolveBindingAccountId
    std::string resolve_binding_account_id(
        const std::optional<std::string>& raw_account_id,
        const std::optional<ChannelPlugin>& plugin,
        const core::Config& cfg) {
        auto normalized = normalize_optional(raw_account_id);
        if (normalized) return *normalized;
        // Try plugin default
        if (plugin && plugin->config) {
            auto default_id_fn = plugin->config->default_account_id;
            if (default_id_fn) {
                auto id = default_id_fn(cfg);
                if (!id.empty()) return id;
            }
        }
        return "default";
    }
} // namespace

// ============================================================================
// resolve_channel_target_id (对齐 OC: resolveChannelTargetId)
// ============================================================================

std::optional<std::string> resolve_channel_target_id(
    const std::string& channel,
    const std::optional<std::string>& target) {
    auto normalized = normalize_optional(target);
    if (!normalized) return std::nullopt;

    std::string lower = to_lower_trim(*normalized);
    std::string channel_prefix = channel + ":";

    // Strip channel prefix recursively
    if (lower.size() > channel_prefix.size() && lower.substr(0, channel_prefix.size()) == channel_prefix) {
        std::string rest = normalized->substr(channel_prefix.size());
        return resolve_channel_target_id(channel, rest);
    }

    // Canonical prefix → return as-is
    if (has_canonical_prefix(lower)) {
        return *normalized;
    }

    // Fallback: return the raw target
    return *normalized;
}

// ============================================================================
// resolve_conversation_binding_context (对齐 OC: resolveConversationBindingContext)
// ============================================================================

std::optional<ConversationBindingContext> resolve_conversation_binding_context(
    const ResolveConversationBindingContextInput& params) {
    // Normalize channel
    auto channel_opt = normalize_any_channel_id(params.channel.value_or(""));
    if (!channel_opt) {
        channel_opt = normalize_chat_channel_id(params.channel.value_or(""));
    }
    if (!channel_opt) {
        auto lower = to_lower_trim(params.channel.value_or(""));
        if (lower.empty()) return std::nullopt;
        channel_opt = lower;
    }
    std::string channel = *channel_opt;

    // Resolve plugin
    auto plugin = ChannelPluginRegistry::instance().get_plugin(channel);

    // Resolve account id
    std::string account_id = resolve_binding_account_id(params.account_id, plugin, params.cfg);

    // Normalize thread id
    auto thread_id = normalize_optional(params.thread_id);

    // ──── Strategy 1: Plugin bindings.resolveCommandConversation() ────
    // (In C++ we don't have dynamic plugin hooks yet; skip to generic resolution)

    // ──── Strategy 2: Plugin threading.resolveFocusedBinding() ────
    // (Skip: requires loaded plugin runtime with threading adapter)

    // ──── Strategy 3: Generic channel target resolution ────
    auto base_conversation_id =
        resolve_channel_target_id(channel, params.originating_to);
    if (!base_conversation_id) {
        base_conversation_id = resolve_channel_target_id(channel, params.command_to);
    }
    if (!base_conversation_id) {
        base_conversation_id = resolve_channel_target_id(channel, params.fallback_to);
    }

    auto parent_conversation_id = resolve_channel_target_id(channel, params.thread_parent_id);
    if (!parent_conversation_id && thread_id && base_conversation_id && *base_conversation_id != *thread_id) {
        parent_conversation_id = base_conversation_id;
    }

    std::string conversation_id;
    if (thread_id) {
        conversation_id = *thread_id;
    } else if (base_conversation_id) {
        conversation_id = *base_conversation_id;
    } else {
        return std::nullopt;
    }

    ConversationBindingContext result;
    result.channel = channel;
    result.account_id = account_id;
    result.conversation_id = conversation_id;
    result.parent_conversation_id = parent_conversation_id;
    result.thread_id = thread_id;

    return result;
}

} // namespace channels
} // namespace hunterclaw
