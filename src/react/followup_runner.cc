#include "hunterclaw/react/followup_runner.h"
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace react {

namespace {
auto log() {
    static auto logger = spdlog::get("react");
    if (!logger) logger = spdlog::stdout_logger_mt("react");
    return logger;
}

bool is_silent_reply(const std::string& text) {
    std::string trimmed = text;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return true;
    auto e = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(s, e - s + 1);
    return trimmed.empty() || trimmed == "[SILENT]" || trimmed.find("[SILENT]") == 0;
}

bool has_media(const FollowupRunPayload& payload) {
    return payload.media_url.has_value() || !payload.media_urls.empty();
}
}

// ============================================================================
// createFollowupRunner (对齐 followup-runner.ts:38-403)
// ============================================================================

std::function<void(const FollowupRunItem& item)> create_followup_runner(
    const FollowupRunnerParams& params
) {
    // 对齐: create typing signaler
    TypingSignaler* signaler_ptr = nullptr;
    std::shared_ptr<TypingSignaler> signaler_holder;
    if (params.typing) {
        signaler_holder = std::make_shared<TypingSignaler>(
            create_typing_signaler(*params.typing, params.typing_mode, params.is_heartbeat));
        signaler_ptr = signaler_holder.get();
    }

    return [params, signaler_ptr, signaler_holder](const FollowupRunItem& item) {
        // 对齐: replyOperation = createReplyOperation(...)
        auto reply_op = ReplyRunRegistry::instance().begin(
            item.run.session_key.empty() ? params.session_key : item.run.session_key,
            item.run.session_id,
            false  // reset_triggered
        );

        if (!reply_op) {
            log()->warn("Followup run: failed to create reply operation for '{}'",
                       item.run.session_key);
            return;
        }

        try {
            // Check abort
            if (params.abort_signal && params.abort_signal->load()) {
                reply_op->abort_by_user();
                return;
            }

            // 对齐: replyOperation.setPhase("running")
            reply_op->set_phase(ReplyOperationPhase::Running);

            // 对齐: run the agent
            std::vector<FollowupRunPayload> payloads;
            if (params.run_agent) {
                try {
                    payloads = params.run_agent(item);
                } catch (const std::exception& e) {
                    reply_op->fail(ReplyOperationFailureCode::RunFailed);
                    log()->error("Followup agent failed: {}", e.what());
                    return;
                }
            }

            if (payloads.empty()) {
                reply_op->complete();
                return;
            }

            // 对齐: sanitize payloads — strip silent, heartbeat
            std::vector<FollowupRunPayload> final_payloads;
            for (const auto& p : payloads) {
                if (is_silent_reply(p.text) && !has_media(p)) {
                    continue;
                }
                final_payloads.push_back(p);
            }

            if (final_payloads.empty()) {
                reply_op->complete();
                return;
            }

            // 对齐: sendFollowupPayloads
            bool should_route = item.originating_channel.has_value() &&
                               item.originating_to.has_value();

            for (const auto& payload : final_payloads) {
                // Signal typing
                if (signaler_ptr && signaler_ptr->signal_text_delta) {
                    signaler_ptr->signal_text_delta(payload.text);
                }

                if (should_route && params.route_to_originating) {
                    bool routed = params.route_to_originating(payload, item);
                    if (!routed && params.send_payload) {
                        params.send_payload(payload, item);
                    }
                } else if (params.send_payload) {
                    params.send_payload(payload, item);
                }
            }

            reply_op->complete();

        } catch (const std::exception& e) {
            reply_op->fail(ReplyOperationFailureCode::RunFailed);
            log()->error("Followup runner error: {}", e.what());
        } catch (...) {
            reply_op->complete();
        }

        // 对齐: typing.markRunComplete() + typing.markDispatchIdle()
        if (params.typing) {
            if (params.typing->mark_run_complete) params.typing->mark_run_complete();
            if (params.typing->mark_dispatch_idle) params.typing->mark_dispatch_idle();
        }
    };
}

}  // namespace react
}  // namespace hunterclaw
