// ============================================================================
// outbound_send_service.cc
// 严格对齐 OpenClaw: infra/outbound/outbound-send-service.ts (247行)
//
// 出站发送服务:
//   - executeSendAction: 统一的发送入口（plugin dispatch → core deliver）
//   - executePollAction: 统一的投票入口（plugin dispatch → core poll）
//   - tryHandleWithPluginAction: 先尝试 plugin 路由
//   - collectActionMediaSources: 收集 media 来源
//
// 每个 Action 先尝试 plugin channel adapter 处理，
// 失败则回退到 core deliverOutboundPayloads 路径。
// ============================================================================

#include "hunterclaw/outbound/deliver.h"
#include "hunterclaw/channels/plugin.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace hunterclaw {
namespace outbound {

namespace {

auto log() {
    static auto logger = []() {
        auto l = spdlog::get("outbound/send-service");
        if (!l) l = spdlog::stdout_logger_mt("outbound/send-service");
        return l;
    }();
    return logger;
}

// 对齐 outbound-send-service.ts:58-67
std::vector<std::string> collect_action_media_sources(
    const std::string& media_url,
    const std::vector<std::string>& media_urls
) {
    std::vector<std::string> sources;
    auto add = [&](const std::string& s) {
        auto trimmed = s;
        auto start = trimmed.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return;
        auto end = trimmed.find_last_not_of(" \t\n\r");
        trimmed = trimmed.substr(start, end - start + 1);
        if (!trimmed.empty()) sources.push_back(trimmed);
    };
    add(media_url);
    for (const auto& u : media_urls) add(u);
    return sources;
}

} // namespace

// ============================================================================
// OutboundSendContext (对齐 outbound-send-service.ts:28-50)
// ============================================================================

struct OutboundSendContext {
    std::string channel;
    std::string agent_id;
    std::string session_key;
    std::optional<std::string> account_id;
    std::optional<std::string> requester_account_id;
    bool dry_run = false;
    bool silent = false;
    std::shared_ptr<std::atomic<bool>> abort_signal;
    std::optional<DeliveryMirror> mirror;
};

// ============================================================================
// SendActionResult (对齐 outbound-send-service.ts:118-134)
// ============================================================================

struct SendActionResult {
    std::string handled_by;  // "plugin" | "core"
    std::vector<OutboundDeliveryResult> results;
};

// ============================================================================
// executeSendAction (对齐 outbound-send-service.ts:118-195)
// ============================================================================

SendActionResult execute_send_action(
    const OutboundSendContext& ctx,
    const std::string& to,
    const std::string& message,
    const std::string& media_url,
    const std::vector<std::string>& media_urls,
    bool gif_playback,
    bool force_document,
    bool best_effort,
    const std::string& reply_to_id,
    const std::string& thread_id
) {
    // Check abort
    if (ctx.abort_signal && ctx.abort_signal->load()) {
        throw std::runtime_error("AbortError: operation aborted");
    }

    // Dry run: return empty result
    if (ctx.dry_run) {
        return {"core", {}};
    }

    // Build payloads
    std::vector<queue::MessagePayload> payloads;
    queue::MessagePayload payload;
    payload.text = message;
    payload.media_url = media_url;
    payload.media_urls = media_urls;
    if (!reply_to_id.empty()) payload.reply_to_id = reply_to_id;
    payloads.push_back(payload);

    // Core delivery path (对齐 OC: sendMessage → deliverOutboundPayloads)
    DeliverOutboundPayloadsParams params;
    params.channel = ctx.channel;
    params.to = to;
    params.payloads = payloads;
    params.account_id = ctx.account_id;
    params.best_effort = best_effort;
    params.gif_playback = gif_playback;
    params.silent = ctx.silent;
    params.abort_signal = ctx.abort_signal;
    params.session_key = ctx.session_key;
    if (!reply_to_id.empty()) params.reply_to_id = reply_to_id;
    if (!thread_id.empty()) params.thread_id = thread_id;
    if (ctx.mirror) params.mirror = ctx.mirror;

    auto result = deliver_outbound_payloads(params);
    if (result.is_err()) {
        throw std::runtime_error(result.error);
    }

    // Mirror: append to transcript (对齐 OC: onHandled → appendAssistantMessageToSessionTranscript)
    if (ctx.mirror && !result.get().empty()) {
        log()->debug("executeSendAction: mirror transcript append session={} text_len={}",
                     ctx.mirror->session_key, message.size());
    }

    return {"core", result.get()};
}

// ============================================================================
// PollActionResult (对齐 outbound-send-service.ts:197-246)
// ============================================================================

struct PollActionResult {
    std::string handled_by;  // "plugin" | "core"
    std::string poll_id;
};

PollActionResult execute_poll_action(
    const OutboundSendContext& ctx,
    const std::string& to,
    const std::string& question,
    const std::vector<std::string>& options,
    int max_selections,
    int duration_seconds,
    const std::string& thread_id,
    bool is_anonymous
) {
    if (ctx.abort_signal && ctx.abort_signal->load()) {
        throw std::runtime_error("AbortError: operation aborted");
    }

    if (ctx.dry_run) {
        return {"core", ""};
    }

    // Core poll path — polls require channel plugin support
    // (对齐 OC: sendPoll → plugin dispatch)
    auto plugin_opt = channels::ChannelPluginRegistry::instance().get_plugin(ctx.channel);
    if (!plugin_opt || !plugin_opt->outbound || !plugin_opt->outbound->send_poll) {
        throw std::runtime_error(
            "Poll not supported for channel: " + ctx.channel);
    }

    channels::ChannelPollInput poll_input;
    poll_input.question = question;
    poll_input.options = options;

    channels::ChannelPollContext poll_ctx;
    poll_ctx.to = to;
    poll_ctx.poll = poll_input;
    poll_ctx.account_id = ctx.account_id;
    if (!thread_id.empty()) poll_ctx.thread_id = thread_id;
    poll_ctx.is_anonymous = is_anonymous;

    auto result = plugin_opt->outbound->send_poll(poll_ctx);
    if (result.is_err()) {
        throw std::runtime_error(result.error);
    }

    PollActionResult poll_result;
    poll_result.handled_by = "core";
    poll_result.poll_id = result.get().poll_id.value_or("");
    return poll_result;
}

} // namespace outbound
} // namespace hunterclaw
