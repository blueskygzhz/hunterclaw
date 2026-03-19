// Aligned with openclaw: src/infra/outbound/deliver.ts
//
// deliverOutboundPayloads:
//   1. Write-ahead enqueueDelivery (skip_queue=false)
//   2. Wrap onError to track hadPartialFailure (aligned with openclaw)
//   3. deliverOutboundPayloadsCore → ChannelOutboundAdapter::send_text / send_media
//   4. ackDelivery on success / failDelivery on partial or full error
//   5. AbortError → ack (treat as intentional cancel, not a failure)
//
// deliverOutboundPayloadsCore:
//   - createChannelHandler (ChannelPluginRegistry::get_plugin)
//   - normalizePayloadsForChannelDelivery (whatsapp strip, plain-text sanitize)
//   - per-payload loop:
//     - throwIfAborted (abortSignal check)
//     - sendPayload path (channelData present + adapter.send_payload)
//     - text-only path: sendTextChunks (with chunker support)
//     - media path: per-url send_media (or fallback to send_text if !supportsMedia)
//   - bestEffort: per-payload errors caught, passed to on_error, don't throw

#include "hunterclaw/outbound/deliver.h"
#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/observability/diagnostic_events.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <regex>
#include <stdexcept>

namespace hunterclaw {
namespace outbound {

namespace {

auto log() {
    static auto logger = []() {
        auto l = spdlog::get("outbound/deliver");
        if (!l) l = spdlog::stdout_logger_mt("outbound/deliver");
        return l;
    }();
    return logger;
}

// ============================================================================
// throwIfAborted
// Aligned with openclaw: throwIfAborted (abort.ts)
// ============================================================================
static void throw_if_aborted(const std::shared_ptr<std::atomic<bool>>& abort_signal) {
    if (abort_signal && abort_signal->load()) {
        throw std::runtime_error("AbortError: delivery aborted");
    }
}

static bool is_abort_error(const std::string& msg) {
    return msg.find("AbortError") != std::string::npos ||
           msg.find("aborted") != std::string::npos;
}

// ============================================================================
// isPlainTextSurface
// Aligned with openclaw: isPlainTextSurface (sanitize-text.ts)
// Plain-text surfaces: whatsapp, signal, sms, imessage, line, etc.
// ============================================================================
static bool is_plain_text_surface(const std::string& channel_id) {
    static const std::vector<std::string> PLAIN_TEXT_CHANNELS = {
        "whatsapp", "signal", "sms", "imessage", "line", "viber"
    };
    for (const auto& ch : PLAIN_TEXT_CHANNELS) {
        if (channel_id == ch) return true;
    }
    return false;
}

// ============================================================================
// sanitizeForPlainText
// Aligned with openclaw: sanitizeForPlainText (sanitize-text.ts)
// Strip HTML tags that models occasionally produce (<br>, <b>, etc.)
// ============================================================================
static std::string sanitize_for_plain_text(const std::string& text) {
    // Strip HTML tags
    static const std::regex html_tag_re("<[^>]+>");
    std::string result = std::regex_replace(text, html_tag_re, "");
    // Normalize <br> → newline (already stripped above, but handle &lt;br&gt; etc. if needed)
    return result;
}

// ============================================================================
// normalizePayloadForChannelDelivery
// Aligned with openclaw: normalizePayloadForChannelDelivery (deliver.ts)
// ============================================================================
static std::optional<queue::MessagePayload> normalize_payload_for_channel(
    const queue::MessagePayload& payload,
    const std::string& channel_id
) {
    bool has_media = !payload.media_urls.empty();
    bool has_channel_data = payload.channel_data.has_value() &&
                            !payload.channel_data->empty();

    std::string raw_text = payload.text;

    // Plain-text surface: strip HTML tags (aligned with openclaw)
    // Exception: telegram sendPayload with channelData uses textMode:"html" — preserve raw HTML
    std::string sanitized_text = raw_text;
    if (is_plain_text_surface(channel_id) && !raw_text.empty()) {
        bool is_telegram_channel_data = (channel_id == "telegram" && has_channel_data);
        if (!is_telegram_channel_data) {
            sanitized_text = sanitize_for_plain_text(raw_text);
        }
    }

    // WhatsApp: strip leading blank lines (aligned with openclaw)
    std::string normalized_text = sanitized_text;
    if (channel_id == "whatsapp") {
        static const std::regex leading_blank_re("^(?:[ \\t]*\\r?\\n)+");
        normalized_text = std::regex_replace(sanitized_text, leading_blank_re, "");
    }

    // If text is empty after normalization
    if (normalized_text.empty() || normalized_text.find_first_not_of(" \t\r\n") == std::string::npos) {
        if (!has_media && !has_channel_data) {
            // Skip this payload entirely (aligned with openclaw: return null)
            return std::nullopt;
        }
        // Keep payload but with empty text
        queue::MessagePayload result = payload;
        result.text = "";
        return result;
    }

    if (normalized_text == raw_text) {
        return payload;
    }

    queue::MessagePayload result = payload;
    result.text = normalized_text;
    return result;
}

// ============================================================================
// normalizePayloadsForChannelDelivery
// Aligned with openclaw: normalizePayloadsForChannelDelivery (deliver.ts)
// ============================================================================
static std::vector<queue::MessagePayload> normalize_payloads_for_channel(
    const std::vector<queue::MessagePayload>& payloads,
    const std::string& channel_id
) {
    std::vector<queue::MessagePayload> result;
    for (const auto& payload : payloads) {
        auto normalized = normalize_payload_for_channel(payload, channel_id);
        if (normalized.has_value()) {
            result.push_back(*normalized);
        }
        // null → skip (aligned with openclaw)
    }
    return result;
}

// ============================================================================
// buildPayloadSummary
// Aligned with openclaw: buildPayloadSummary (deliver.ts)
// ============================================================================
static NormalizedOutboundPayload build_payload_summary(
    const queue::MessagePayload& payload
) {
    NormalizedOutboundPayload summary;
    summary.text = payload.text;
    summary.media_urls = payload.media_urls;
    if (payload.channel_data.has_value()) {
        summary.channel_data = *payload.channel_data;
    }
    return summary;
}

// ============================================================================
// loadChannelOutboundAdapter
// Aligned with openclaw: loadChannelOutboundAdapter / createChannelHandler
// ============================================================================
static std::optional<channels::ChannelOutboundAdapter> load_channel_outbound_adapter(
    const std::string& channel_id
) {
    auto plugin_opt = channels::ChannelPluginRegistry::instance().get_plugin(channel_id);
    if (!plugin_opt.has_value()) {
        return std::nullopt;
    }
    return plugin_opt->outbound;  // std::optional<ChannelOutboundAdapter>
}

// ============================================================================
// sendTextChunks
// Aligned with openclaw: sendTextChunks (deliver.ts)
// Applies chunker if available, otherwise sends as-is.
// ============================================================================
static void send_text_chunks(
    const std::string& text,
    const channels::ChannelOutboundContext& base_ctx,
    const channels::ChannelOutboundAdapter& adapter,
    const std::shared_ptr<std::atomic<bool>>& abort_signal,
    std::vector<OutboundDeliveryResult>& results
) {
    throw_if_aborted(abort_signal);

    // ChannelOutboundAdapter is a value type with std::function fields
    bool has_chunker = (bool)adapter.chunker;
    bool has_limit   = adapter.text_chunk_limit.has_value();

    if (!has_chunker || !has_limit) {
        // No chunker: send as-is (aligned with openclaw)
        if (!adapter.send_text) throw std::runtime_error("send_text not implemented");
        channels::ChannelOutboundContext ctx = base_ctx;
        ctx.text = text;
        auto r = adapter.send_text(ctx);
        if (r.is_err()) throw std::runtime_error(r.error);
        results.push_back(r.get());
        return;
    }

    int text_limit = *adapter.text_chunk_limit;
    auto chunks = adapter.chunker(text, text_limit);
    if (chunks.empty() && !text.empty()) {
        chunks.push_back(text);
    }

    for (const auto& chunk : chunks) {
        throw_if_aborted(abort_signal);
        channels::ChannelOutboundContext chunk_ctx = base_ctx;
        chunk_ctx.text = chunk;
        if (!adapter.send_text) throw std::runtime_error("send_text not implemented");
        auto r = adapter.send_text(chunk_ctx);
        if (r.is_err()) throw std::runtime_error(r.error);
        results.push_back(r.get());
    }
}

} // namespace

// ============================================================================
// deliver_outbound_payloads_core
// Aligned with openclaw: deliverOutboundPayloadsCore (deliver.ts)
// ============================================================================
core::Result<std::vector<OutboundDeliveryResult>> deliver_outbound_payloads_core(
    const DeliverOutboundPayloadsParams& params
) {
    const std::string& channel_id = params.channel;
    const std::string& to = params.to;

    // 1. Load channel outbound adapter (aligned with openclaw: createChannelHandler)
    auto outbound_opt = load_channel_outbound_adapter(channel_id);
    if (!outbound_opt.has_value()) {
        return core::Result<std::vector<OutboundDeliveryResult>>::Err(
            "Outbound not configured for channel: " + channel_id
        );
    }
    const channels::ChannelOutboundAdapter& outbound = *outbound_opt;

    // 2. Normalize payloads (aligned with openclaw: normalizePayloadsForChannelDelivery)
    auto normalized_payloads = normalize_payloads_for_channel(params.payloads, channel_id);

    // 3. Build base outbound context
    channels::ChannelOutboundContext base_ctx;
    base_ctx.cfg = core::Config{};  // TODO: pass cfg through params
    base_ctx.to = to;
    base_ctx.account_id = params.account_id;
    base_ctx.reply_to_id = params.reply_to_id;
    base_ctx.thread_id = params.thread_id;
    base_ctx.silent = params.silent ? std::optional<bool>(true) : std::nullopt;
    base_ctx.gif_playback = params.gif_playback ? std::optional<bool>(true) : std::nullopt;

    // supports_media: true if send_media function is set
    bool supports_media = (bool)outbound.send_media;

    std::vector<OutboundDeliveryResult> results;

    // 4. Per-payload send loop (aligned with openclaw: for (const payload of normalizedPayloads))
    for (const auto& payload : normalized_payloads) {
        NormalizedOutboundPayload payload_summary = build_payload_summary(payload);

        try {
            // throwIfAborted (aligned with openclaw)
            throw_if_aborted(params.abort_signal);

            // Notify caller about this payload (aligned with openclaw: onPayload)
            if (params.on_payload) {
                params.on_payload(payload_summary);
            }

            // ── sendPayload path (aligned with openclaw: handler.sendPayload && channelData) ──
            bool has_channel_data = payload.channel_data.has_value() &&
                                    !payload.channel_data->empty();
            if (has_channel_data && outbound.send_payload) {
                channels::ChannelOutboundPayloadContext payload_ctx;
                static_cast<channels::ChannelOutboundContext&>(payload_ctx) = base_ctx;
                payload_ctx.text = payload.text;
                payload_ctx.payload = *payload.channel_data;

                auto send_payload_result = outbound.send_payload(payload_ctx);
                // send_payload returns Result directly (not optional)
                if (send_payload_result.is_err()) {
                    throw std::runtime_error(send_payload_result.error);
                }
                results.push_back(send_payload_result.get());
                continue;
            }

            // ── Text-only path (aligned with openclaw: mediaUrls.length === 0) ──
            if (payload_summary.media_urls.empty()) {
                channels::ChannelOutboundContext text_ctx = base_ctx;
                text_ctx.text = payload_summary.text;
                send_text_chunks(payload_summary.text, text_ctx, outbound,
                                 params.abort_signal, results);
                continue;
            }

            // ── Media path ──────────────────────────────────────────────────
            if (!supports_media) {
                // Aligned with openclaw: adapter does not implement sendMedia
                // → warn and fall back to sendText with caption
                log()->warn(
                    "deliver_outbound_payloads_core: channel '{}' outbound adapter does not "
                    "implement send_media; media URLs will be dropped and text fallback used",
                    channel_id
                );
                std::string fallback_text = payload_summary.text;
                // Trim
                auto first = fallback_text.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) {
                    // No text fallback available (aligned with openclaw: throw)
                    throw std::runtime_error(
                        "Plugin outbound adapter does not implement send_media "
                        "and no text fallback is available for media payload"
                    );
                }
                channels::ChannelOutboundContext text_ctx = base_ctx;
                text_ctx.text = fallback_text;
                send_text_chunks(fallback_text, text_ctx, outbound,
                                 params.abort_signal, results);
                continue;
            }

            // Send each media URL (aligned with openclaw: for (const url of mediaUrls))
            bool first = true;
            for (const auto& url : payload_summary.media_urls) {
                throw_if_aborted(params.abort_signal);

                channels::ChannelOutboundContext media_ctx = base_ctx;
                // First media gets the caption; subsequent ones get empty text
                media_ctx.text = first ? payload_summary.text : "";
                media_ctx.media_url = url;
                first = false;

                if (!outbound.send_media) throw std::runtime_error("send_media not implemented");
                auto r = outbound.send_media(media_ctx);
                if (r.is_err()) throw std::runtime_error(r.error);
                results.push_back(r.get());
            }

        } catch (const std::exception& e) {
            std::string error_msg = e.what();

            if (!params.best_effort) {
                // Re-throw (aligned with openclaw: if (!bestEffort) throw err)
                throw;
            }

            // bestEffort: catch per-payload errors, pass to onError, continue
            // (aligned with openclaw: params.onError?.(err, payloadSummary))
            log()->warn("deliver_outbound_payloads_core: bestEffort payload error: {}", error_msg);
            if (params.on_error) {
                params.on_error(error_msg, payload_summary);
            }
        }
    }

    return core::Result<std::vector<OutboundDeliveryResult>>::Ok(results);
}

// ============================================================================
// deliver_outbound_payloads
// Aligned with openclaw: deliverOutboundPayloads (deliver.ts)
//
// Write-ahead queue wrapper around deliverOutboundPayloadsCore:
//   1. enqueueDelivery (skip_queue=false) — best-effort, don't block on failure
//   2. Wrap onError to detect partial failures (hadPartialFailure tracking)
//   3. deliverOutboundPayloadsCore
//   4. ackDelivery on full success (best-effort cleanup)
//      failDelivery on partial bestEffort failure
//   5. On exception:
//      AbortError → ackDelivery (intentional cancel)
//      Other → failDelivery; re-throw
// ============================================================================
core::Result<std::vector<OutboundDeliveryResult>> deliver_outbound_payloads(
    const DeliverOutboundPayloadsParams& params
) {
    const std::string& channel_id = params.channel;
    const std::string& to = params.to;

    // ── Step 1: Write-ahead enqueue ──────────────────────────────────────────
    // Aligned with openclaw: const queueId = params.skipQueue ? null : await enqueueDelivery(...)
    std::string queue_id;
    if (!params.skip_queue && params.delivery_queue) {
        queue::DeliveryTarget target;
        target.channel = channel_id;
        target.to = to;
        target.account_id = params.account_id;
        target.thread_id = params.thread_id;
        target.reply_to_id = params.reply_to_id;

        // Best-effort: don't block delivery if queue write fails
        // (aligned with openclaw: .catch(() => null))
        auto eq = params.delivery_queue->enqueue(target, params.payloads);
        if (eq.is_ok()) {
            queue_id = eq.get();
        } else {
            log()->warn("deliver_outbound_payloads: enqueue failed (best-effort): {}", eq.error);
        }
    }

    // ── Step 2: Wrap onError to detect partial failures ──────────────────────
    // Aligned with openclaw: hadPartialFailure tracking
    // When bestEffort=true, per-payload errors are caught and passed to onError
    // without throwing — so the outer try/catch never fires. We track whether any
    // payload failed so we can call failDelivery instead of ackDelivery.
    bool had_partial_failure = false;
    DeliverOutboundPayloadsParams wrapped_params = params;
    if (params.on_error) {
        wrapped_params.on_error = [&had_partial_failure, &params](
            const std::string& error,
            const NormalizedOutboundPayload& payload
        ) {
            had_partial_failure = true;
            params.on_error(error, payload);
        };
    } else if (params.best_effort) {
        // Even without an onError callback, track partial failures
        wrapped_params.on_error = [&had_partial_failure](
            const std::string& /*error*/,
            const NormalizedOutboundPayload& /*payload*/
        ) {
            had_partial_failure = true;
        };
    }

    // ── Step 3: Core delivery ────────────────────────────────────────────────
    try {
        auto results = deliver_outbound_payloads_core(wrapped_params);

        if (!queue_id.empty() && params.delivery_queue) {
            if (had_partial_failure) {
                // Aligned with openclaw: failDelivery on partial bestEffort failure
                params.delivery_queue->fail(queue_id, "partial delivery failure (bestEffort)");
            } else {
                // Aligned with openclaw: ackDelivery on full success (best-effort cleanup)
                params.delivery_queue->ack(queue_id);
            }
        }

        return results;

    } catch (const std::exception& e) {
        std::string error_msg = e.what();

        if (!queue_id.empty() && params.delivery_queue) {
            // Aligned with openclaw: AbortError → ack; other errors → fail
            if (is_abort_error(error_msg)) {
                // Aligned with openclaw: treat abort as intentional cancel → ack
                params.delivery_queue->ack(queue_id);
            } else {
                params.delivery_queue->fail(queue_id, error_msg);
            }
        }

        return core::Result<std::vector<OutboundDeliveryResult>>::Err(error_msg);
    }
}

} // namespace outbound
} // namespace hunterclaw
