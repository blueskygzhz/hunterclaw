// Aligned with openclaw: src/channels/inbound-debounce-policy.ts
#pragma once

#include "hunterclaw/auto_reply/inbound_debounce.h"
#include "hunterclaw/auto_reply/command_detection.h"
#include <string>
#include <optional>
#include <functional>
#include <vector>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Channel inbound debounce policy
// Aligned with OpenClaw: inbound-debounce-policy.ts
// ============================================================================

// Aligned with OpenClaw: shouldDebounceTextInbound params
struct ShouldDebounceTextInboundParams {
    std::string text;
    bool has_media = false;
    bool allow_debounce = true;
    // Optional: command normalize options (bot_username)
    std::optional<std::string> bot_username;
};

/**
 * Determine whether a text inbound message should be debounced.
 * Aligned with OpenClaw: shouldDebounceTextInbound()
 *
 * Returns false if:
 *   - allow_debounce is false
 *   - message has media
 *   - text is empty
 *   - text is a control command (e.g. /stop, /reset)
 */
inline bool should_debounce_text_inbound(const ShouldDebounceTextInboundParams& params) {
    if (!params.allow_debounce) return false;
    if (params.has_media) return false;

    std::string text = params.text;
    size_t s = text.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return false;
    size_t e = text.find_last_not_of(" \t\r\n");
    text = text.substr(s, e - s + 1);
    if (text.empty()) return false;

    // Don't debounce control commands
    return !auto_reply::CommandDetection::has_control_command(text);
}

// ============================================================================
// Channel inbound debouncer factory
// Aligned with OpenClaw: createChannelInboundDebouncer()
// ============================================================================

template <typename T>
struct ChannelInboundDebouncerResult {
    int debounce_ms = 0;
    auto_reply::InboundDebouncer<T> debouncer;
};

template <typename T>
struct CreateChannelInboundDebouncerParams {
    // From InboundDebouncer::Params (minus debounce_ms)
    std::function<std::string(const T&)> build_key;
    std::function<std::optional<int>(const T&)> resolve_debounce_ms;
    std::function<bool(const T&)> should_debounce;
    std::function<void(const std::vector<T>&)> on_flush;
    std::function<void(const std::string&, const std::vector<T>&)> on_error;

    // Channel-specific
    auto_reply::InboundDebounceConfig debounce_config;
    std::string channel;
    std::optional<int> debounce_ms_override;
};

/**
 * Create a channel-aware inbound debouncer.
 * Aligned with OpenClaw: createChannelInboundDebouncer()
 */
template <typename T>
ChannelInboundDebouncerResult<T> create_channel_inbound_debouncer(
    CreateChannelInboundDebouncerParams<T> params)
{
    int debounce_ms = auto_reply::resolve_inbound_debounce_ms(
        params.debounce_config,
        params.channel,
        params.debounce_ms_override
    );

    typename auto_reply::InboundDebouncer<T>::Params debouncer_params;
    debouncer_params.debounce_ms = debounce_ms;
    debouncer_params.build_key = std::move(params.build_key);
    debouncer_params.resolve_debounce_ms = std::move(params.resolve_debounce_ms);
    debouncer_params.should_debounce = std::move(params.should_debounce);
    debouncer_params.on_flush = std::move(params.on_flush);
    debouncer_params.on_error = std::move(params.on_error);

    return ChannelInboundDebouncerResult<T>{
        debounce_ms,
        auto_reply::InboundDebouncer<T>(std::move(debouncer_params))
    };
}

} // namespace channels
} // namespace hunterclaw
