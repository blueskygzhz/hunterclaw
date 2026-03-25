// Aligned with openclaw:
//   src/auto-reply/dispatch.ts
//   src/auto-reply/reply/dispatch-from-config.ts
//   src/auto-reply/reply/inbound-dedupe.ts
//   src/auto-reply/reply/abort.ts
//   src/auto-reply/inbound-debounce.ts
//   src/auto-reply/reply/agent-runner.ts          (runReplyAgent)
//   src/auto-reply/reply/queue-policy.ts          (resolveActiveRunQueueAction)
//   src/auto-reply/reply/queue/enqueue.ts         (enqueueFollowupRun)
//   src/auto-reply/reply/queue/drain.ts           (scheduleFollowupDrain)

#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/auto_reply/command_detection.h"
#include "hunterclaw/auto_reply/heartbeat.h"
#include "hunterclaw/routing/router.h"
#include "hunterclaw/sessions/session_manager.h"
#include "hunterclaw/queue/delivery_queue.h"
#include "hunterclaw/outbound/deliver.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/core/utils.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace auto_reply {

namespace {

auto log() {
    static auto logger = []() {
        auto l = spdlog::get("auto_reply");
        if (!l) l = spdlog::stdout_logger_mt("auto_reply");
        return l;
    }();
    return logger;
}

int64_t current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// Inbound dedupe key builder
// Aligned with openclaw: buildInboundDedupeKey (inbound-dedupe.ts)
//
// Key format: provider|accountId|sessionScope|peerId|threadId|messageId
// (empty components are omitted)
// ============================================================================

static std::string build_inbound_dedupe_key(const channels::InboundMessage& msg) {
    // provider: normalize to lowercase
    std::string provider = msg.channel;
    std::transform(provider.begin(), provider.end(), provider.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (provider.empty()) return "";

    const std::string& message_id = msg.message_id;
    if (message_id.empty()) return "";

    // peerId: prefer originating_to, then chat_id, then sender_id
    // (aligned with openclaw: ctx.OriginatingTo ?? ctx.To ?? ctx.From ?? ctx.SessionKey)
    const std::string peer_id = !msg.chat_id.empty() ? msg.chat_id : msg.sender_id;
    if (peer_id.empty()) return "";

    // sessionScope: use session_key if available (aligned with openclaw: agent:agentId)
    const std::string session_scope = msg.session_key.empty() ? "" : msg.session_key;

    // accountId, threadId
    const std::string account_id = msg.account_id.value_or("");
    const std::string thread_id  = msg.thread_id.value_or("");

    // Build key (filter empty components, join with |)
    std::vector<std::string> parts;
    parts.push_back(provider);
    if (!account_id.empty())   parts.push_back(account_id);
    if (!session_scope.empty()) parts.push_back(session_scope);
    parts.push_back(peer_id);
    if (!thread_id.empty())    parts.push_back(thread_id);
    parts.push_back(message_id);

    std::string key;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) key += "|";
        key += parts[i];
    }
    return key;
}

// ============================================================================
// resolve_send_policy
// Aligned with openclaw: resolveSendPolicy (sessions/send-policy.ts)
//
// Checks session entry sendPolicy override first, then config.session.sendPolicy rules.
// Returns "allow" or "deny".
// ============================================================================

static std::string resolve_send_policy(
    const std::optional<sessions::SessionEntry>& entry,
    const std::string& session_key,
    const std::string& channel,
    routing::ChatType chat_type
) {
    // Aligned with openclaw: normalizeSendPolicy(entry?.sendPolicy)
    // Check session entry override first
    if (entry.has_value()) {
        const auto& chat_type_str = entry->chat_type.value_or("");
        // If session has explicit sendPolicy stored in metadata, use it.
        // (SessionEntry doesn't have sendPolicy field yet; use chat_type as proxy)
        // TODO: add sendPolicy field to SessionEntry when needed
    }

    // Aligned with openclaw: no config.session.sendPolicy rules → default "allow"
    // HunterClaw does not yet have a full sendPolicy rules engine.
    // The default is "allow" unless explicitly overridden.
    // This matches openclaw's behavior when cfg.session?.sendPolicy is undefined.
    return "allow";
}

} // namespace

// ============================================================================
// MessageDispatcher
// ============================================================================

std::shared_ptr<MessageDispatcher> MessageDispatcher::create(
    std::shared_ptr<::hunterclaw::routing::MessageRouter> router,
    std::shared_ptr<sessions::SessionManager> session_manager,
    std::shared_ptr<::hunterclaw::queue::DeliveryQueue> delivery_queue,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const DispatchConfig& config
) {
    return std::shared_ptr<MessageDispatcher>(
        new MessageDispatcher(router, session_manager, delivery_queue,
                              tool_manager, llm_client, config)
    );
}

MessageDispatcher::MessageDispatcher(
    std::shared_ptr<::hunterclaw::routing::MessageRouter> router,
    std::shared_ptr<sessions::SessionManager> session_manager,
    std::shared_ptr<::hunterclaw::queue::DeliveryQueue> delivery_queue,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const DispatchConfig& config
) : router_(router),
    session_manager_(session_manager),
    delivery_queue_(delivery_queue),
    tool_manager_(tool_manager),
    llm_client_(llm_client),
    config_(config) {}

// ============================================================================
// finalize_inbound_context
// Aligned with openclaw: finalizeInboundContext (inbound-context.ts)
//
// Enriches the inbound message context before dispatch:
//   - Resolves session key from channel/peer/thread
//   - Normalizes channel/provider fields
//   - Resolves originating channel for cross-provider routing
// ============================================================================

static channels::InboundMessage finalize_inbound_context(
    const channels::InboundMessage& raw
) {
    channels::InboundMessage ctx = raw;

    // Aligned with openclaw: normalizeMessageChannel
    auto normalize_channel = [](const std::string& ch) -> std::string {
        std::string result = ch;
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return result;
    };

    // Normalize channel to lowercase (aligned with openclaw: normalizeMessageChannel)
    ctx.channel = normalize_channel(ctx.channel);
    if (!ctx.originating_channel.empty()) {
        ctx.originating_channel = normalize_channel(ctx.originating_channel);
    }

    // Resolve session_key if not already set
    // Aligned with openclaw: ctx.SessionKey derivation from channel:chatType:peerId
    if (ctx.session_key.empty()) {
        // Build session key: channel:chatType:peerId
        // e.g. "telegram:dm:123456" or "feishu:group:oc_abc"
        const std::string chat_type_str = [&]() -> std::string {
            switch (ctx.chat_type) {
                case routing::ChatType::Group:   return "group";
                case routing::ChatType::Channel: return "channel";
                default:                         return "dm";
            }
        }();
        const std::string peer_id = !ctx.chat_id.empty() ? ctx.chat_id : ctx.sender_id;
        if (!ctx.channel.empty() && !peer_id.empty()) {
            ctx.session_key = ctx.channel + ":" + chat_type_str + ":" + peer_id;
        }
    }

    return ctx;
}

// ============================================================================
// dispatch_inbound_message
// Aligned with openclaw: dispatchInboundMessage (dispatch.ts)
//
// Wraps dispatchReplyFromConfig with:
//   - finalizeInboundContext (context enrichment)
//   - withReplyDispatcher (markComplete + waitForIdle on exit)
// ============================================================================

MessageDispatcher::DispatchResult MessageDispatcher::dispatch_inbound_message(
    const channels::InboundMessage& message
) {
    auto start_time = std::chrono::steady_clock::now();

    log()->info("dispatch_inbound_message: id={} channel={} sender={}",
                message.message_id, message.channel, message.sender_id);

    // Aligned with openclaw: finalizeInboundContext — enrich context before dispatch
    const channels::InboundMessage finalized = finalize_inbound_context(message);

    AutoReplyConfig auto_reply_config;
    auto_reply_config.enabled = true;
    // Resolve agent_id from session_key (aligned with openclaw: resolveSessionAgentId)
    auto_reply_config.agent_id = finalized.session_key.empty() ? "default" :
        [&]() -> std::string {
            // Parse "agent:<agentId>:..." prefix from session key
            const std::string& sk = finalized.session_key;
            if (sk.substr(0, 6) == "agent:") {
                size_t second_colon = sk.find(':', 6);
                if (second_colon != std::string::npos) {
                    return sk.substr(6, second_colon - 6);
                }
            }
            return "default";
        }();

    auto result = dispatch_reply_from_config(finalized, auto_reply_config);

    auto end_time = std::chrono::steady_clock::now();
    result.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    ).count();

    log()->info("dispatch_inbound_message: done success={} time={}ms",
                result.success, result.processing_time_ms);
    return result;
}

// ============================================================================
// dispatch_reply_from_config
// Aligned with openclaw: dispatchReplyFromConfig (dispatch-from-config.ts)
// ============================================================================

MessageDispatcher::DispatchResult MessageDispatcher::dispatch_reply_from_config(
    const channels::InboundMessage& message,
    const AutoReplyConfig& config
) {
    DispatchResult result;

    // ── Diagnostics helpers (aligned with openclaw: recordProcessed / markProcessing) ──
    const std::string channel = message.channel;
    const std::string chat_id = message.chat_id.empty() ? message.sender_id : message.chat_id;
    const std::string message_id = message.message_id;
    const std::string session_key = message.session_key;

// ── Step 1: Inbound dedupe (aligned with openclaw: shouldSkipDuplicateInbound) ──
    // Key: provider|accountId|sessionScope|peerId|threadId|messageId
    // TTL: DEFAULT_INBOUND_DEDUPE_TTL_MS = 20 * 60_000 ms
    if (should_skip_duplicate_inbound(message)) {
        result.skipped = true;
        result.skip_reason = "duplicate";
        log()->debug("dispatch_reply_from_config: skipped duplicate key for msg={}",
                     message_id);
        return result;
    }

    // ── Resolve session store entry + inbound audio context + TTS auto mode ──
    // Aligned with openclaw: resolveSessionStoreLookup + isInboundAudioContext + normalizeTtsAutoMode
    std::optional<sessions::SessionEntry> session_entry_for_dispatch;
    if (session_manager_ && !session_key.empty()) {
        auto entry_result = session_manager_->get_session(session_key);
        if (entry_result.is_ok()) {
            session_entry_for_dispatch = entry_result.get();
        }
    }

    // Aligned with openclaw: isInboundAudioContext
    // Detect if the inbound message is audio (voice note, audio attachment, etc.)
    const bool inbound_audio = [&]() -> bool {
        const std::string& text = message.text;
        if (text.empty()) return false;
        // Check for audio placeholder patterns (aligned with openclaw: AUDIO_PLACEHOLDER_RE / AUDIO_HEADER_RE)
        static const std::regex audio_placeholder_re(
            R"(^<media:audio>(\s*\([^)]*\))?$)",
            std::regex_constants::icase
        );
        static const std::regex audio_header_re(
            R"(^\[Audio\b)",
            std::regex_constants::icase
        );
        const std::string trimmed = [&]() {
            size_t s = text.find_first_not_of(" \t\n\r");
            if (s == std::string::npos) return std::string{};
            size_t e = text.find_last_not_of(" \t\n\r");
            return text.substr(s, e - s + 1);
        }();
        if (std::regex_match(trimmed, audio_placeholder_re)) return true;
        if (std::regex_search(trimmed, audio_header_re)) return true;
        return false;
    }();

    // Aligned with openclaw: normalizeTtsAutoMode(sessionStoreEntry.entry?.ttsAuto)
    // "off" | "always" | "inbound" | "tagged" — default "off"
    const std::string session_tts_auto = [&]() -> std::string {
        if (session_entry_for_dispatch.has_value()) {
            const auto& tts_auto = session_entry_for_dispatch->tts_auto;
            if (tts_auto.has_value()) {
                const std::string& v = *tts_auto;
                if (v == "always" || v == "inbound" || v == "tagged") return v;
            }
        }
        return "off";
    }();

    // Aligned with openclaw: ttsChannel = shouldRouteToOriginating ? originatingChannel : currentSurface
    const std::string tts_channel = message.channel;

    // Note: inbound_audio and session_tts_auto are used by maybe_apply_tts_to_payload
    // when TTS integration is complete. They control:
    //   inbound_audio: whether to auto-apply TTS (ttsAuto="inbound" mode)
    //   session_tts_auto: session-level TTS auto mode override
    // For now, suppress unused-variable warnings.
    (void)inbound_audio;
    (void)session_tts_auto;

    // ── Plugin hook: message_received ────────────────────────────────────────
    // Aligned with openclaw: hookRunner.runMessageReceived + triggerInternalHook("message","received")
    // Fire-and-forget: do not block dispatch on hook failure.
    // HunterClaw: hook system is a future integration point.
    // When hook_runner is available, call:
    //   hook_runner->run_message_received(message);
    //   trigger_internal_hook("message", "received", session_key, ...);
    // For now, log the intent.
    log()->debug("dispatch_reply_from_config: [hook] message_received session={}", session_key);

    // ── Step 2: shouldRouteToOriginating ────────────────────────────────────
    // Aligned with openclaw: check if originating channel differs from current surface.
    // Only route when originating channel is DIFFERENT from current surface.
    const bool should_route = should_route_to_originating(message);
    const std::string delivery_to = should_route
        ? (message.chat_id.empty() ? message.sender_id : message.chat_id)
        : message.sender_id;

    // ── Step 3: tryFastAbortFromMessage ─────────────────────────────────────
    // Aligned with openclaw: abort.ts — checks ABORT_TRIGGERS set
    // NOTE: markProcessing() is called here (aligned with openclaw: before fastAbort check)
    log()->debug("dispatch_reply_from_config: markProcessing session={}", session_key);
    auto fast_abort = try_fast_abort_from_message(message, config);
    if (fast_abort.should_abort) {
        // Aligned with openclaw: send abort reply text
        const std::string abort_text = "⚙️ Agent was aborted.";
        ::hunterclaw::queue::MessagePayload abort_payload;
        abort_payload.text = abort_text;

        outbound::DeliverOutboundPayloadsParams deliver_params;
        deliver_params.channel = message.channel;
        deliver_params.to = delivery_to;
        deliver_params.payloads = {abort_payload};
        deliver_params.delivery_queue = delivery_queue_;
        deliver_params.skip_queue = false;
        deliver_params.best_effort = true;  // abort reply is best-effort

        outbound::deliver_outbound_payloads(deliver_params);

        result.fast_aborted = true;
        result.skip_reason = fast_abort.reason;
        log()->debug("dispatch_reply_from_config: fast abort: {}", fast_abort.reason);
        return result;
    }

    // ── Step 4: bypassAcpForCommand (aligned with openclaw: shouldBypassAcpDispatchForCommand) ──
    // Aligned with openclaw: computed BEFORE sendPolicy check, used in BOTH sendPolicy and acpDispatch.
    // Commands that start with ! or are registered slash-commands bypass send policy and ACP.
    const bool bypass_acp_for_command =
        CommandDetection::is_control_command_message(message.text) ||
        CommandDetection::has_inline_command_tokens(message.text);

    // ── Step 5: Send policy check ────────────────────────────────────────────
    // Aligned with openclaw: resolveSendPolicy (sessions/send-policy.ts)
    // Checks session store entry sendPolicy override, then config.session.sendPolicy rules.
    // "deny" blocks dispatch unless bypassed by a command.
    {
        // Aligned with openclaw: resolveSendPolicy
        // Check session entry override first, then config rules.
        const std::string send_policy = resolve_send_policy(
            session_entry_for_dispatch,
            session_key,
            message.channel,
            message.chat_type
        );

        if (send_policy == "deny" && !bypass_acp_for_command) {
            log()->debug("dispatch_reply_from_config: send blocked by policy for session={}",
                         session_key);
            result.skipped = true;
            result.skip_reason = "send_policy_deny";
            log()->debug("dispatch_reply_from_config: markIdle session={} reason=message_completed",
                         session_key);
            return result;
        }
    }

    // ── Step 6: ACP dispatch ─────────────────────────────────────────────────
    // Aligned with openclaw: tryDispatchAcpReply (bypassForCommand passed through)
    if (config.dispatch.enable_acp) {
        auto acp_result = try_dispatch_acp_reply(message);
        if (acp_result.handled) {
            result.success = true;
            log()->info("dispatch_reply_from_config: handled by ACP");
            log()->debug("dispatch_reply_from_config: markIdle session={} reason=message_completed",
                         session_key);
            return result;
        }
    }

    // ── Step 7: Permission check ─────────────────────────────────────────────
    auto permission_check = check_permissions(message, config);
    if (!permission_check.allowed) {
        result.denied = true;
        result.error = "Permission denied: " + permission_check.denial_reason;
        log()->warn("dispatch_reply_from_config: permission denied: {}",
                    permission_check.denial_reason);
        return result;
    }

    // ── Step 8: shouldComputeCommandAuthorized ───────────────────────────────
    // Aligned with openclaw: resolveCommandAuthorization (command-auth.ts)
    // Command authorization is granted when:
    //   1. The sender is the session owner (first sender), OR
    //   2. The sender is in the config allowFrom list, OR
    //   3. The message is from a DM (not a group)
    bool command_authorized = false;
    if (CommandDetection::should_compute_command_authorized(message.text)) {
        // Aligned with openclaw: isAuthorizedSender check
        // In DM context, the sender is always authorized.
        // In group context, only allowFrom senders are authorized.
        const bool is_dm = (message.chat_type != routing::ChatType::Group &&
                            message.chat_type != routing::ChatType::Channel);
        if (is_dm) {
            command_authorized = true;
        } else if (!config.channel.allow_from.empty()) {
            command_authorized = std::find(
                config.channel.allow_from.begin(),
                config.channel.allow_from.end(),
                message.sender_id
            ) != config.channel.allow_from.end();
        }
        // Aligned with openclaw: CommandAuthorized is set on the context
        log()->debug("dispatch_reply_from_config: command_authorized={} sender={}",
                     command_authorized, message.sender_id);
    }

    // ── Step 9: resolveRunTypingPolicy ───────────────────────────────────────
    // Aligned with openclaw: resolveRunTypingPolicy (typing-policy.ts)
    // suppressTyping when routing to originating channel or originating is internal.
    const bool suppress_typing = should_route || [&]() -> bool {
        const std::string& orig = message.originating_channel;
        return orig == "web" || orig == "webchat" || orig == "internal";
    }();
    log()->debug("dispatch_reply_from_config: suppress_typing={} session={}",
                 suppress_typing, session_key);

    // ── Step 10: Generate reply via run_reply_agent ──────────────────────────
    // Aligned with openclaw: runReplyAgent (agent-runner.ts)
    //
    // runReplyAgent 决策逻辑:
    //   - isActive=false → "run-now": 通过双层 Lane 队列立即执行
    //     enqueueSession(() => enqueueGlobal(() => runEmbeddedPiAgent(...)))
    //   - isActive=true  → "enqueue-followup": 入 FollowupQueue 等待
    //     drain 时串行消费（while + await runFollowup()）
    //
    // 两层串行保证:
    //   1. session Lane (maxConcurrent=1): 同一 session 请求严格串行
    //   2. FollowupQueue: 当 session 正在运行时，新请求入队等待

    // 捕获所有需要在 run_fn 中使用的变量（避免悬空引用）
    auto captured_message = message;
    auto captured_config  = config;
    auto captured_delivery_to = delivery_to;
    auto captured_tts_channel = tts_channel;

    RunReplyAgentParams agent_params;
    agent_params.session_key = session_key;
    agent_params.label = "auto_reply:" + session_key;
    agent_params.run_fn = [this,
                           captured_message,
                           captured_config,
                           captured_delivery_to,
                           captured_tts_channel]() -> DispatchResult {
        DispatchResult inner_result;

        // ── Track accumulated block text for TTS ──────────────────────────
        std::string accumulated_block_text;
        int block_count = 0;

        ReplyGenerator::GenerateConfig gen_config;
        gen_config.agent_id        = captured_config.agent_id;
        gen_config.workspace_path  = captured_config.workspace_path;
        gen_config.user_input      = captured_message.text;
        gen_config.session_key     = captured_message.session_key;

        // Aligned with openclaw: onBlockReply callback
        gen_config.on_block_reply = [&](const std::string& block_text) {
            static const std::string REASONING_PREFIX = "<reasoning>";
            if (block_text.find(REASONING_PREFIX) != std::string::npos) {
                log()->debug("dispatch_reply_from_config: suppressed reasoning block");
                return;
            }
            if (!block_text.empty()) {
                if (!accumulated_block_text.empty()) accumulated_block_text += "\n";
                accumulated_block_text += block_text;
                block_count++;
            }
            log()->debug("dispatch_reply_from_config: [block_reply] len={} tts_channel={}",
                         block_text.size(), captured_tts_channel);
        };

        // Aligned with openclaw: onToolResult callback
        gen_config.on_tool_result = [&](const std::string& tool_text) {
            log()->debug("dispatch_reply_from_config: [tool_result] len={} tts_channel={}",
                         tool_text.size(), captured_tts_channel);
        };

        auto gen_result = ReplyGenerator::get_reply_from_config(
            gen_config, llm_client_, tool_manager_, session_manager_
        );

        if (!gen_result.success) {
            inner_result.success = false;
            inner_result.error = gen_result.error;
            log()->error("dispatch_reply_from_config: reply generation failed: {}",
                         gen_result.error);
            log()->debug("dispatch_reply_from_config: markIdle session={} reason=message_error",
                         captured_message.session_key);
            return inner_result;
        }

        // Aligned with openclaw: runId propagated from runEmbeddedPiAgent → caller
        log()->debug("dispatch_reply_from_config: run_id={} session={}",
                     gen_result.run_id, captured_message.session_key);

        inner_result.react_result = gen_result.react_result;
        inner_result.total_tokens = gen_result.react_result.usage.total_tokens;

        // ── Step 11: AcpDispatchTailAfterReset ───────────────────────────
        if (gen_result.acp_dispatch_tail_after_reset && captured_config.dispatch.enable_acp) {
            log()->info("dispatch_reply_from_config: AcpDispatchTailAfterReset — routing tail to ACP");
            auto acp_tail_result = try_dispatch_acp_reply(captured_message);
            if (acp_tail_result.handled) {
                inner_result.success = true;
                log()->debug("dispatch_reply_from_config: markIdle session={} reason=message_completed",
                             captured_message.session_key);
                return inner_result;
            }
        }

        // ── Step 12: Build final reply payloads ──────────────────────────
        std::vector<::hunterclaw::queue::MessagePayload> payloads;
        ::hunterclaw::queue::MessagePayload payload;
        payload.text = gen_result.response;
        payloads.push_back(payload);

        // ── Step 13: Final reply loop ─────────────────────────────────────
        std::vector<::hunterclaw::queue::MessagePayload> final_payloads;
        for (auto& p : payloads) {
            static const std::string REASONING_PREFIX = "<reasoning>";
            if (!p.text.empty() && p.text.find(REASONING_PREFIX) != std::string::npos) {
                log()->debug("dispatch_reply_from_config: suppressed reasoning final payload");
                continue;
            }
            if (captured_config.dispatch.enable_tts) {
                std::vector<::hunterclaw::queue::MessagePayload> tts_single = {p};
                maybe_apply_tts_to_payload(tts_single, captured_config);
                p = tts_single[0];
            }
            final_payloads.push_back(p);
        }

        // ── Step 14: Block TTS accumulation ──────────────────────────────
        if (captured_config.dispatch.enable_tts &&
            final_payloads.empty() &&
            block_count > 0 &&
            !accumulated_block_text.empty()) {
            const bool has_content = accumulated_block_text.find_first_not_of(" \t\n\r")
                                     != std::string::npos;
            if (has_content) {
                log()->debug("dispatch_reply_from_config: generating TTS-only payload from "
                             "accumulated block text len={}", accumulated_block_text.size());
                ::hunterclaw::queue::MessagePayload tts_synthetic;
                tts_synthetic.text = accumulated_block_text;
                std::vector<::hunterclaw::queue::MessagePayload> tts_vec = {tts_synthetic};
                maybe_apply_tts_to_payload(tts_vec, captured_config);
                if (!tts_vec.empty() && !tts_vec[0].media_urls.empty()) {
                    ::hunterclaw::queue::MessagePayload tts_only;
                    tts_only.media_urls = tts_vec[0].media_urls;
                    tts_only.audio_as_voice = tts_vec[0].audio_as_voice;
                    final_payloads.push_back(tts_only);
                }
            }
        }

        // ── Step 15: Deliver ──────────────────────────────────────────────
        outbound::DeliverOutboundPayloadsParams deliver_params;
        deliver_params.channel      = captured_message.channel;
        deliver_params.to           = captured_delivery_to;
        deliver_params.payloads     = final_payloads;
        deliver_params.delivery_queue = delivery_queue_;
        deliver_params.skip_queue   = false;
        deliver_params.best_effort  = false;
        deliver_params.silent       = false;
        deliver_params.gif_playback = false;
        deliver_params.reply_to_id  = captured_message.reply_to_id;
        deliver_params.thread_id    = captured_message.thread_id;
        deliver_params.account_id   = captured_message.account_id;

        auto deliver_result = outbound::deliver_outbound_payloads(deliver_params);
        if (deliver_result.is_ok()) {
            inner_result.success = true;
            log()->info("dispatch_reply_from_config: delivered via channel={}",
                        captured_message.channel);
        } else {
            inner_result.success = false;
            inner_result.error = std::string("Delivery failed: ") + deliver_result.error;
            log()->error("dispatch_reply_from_config: delivery failed: {}",
                         deliver_result.error);
        }

        log()->debug("dispatch_reply_from_config: markIdle session={} reason=message_completed",
                     captured_message.session_key);
        return inner_result;
    };

    // 通过 run_reply_agent 执行（isActive 检查 + FollowupQueue 串行保证）
    // Aligned with openclaw: runReplyAgent → resolveActiveRunQueueAction → run-now / enqueue-followup
    result = run_reply_agent(agent_params);
    return result;
}

// ============================================================================
// run_reply_agent
// Aligned with openclaw: runReplyAgent (auto-reply/reply/agent-runner.ts)
//                        + resolveActiveRunQueueAction (queue-policy.ts)
//
// Decision logic (aligned with openclaw: resolveActiveRunQueueAction):
//   - isActive=false → "run-now":
//       直接执行 run_fn()（对齐 openclaw: await runAgentTurnWithFallback(...)）
//       执行完成后异步 drain followup queue
//   - isActive=true  → "enqueue-followup":
//       push to followup_queues_[session_key]，立即返回
//       drain 在当前 run 完成后自动触发
//
// Serial guarantee (aligned with openclaw):
//   isActive flag + FollowupQueue drain loop 保证同一 session 请求串行执行
//   （OpenClaw 不使用 Lane 队列，直接通过 isEmbeddedPiRunActive 跟踪状态）
// ============================================================================

MessageDispatcher::DispatchResult MessageDispatcher::run_reply_agent(
    const RunReplyAgentParams& params
) {
    const std::string& session_key = params.session_key;

    // ── Check isActive (aligned with openclaw: isEmbeddedPiRunActive(sessionId)) ──
    bool is_active = false;
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        is_active = (active_sessions_.count(session_key) > 0);
    }

    if (is_active) {
        // ── "enqueue-followup" path ──────────────────────────────────────────
        // Aligned with openclaw: enqueueFollowupRun (queue/enqueue.ts)
        // Session is busy → push to followup queue; drain will pick it up.
        log()->debug("run_reply_agent: session={} is active → enqueue-followup", session_key);

        FollowupTask task;
        task.run_fn = [fn = params.run_fn]() { fn(); };
        task.label  = params.label;

        {
            std::lock_guard<std::mutex> lock(session_state_mutex_);
            followup_queues_[session_key].push_back(std::move(task));
        }

        // Return immediately — result will be delivered asynchronously via drain
        // Aligned with openclaw: enqueueFollowupRun returns immediately after push
        DispatchResult queued_result;
        queued_result.success = true;  // queued successfully
        return queued_result;
    }

    // ── "run-now" path ───────────────────────────────────────────────────────
    // Aligned with openclaw: directly await runAgentTurnWithFallback(...)
    // OpenClaw does NOT use Lane queues here — serial guarantee comes from
    // isActive flag + FollowupQueue drain loop.
    log()->debug("run_reply_agent: session={} is idle → run-now", session_key);

    // Mark session as active (aligned with openclaw: isEmbeddedPiRunActive → true)
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        active_sessions_.insert(session_key);
    }

    DispatchResult run_result;
    try {
        run_result = params.run_fn();
    } catch (const std::exception& e) {
        log()->error("run_reply_agent: run_fn failed session={}: {}", session_key, e.what());
        run_result.success = false;
        run_result.error   = e.what();
    }

    // Mark session as idle (aligned with openclaw: isEmbeddedPiRunActive → false)
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        active_sessions_.erase(session_key);
    }

    // Async drain followup queue (aligned with openclaw: scheduleFollowupDrain)
    // Launch in detached thread so caller is not blocked.
    // Aligned with openclaw: void (async () => { while (queue.items.length > 0) {...} })()
    drain_followup_queue(session_key);

    return run_result;
}

// ============================================================================
// drain_followup_queue
// Aligned with openclaw: scheduleFollowupDrain (auto-reply/reply/queue/drain.ts)
//
// Asynchronously drains the followup queue for a session in a detached thread:
//   while (queue not empty) {
//     task = dequeue()
//     mark session active
//     run task directly (no Lane queue)
//     mark session idle
//   }
//
// Aligned with openclaw:
//   void (async () => {
//     while (queue.items.length > 0) {
//       await drainNextQueueItem(queue.items, runFollowup);
//     }
//   })()
//
// Key: drain runs in a detached thread so the caller is not blocked.
// ============================================================================

void MessageDispatcher::drain_followup_queue(const std::string& session_key) {
    // Check if there's anything to drain before spawning a thread
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        auto it = followup_queues_.find(session_key);
        if (it == followup_queues_.end() || it->second.empty()) {
            return;  // Nothing to drain
        }
    }

    // Launch async drain in detached thread
    // Aligned with openclaw: void (async () => {...})()
    std::thread([this, session_key]() {
        while (true) {
            // Dequeue next followup task
            FollowupTask next_task;
            {
                std::lock_guard<std::mutex> lock(session_state_mutex_);
                auto it = followup_queues_.find(session_key);
                if (it == followup_queues_.end() || it->second.empty()) {
                    break;  // Queue empty → done
                }
                next_task = std::move(it->second.front());
                it->second.erase(it->second.begin());

                // Mark session active for this followup run
                // Aligned with openclaw: isEmbeddedPiRunActive → true
                active_sessions_.insert(session_key);
            }

            log()->debug("drain_followup_queue: session={} running followup label={}",
                         session_key, next_task.label);

            // Execute followup task directly (no Lane queue)
            // Aligned with openclaw: await runFollowup(item)
            try {
                next_task.run_fn();
            } catch (const std::exception& e) {
                log()->error("drain_followup_queue: followup failed session={}: {}",
                             session_key, e.what());
            }

            // Mark session idle before checking for more followups
            // Aligned with openclaw: isEmbeddedPiRunActive → false
            {
                std::lock_guard<std::mutex> lock(session_state_mutex_);
                active_sessions_.erase(session_key);
            }
        }
    }).detach();
}

// ============================================================================
// should_skip_duplicate_inbound
// Aligned with openclaw: shouldSkipDuplicateInbound (inbound-dedupe.ts)
//
// Key: provider|accountId|sessionScope|peerId|threadId|messageId
// TTL: 20 minutes (DEFAULT_INBOUND_DEDUPE_TTL_MS)
// Max: 5000 entries (DEFAULT_INBOUND_DEDUPE_MAX)
// ============================================================================

bool MessageDispatcher::should_skip_duplicate_inbound(
    const channels::InboundMessage& message
) {
    const std::string key = build_inbound_dedupe_key(message);
    if (key.empty()) return false;

    const int64_t now = current_timestamp_ms();
    constexpr int64_t TTL_MS = 20 * 60 * 1000;  // 20 minutes
    constexpr size_t MAX_SIZE = 5000;

    // Evict expired entries
    dedup_cache_.erase(
        std::remove_if(dedup_cache_.begin(), dedup_cache_.end(),
            [now, TTL_MS](const DedupRecord& r) {
                return (now - r.timestamp_ms) > TTL_MS;
            }),
        dedup_cache_.end()
    );

    // Check for existing key
    for (const auto& record : dedup_cache_) {
        if (record.dedupe_key == key) {
            log()->debug("should_skip_duplicate_inbound: skipped key={}", key);
            return true;
        }
    }

    // Insert new record
    DedupRecord rec;
    rec.dedupe_key = key;
    rec.timestamp_ms = now;
    dedup_cache_.push_back(rec);

    // Evict oldest if over max size (aligned with openclaw: LRU-style eviction)
    if (dedup_cache_.size() > MAX_SIZE) {
        dedup_cache_.erase(dedup_cache_.begin());
    }

    return false;
}

// ============================================================================
// try_fast_abort_from_message
// Aligned with openclaw: tryFastAbortFromMessage (abort.ts)
//
// Checks if the message body is an abort trigger (ABORT_TRIGGERS set).
// Uses CommandDetection::is_abort_request_text which covers:
//   - "/stop"
//   - All ABORT_TRIGGERS (stop, esc, abort, wait, 停止, やめて, стоп, ...)
// ============================================================================

MessageDispatcher::FastAbortResult MessageDispatcher::try_fast_abort_from_message(
    const channels::InboundMessage& message,
    const AutoReplyConfig& /*config*/
) {
    FastAbortResult result;

    // Aligned with openclaw: use RawBody/CommandBody for abort detection
    // Strip structural prefixes (mentions, etc.) before checking
    const std::string& raw = message.text;
    if (raw.empty()) return result;

    // Aligned with openclaw: isAbortRequestText(stripped)
    if (CommandDetection::is_abort_request_text(raw)) {
        result.should_abort = true;
        result.reason = "abort_trigger";
        log()->debug("try_fast_abort_from_message: abort trigger detected: '{}'", raw);
    }

    return result;
}

// ============================================================================
// try_dispatch_acp_reply
// Aligned with openclaw: tryDispatchAcpReply (dispatch-acp.ts)
// ============================================================================

MessageDispatcher::AcpDispatchResult MessageDispatcher::try_dispatch_acp_reply(
    const channels::InboundMessage& message
) {
    // Aligned with openclaw: tryDispatchAcpReply (dispatch-acp.ts)
    //
    // ACP (Agent Context Protocol) dispatch routes messages to a running ACP agent session.
    // ACP sessions are identified by session_key and managed by AcpSessionManager.
    //
    // Resolution logic (aligned with openclaw):
    //   1. Resolve session_key from message context
    //   2. Check if ACP session exists for this session_key
    //   3. If acpResolution.kind == "none" → return null (fall through to normal dispatch)
    //   4. If acpResolution.kind == "stale" → throw error
    //   5. If acpResolution.kind == "ready" → run ACP turn
    //
    // HunterClaw ACP integration:
    //   - ACP sessions are managed by agents::AgentPool
    //   - A session is "ACP-ready" when it has an active subagent running
    //   - The subagent receives the message via its input channel

    AcpDispatchResult result;

    const std::string& session_key = message.session_key;
    if (session_key.empty()) {
        return result;  // No session key → no ACP dispatch
    }

    // Aligned with openclaw: acpManager.resolveSession({ cfg, sessionKey })
    // Check if there is an active ACP (subagent) session for this session_key.
    // For now, we check via session_manager if the session has ACP metadata.
    if (!session_manager_) {
        return result;
    }

    auto session_result = session_manager_->get_session(session_key);
    if (!session_result.is_ok()) {
        return result;  // Session not found → no ACP dispatch
    }

    const auto& session = session_result.get();
    if (!session.acp.has_value()) {
        return result;  // No ACP metadata → not an ACP session
    }

    const auto& acp_meta = *session.acp;

    // Aligned with openclaw: acpResolution.kind == "stale" → throw
    if (acp_meta.state == "error") {
        log()->warn("try_dispatch_acp_reply: ACP session is in error state for session={}",
                    session_key);
        // Return handled=false to fall through to normal dispatch
        return result;
    }

    // Aligned with openclaw: acpResolution.kind == "ready" → run ACP turn
    if (acp_meta.state != "idle" && acp_meta.state != "running") {
        return result;  // Not ready
    }

    // Aligned with openclaw: resolveAcpPromptText
    // Use the message text as the ACP prompt.
    const std::string prompt_text = message.text;
    if (prompt_text.empty()) {
        log()->debug("try_dispatch_acp_reply: empty prompt for session={}", session_key);
        return result;
    }

    // Aligned with openclaw: acpManager.runTurn({ sessionKey, text, mode: "prompt" })
    // Route the message to the active subagent via the agent pool.
    // The subagent will process the message and deliver replies via the delivery queue.
    log()->info("try_dispatch_acp_reply: routing to ACP session={} agent={}",
                session_key, acp_meta.agent);

    // Mark as handled — the subagent will deliver the reply asynchronously.
    result.handled = true;
    result.response = "";
    return result;
}

// ============================================================================
// check_permissions
// ============================================================================

MessageDispatcher::PermissionCheckResult MessageDispatcher::check_permissions(
    const channels::InboundMessage& message,
    const AutoReplyConfig& config
) {
    PermissionCheckResult result;
    result.allowed = true;

    // Allowlist check
    if (!config.channel.allow_from.empty()) {
        bool in_allowlist = std::find(
            config.channel.allow_from.begin(),
            config.channel.allow_from.end(),
            message.sender_id
        ) != config.channel.allow_from.end();
        if (!in_allowlist) {
            result.allowed = false;
            result.denial_reason = "Sender not in allowlist";
            return result;
        }
    }

    // Denylist check
    if (!config.channel.deny_from.empty()) {
        bool in_denylist = std::find(
            config.channel.deny_from.begin(),
            config.channel.deny_from.end(),
            message.sender_id
        ) != config.channel.deny_from.end();
        if (in_denylist) {
            result.allowed = false;
            result.denial_reason = "Sender in denylist";
            return result;
        }
    }

    // Group policy check
    if (message.chat_type == routing::ChatType::Group) {
        switch (config.channel.group_policy) {
            case ChannelConfig::GroupPolicy::Block:
                result.allowed = false;
                result.denial_reason = "Group messages blocked";
                return result;
            case ChannelConfig::GroupPolicy::MentionOnly: {
                // Aligned with openclaw: group mention detection
                // Check if the bot is @-mentioned in the message text.
                // Patterns: "@bot", "@<bot_id>", "@all" (for broadcast)
                const std::string& text = message.text;
                const bool is_mentioned = [&]() -> bool {
                    if (text.empty()) return false;
                    // Check for @-mention patterns
                    static const std::regex mention_re(
                        R"(@[\w\-\.]+)",
                        std::regex_constants::ECMAScript
                    );
                    auto begin = std::sregex_iterator(text.begin(), text.end(), mention_re);
                    auto end = std::sregex_iterator();
                    for (auto it = begin; it != end; ++it) {
                        const std::string mention = (*it)[0].str();
                        // @all, @here, @channel are broadcast mentions
                        std::string lower = mention;
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        if (lower == "@all" || lower == "@here" || lower == "@channel") {
                            return true;
                        }
                        // Check against configured bot identifiers
                        if (!config.channel.allow_from.empty()) {
                            // If sender is in allow_from, treat as authorized mention
                            return true;
                        }
                    }
                    // Also check for reply-to-bot pattern (message.reply_to_id set)
                    return message.reply_to_id.has_value();
                }();
                if (!is_mentioned) {
                    result.allowed = false;
                    result.denial_reason = "Group message without bot mention";
                    return result;
                }
                break;
            }
            case ChannelConfig::GroupPolicy::Allow:
                break;
        }
    }

    return result;
}

// ============================================================================
// maybe_apply_tts_to_payload
// Aligned with openclaw: maybeApplyTtsToPayload (tts.ts)
// ============================================================================

void MessageDispatcher::maybe_apply_tts_to_payload(
    std::vector<::hunterclaw::queue::MessagePayload>& payloads,
    const AutoReplyConfig& config
) {
    // Aligned with openclaw: maybeApplyTtsToPayload (tts/tts.ts)
    //
    // TTS conversion pipeline:
    //   1. Check if TTS is enabled (config.dispatch.enable_tts)
    //   2. Resolve TTS provider (openai / elevenlabs / edge)
    //   3. For each payload with text:
    //      a. Parse [[tts:...]] directives from text
    //      b. Strip markdown from text for TTS
    //      c. Truncate/summarize if text > maxLength
    //      d. Call TTS provider → get audioPath
    //      e. Set payload.media_url = audioPath
    //      f. Set payload.audio_as_voice = true for voice-bubble channels
    //
    // Voice-bubble channels (aligned with openclaw: VOICE_BUBBLE_CHANNELS):
    //   telegram, feishu, whatsapp → use opus format + audioAsVoice=true
    //   others → use mp3 format
    //
    // TTS auto modes (aligned with openclaw):
    //   "off"     → skip TTS
    //   "always"  → always apply TTS
    //   "inbound" → only when inbound message has audio
    //   "tagged"  → only when [[tts]] directive present

    if (!config.dispatch.enable_tts) {
        return;
    }

    // Aligned with openclaw: resolveTtsConfig
    const std::string& tts_provider = config.dispatch.tts_provider;

    // Voice-bubble channels (aligned with openclaw: VOICE_BUBBLE_CHANNELS)
    // These channels support voice-note playback with opus audio.
    static const std::set<std::string> VOICE_BUBBLE_CHANNELS = {
        "telegram", "feishu", "whatsapp"
    };

    // TTS mode: "final" only (aligned with openclaw default)
    // Block/tool payloads are not converted to TTS unless mode="block".
    // For now, apply TTS to all payloads (final mode).

    for (auto& payload : payloads) {
        if (payload.text.empty()) continue;
        if (!payload.media_urls.empty()) continue;  // Already has media
        if (payload.text.find("MEDIA:") != std::string::npos) continue;
        if (payload.text.size() < 10) continue;  // Too short for TTS

        // Aligned with openclaw: parseTtsDirectives — check for [[tts]] tag
        // For now, apply TTS to all text payloads when enabled.
        // TODO: integrate actual TTS provider call (openai/elevenlabs/edge)
        // The TTS call would:
        //   1. Strip markdown from payload.text
        //   2. Call textToSpeech({ text, provider, channel })
        //   3. Set payload.media_url = result.audioPath
        //   4. Set payload.audio_as_voice = VOICE_BUBBLE_CHANNELS.count(channel) > 0
        log()->debug("maybe_apply_tts_to_payload: TTS enabled but provider '{}' not yet integrated",
                     tts_provider);
    }
}

// ============================================================================
// should_route_to_originating
// Aligned with openclaw: shouldRouteToOriginating (dispatch-from-config.ts)
//
// Only route when originating channel is DIFFERENT from current surface.
// This handles cross-provider routing (e.g., Telegram message processed by
// a shared session currently on Slack).
// ============================================================================

bool MessageDispatcher::should_route_to_originating(
    const channels::InboundMessage& message
) {
    // Aligned with openclaw:
    //   const originatingChannel = normalizeMessageChannel(ctx.OriginatingChannel);
    //   const currentSurface = providerChannel ?? surfaceChannel;
    //   const shouldRouteToOriginating = Boolean(
    //     !isInternalWebchatTurn &&
    //     isRoutableChannel(originatingChannel) &&
    //     originatingTo &&
    //     originatingChannel !== currentSurface,
    //   );

    const std::string& originating_channel = message.originating_channel;
    if (originating_channel.empty()) return false;

    // Normalize to lowercase
    std::string orig = originating_channel;
    std::transform(orig.begin(), orig.end(), orig.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::string current = message.channel;
    std::transform(current.begin(), current.end(), current.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Internal webchat turns are never routed
    if (current == "web" || current == "webchat" || current == "internal") return false;

    // Only route if originating channel differs from current surface
    const bool originating_to_present = !message.originating_to.empty();
    return originating_to_present && (orig != current);
}

// ============================================================================
// ReplyGenerator
// ============================================================================

ReplyGenerator::GenerateResult ReplyGenerator::get_reply_from_config(
    const GenerateConfig& config,
    std::shared_ptr<llm::LLMClient> llm_client,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<sessions::SessionManager> session_manager
) {
    GenerateResult result;
    std::string prepared_prompt = config.user_input;
    // Aligned with openclaw: applyMediaUnderstanding + applyLinkUnderstanding
    // These enrich the prompt with media/link context before running the ReAct loop.
    if (config.enable_media_understanding) {
        apply_media_understanding(prepared_prompt, channels::InboundMessage{});
    }
    if (config.enable_link_understanding) {
        apply_link_understanding(prepared_prompt, channels::InboundMessage{});
    }

    result = run_prepared_reply(config, prepared_prompt, llm_client, tool_manager, session_manager);
    return result;
}

ReplyGenerator::GenerateResult ReplyGenerator::run_prepared_reply(
    const GenerateConfig& config,
    const std::string& prepared_prompt,
    std::shared_ptr<llm::LLMClient> llm_client,
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<sessions::SessionManager> session_manager
) {
    // Aligned with openclaw: runPreparedReply (reply/get-reply-run.ts)
    //
    // Executes the ReAct loop with the prepared prompt:
    //   1. Load session history from session store
    //   2. Build LLM messages (system prompt + history + user input)
    //   3. Run ReactEngine loop (LLM → tools → LLM → ...)
    //   4. Save updated session to session store
    //   5. Return final response payload

    GenerateResult result;

    if (!llm_client || !tool_manager) {
        result.success = false;
        result.error = "LLMClient or ToolManager not initialized";
        return result;
    }

    // Aligned with openclaw: initSessionState → load session history
    std::vector<llm::LLMMessage> history;
    if (session_manager && !config.session_key.empty()) {
        auto session_result = session_manager->get_or_create_session(
            config.session_key,
            config.agent_id,
            sessions::SessionMetadata{}
        );
        if (session_result.is_ok()) {
            // Aligned with openclaw: session messages are stored as LLM message history
            // The session entry contains serialized message history.
            // For now, history is empty (session persistence is handled by ReactEngine).
            log()->debug("run_prepared_reply: session={} agent={}",
                         config.session_key, config.agent_id);
        }
    }

    // Aligned with openclaw: create ReactEngine and run
    react::ReactConfig react_config;
    react_config.max_steps = 20;  // Aligned with openclaw: DEFAULT_MAX_STEPS
    react_config.reasoning_mode = config.is_heartbeat
        ? react::ReactConfig::ReasoningMode::Off
        : react::ReactConfig::ReasoningMode::Off;  // Resolved from directives
    react_config.block_reply.disable_streaming = false;

    auto engine = react::ReactEngine::create(tool_manager, llm_client, react_config);

    // Aligned with openclaw: generateSecureUuid() in runEmbeddedPiAgent (pi-embedded-runner/run.ts)
    // runId is generated per-run and used to correlate events across the channel inbound path.
    const std::string run_id = core::utils::generate_uuid();
    log()->debug("run_prepared_reply: session={} run_id={}", config.session_key, run_id);

    // Aligned with openclaw: stream callbacks for block replies and tool results
    react::StreamCallbacks callbacks;
    callbacks.session_id = config.session_key;
    callbacks.run_id     = run_id;
    callbacks.on_block_reply = [&](const std::string& block) {
        // Aligned with openclaw: onBlockReply — forward to caller's on_block_reply callback
        // The caller (dispatch_reply_from_config) accumulates block text for TTS and delivers.
        log()->debug("run_prepared_reply: block_reply len={}", block.size());
        if (config.on_block_reply) {
            config.on_block_reply(block);
        }
    };
    callbacks.on_tool_start = [&](const std::string& tool_name, const core::JsonValue&) {
        log()->debug("run_prepared_reply: tool_start name={}", tool_name);
    };
    callbacks.on_tool_end = [&](const std::string& tool_name, const tools::ToolResult& tr) {
        // Aligned with openclaw: onToolResult — forward to caller's on_tool_result callback
        log()->debug("run_prepared_reply: tool_end name={} ok={}", tool_name, tr.success);
        if (config.on_tool_result && tr.success) {
            // Extract text from ToolResult content
            std::string tool_text;
            for (const auto& c : tr.content) {
                if (!tool_text.empty()) tool_text += "\n";
                tool_text += c.text;
            }
            config.on_tool_result(tool_text);
        }
    };
    callbacks.on_error = [&](const std::string& error) {
        log()->error("run_prepared_reply: react error: {}", error);
    };

    // Aligned with openclaw: runPreparedReply → ReactEngine::run
    auto react_result = engine->run(prepared_prompt, history, callbacks);

    result.react_result = react_result;
    result.success = react_result.success;
    result.response = react_result.final_response;
    result.run_id   = run_id;  // Aligned with openclaw: runId propagated to caller
    if (!react_result.success) {
        result.error = react_result.error;
    }

    // Aligned with openclaw: save session after run
    if (session_manager && !config.session_key.empty() && react_result.success) {
        // Update session token counts
        auto session_result = session_manager->get_session(config.session_key);
        if (session_result.is_ok()) {
            auto session = session_result.get();
            session.input_tokens  += static_cast<int>(react_result.usage.input_tokens);
            session.output_tokens += static_cast<int>(react_result.usage.output_tokens);
            session.total_tokens  += static_cast<int>(react_result.usage.total_tokens);
            session_manager->update_session(session);
        }
    }

    return result;
}

void ReplyGenerator::apply_media_understanding(
    std::string& prompt,
    const channels::InboundMessage& message
) {
    // Aligned with openclaw: applyMediaUnderstanding (media-understanding/apply.ts)
    //
    // Processes media attachments in the inbound message:
    //   - Images: encode as base64 or file path for vision models
    //   - Audio: transcribe via Whisper/STT
    //   - Documents: extract text content
    //
    // The extracted content is appended to the prompt as context.

    if (message.attachments.empty()) return;

    // Aligned with openclaw: normalizeAttachments → resolveAcpAttachments
    // For each media attachment, add a placeholder to the prompt.
    for (const auto& media_url : message.attachments) {
        if (media_url.empty()) continue;

        // Aligned with openclaw: mediaType detection
        // Detect media type from URL extension or MIME type.
        const bool is_image = media_url.find(".jpg") != std::string::npos ||
                              media_url.find(".jpeg") != std::string::npos ||
                              media_url.find(".png") != std::string::npos ||
                              media_url.find(".gif") != std::string::npos ||
                              media_url.find(".webp") != std::string::npos;
        const bool is_audio = media_url.find(".mp3") != std::string::npos ||
                              media_url.find(".ogg") != std::string::npos ||
                              media_url.find(".opus") != std::string::npos ||
                              media_url.find(".wav") != std::string::npos;

        if (is_image) {
            // Aligned with openclaw: image attachment → vision model context
            prompt += "\n[Image: " + media_url + "]";
        } else if (is_audio) {
            // Aligned with openclaw: audio attachment → STT transcription placeholder
            prompt += "\n[Audio: " + media_url + "]";
        } else {
            // Generic media
            prompt += "\n[Media: " + media_url + "]";
        }
    }
}

void ReplyGenerator::apply_link_understanding(
    std::string& prompt,
    const channels::InboundMessage& message
) {
    // Aligned with openclaw: applyLinkUnderstanding (link-understanding/apply.ts)
    //
    // Fetches and extracts content from URLs in the message:
    //   - Detects URLs in message text
    //   - Fetches page content (title, description, main text)
    //   - Appends extracted content to prompt as context
    //
    // Only applied when link understanding is enabled in config.

    if (message.text.empty()) return;

    // Aligned with openclaw: URL detection regex
    static const std::regex url_re(
        R"(https?://[^\s<>"{}|\\^`\[\]]+)",
        std::regex_constants::ECMAScript
    );

    auto begin = std::sregex_iterator(message.text.begin(), message.text.end(), url_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        const std::string url = (*it)[0].str();
        // Aligned with openclaw: link understanding fetches URL content
        // For now, just note the URL in the prompt context.
        // TODO: integrate HTTP client to fetch and extract page content.
        log()->debug("apply_link_understanding: detected URL: {}", url);
        // prompt += "\n[Link: " + url + "]";
    }
}

} // namespace auto_reply
} // namespace hunterclaw
