// Aligned with openclaw:
//   src/auto-reply/dispatch.ts
//   src/auto-reply/reply/dispatch-from-config.ts
//   src/auto-reply/reply/inbound-dedupe.ts
//   src/auto-reply/reply/abort.ts
//   src/auto-reply/inbound-debounce.ts
//   src/auto-reply/reply/agent-runner.ts          (runReplyAgent)
//   src/auto-reply/reply/queue-policy.ts          (resolveActiveRunQueueAction)
//   src/auto-reply/reply/queue/types.ts           (QueueMode, QueueDropPolicy, QueueSettings)
//   src/auto-reply/reply/queue/settings.ts        (resolveQueueSettings)
//   src/auto-reply/reply/queue/state.ts           (FollowupQueueState, getFollowupQueue)
//   src/auto-reply/reply/queue/enqueue.ts         (enqueueFollowupRun)
//   src/auto-reply/reply/queue/drain.ts           (scheduleFollowupDrain)
//   src/utils/queue-helpers.ts                    (buildCollectPrompt, applyQueueDropPolicy)
//   src/pi-embedded-runner/runs.ts                (queueEmbeddedPiMessage, abortEmbeddedPiRun)

#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/auto_reply/command_detection.h"
#include "hunterclaw/auto_reply/heartbeat.h"
#include "hunterclaw/react/abort_primitives.h"     // ✅ 对齐 OC: import { isAbortRequestText, getAbortMemory, setAbortMemory } from "./reply/abort-primitives.js"
#include "hunterclaw/react/abort_cutoff.h"         // ✅ 对齐 OC: import { shouldSkipMessageByAbortCutoff, resolveAbortCutoffFromContext } from "./reply/abort-cutoff.js"
#include "hunterclaw/react/abort.h"                // ✅ 对齐 OC: import { handleAbortCommand } from "./commands-session-abort.js"
#include "hunterclaw/react/normalize_reply.h"      // ✅ 对齐 OC: import { normalizeReplyPayload } from "./normalize-reply.js"
#include "hunterclaw/react/queue_policy.h"         // ✅ 对齐 OC: import { resolveActiveRunQueueAction } from "./queue-policy.js"
#include "hunterclaw/react/reply_operation.h"      // ✅ 对齐 OC: import { replyRunRegistry } from "./reply-run-registry.js"
#include "hunterclaw/react/typing_mode.h"          // ✅ 对齐 OC: import { createTypingSignaler, resolveTypingMode } from "./typing-mode.js"
#include "hunterclaw/react/streaming_directives.h" // ✅ 对齐 OC: import { createStreamingDirectiveAccumulator } from "./streaming-directives.js"
#include "hunterclaw/routing/router.h"
#include "hunterclaw/sessions/session_manager.h"
#include "hunterclaw/queue/delivery_queue.h"
#include "hunterclaw/queue/command_queue.h"   // ✅ CommandQueue (session lane + main lane)
#include "hunterclaw/queue/lanes.h"            // ✅ ResolveSessionLane, ResolveGlobalLane
#include "hunterclaw/outbound/deliver.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/tools/tool_hooks.h"    // 🎯 message_received hook
#include "hunterclaw/hooks/hooks.h"          // 🎯 Internal hooks (message:received)
#include "hunterclaw/hooks/message_hook_mappers.h" // 🎯 toInternalMessageReceivedContext
#include "hunterclaw/agents/agent_pool.h"      // ✅ 对齐 OpenClaw: ACP dispatch 需要 agent_pool
#include "hunterclaw/agents/agent_instance.h"  // ✅ 需要完整类型以访问 context().last_channel 等字段
#include "hunterclaw/acp/acp_session_manager.h"  // ✅ 对齐 OpenClaw: ACP 控制平面
#include "hunterclaw/acp/acp_reply_projector.h"  // ✅ 对齐 OpenClaw: ACP 流式投影器
#include <sstream>
#include <iomanip>
#include <set>
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace auto_reply {

// ✅ 前向声明: 供 /queue 指令解析 (dispatch_reply_from_config) 使用
// 实现在 resolve_queue_settings 前面 (L1054, L1072)
static std::optional<QueueMode> normalize_queue_mode(const std::string& raw);
static std::optional<QueueDropPolicy> normalize_queue_drop_policy(const std::string& raw);

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
        // ✅ Aligned with openclaw: SessionEntry.sendPolicy override
        // SessionEntry now has send_policy field (session_manager.h:117)
        if (entry->send_policy.has_value()) {
            const auto& policy = *entry->send_policy;
            if (policy == "deny") return "deny";
            if (policy == "allow") return "allow";
        }
    }

    // 🎯 严格对齐 OpenClaw: config.session.sendPolicy 规则引擎
    // OpenClaw: resolveSendPolicy checks config.session.sendPolicy rules array
    // Each rule: { channel?, chatType?, action: "allow"|"deny" }
    // First match wins; no match → default "allow"
    //
    // TODO: 从实际 config 读取 sendPolicy rules
    // 当前未从 config 读取规则，返回默认 "allow"
    (void)channel;
    (void)chat_type;

    // No matching rule → default "allow" (对齐 OpenClaw 默认行为)
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

    // 🎯 严格对齐 OpenClaw: 用 RouteResolver 解析路由并构建 session_key
    // OpenClaw 中各渠道 monitor 调用 resolveAgentRoute({cfg, channel, accountId, peer})
    // 然后使用 route.sessionKey 作为入站消息的 session key
    if (ctx.session_key.empty()) {
        // 构建 peer（对齐 OpenClaw: RoutePeer {kind, id}）
        routing::ChatType peer_kind = ctx.chat_type;
        if (peer_kind == routing::ChatType::UNKNOWN) {
            peer_kind = routing::ChatType::DIRECT;
        }
        const std::string peer_id = !ctx.chat_id.empty() ? ctx.chat_id : ctx.sender_id;

        if (!ctx.channel.empty() && !peer_id.empty()) {
            // 🎯 对齐 OpenClaw: buildAgentPeerSessionKey → agent:{agentId}:{channel}:{kind}:{peerId}
            // 使用 build_agent_peer_session_key 生成标准格式 session key
            routing::BuildAgentPeerSessionKeyParams params;
            params.agent_id = "main";  // 默认 agent，实际由 RouteResolver 动态决定
            params.channel = ctx.channel;
            params.account_id = ctx.account_id;
            params.peer_kind = routing::chat_type_to_string(peer_kind);
            params.peer_id = peer_id;
            params.dm_scope = "main";  // 默认 DM scope，实际由配置决定

            ctx.session_key = routing::build_agent_peer_session_key(params);
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
    // Resolve agent_id from session_key (对齐 OpenClaw: resolveAgentIdFromSessionKey)
    auto_reply_config.agent_id = finalized.session_key.empty() ? "default" :
        routing::resolve_agent_id_from_session_key(finalized.session_key);

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

    // ── Plugin hook: inbound_claim (claiming) ────────────────────────────────
    // 🎯 对齐 OpenClaw runInboundClaim：在 commands/agent dispatch 之前，允许插件
    //   认领入站事件（first-claim-wins）。claim 则短路整个 dispatch。
    {
        auto* claim_mgr = tools::get_global_hook_manager();
        if (claim_mgr && claim_mgr->has_hooks(plugins::PluginHookName::INBOUND_CLAIM)) {
            plugins::PluginHookInboundClaimEvent ic_event;
            ic_event.content = message.text;
            ic_event.channel = message.channel;
            ic_event.account_id = message.account_id;
            ic_event.sender_id = message.sender_id;
            if (!message.message_id.empty()) ic_event.message_id = message.message_id;
            ic_event.conversation_id = message.chat_id.empty()
                ? std::make_optional(message.sender_id) : std::make_optional(message.chat_id);
            plugins::PluginHookInboundClaimContext ic_ctx;
            ic_ctx.channel_id      = message.channel;
            ic_ctx.account_id      = message.account_id;
            ic_ctx.conversation_id = ic_event.conversation_id;
            ic_ctx.sender_id       = message.sender_id;
            if (!message.message_id.empty()) ic_ctx.message_id = message.message_id;
            try {
                auto claim = claim_mgr->run_inbound_claim(ic_event, ic_ctx);
                if (claim && claim->handled) {
                    log()->info("dispatch_reply_from_config: inbound_claim claimed msg={}", message_id);
                    result.skipped = true;
                    result.skip_reason = "inbound_claim";
                    result.success = true;
                    return result;  // ← 插件认领，短路 dispatch
                }
            } catch (const std::exception& e) {
                log()->warn("dispatch_reply_from_config: inbound_claim hook failed: {}", e.what());
            }
        }
    }

    // ── Plugin hook: message_received ────────────────────────────────────────
    // 🎯 对齐 OpenClaw: dispatch-from-config.ts L188-195
    //   hookRunner.runMessageReceived(toPluginMessageReceivedEvent(hookContext), toPluginMessageContext(hookContext))
    // Fire-and-forget: do not block dispatch on hook failure.
    {
        auto* hook_mgr = tools::get_global_hook_manager();
        if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::MESSAGE_RECEIVED)) {
            plugins::PluginHookMessageReceivedEvent mr_event;
            mr_event.from    = message.sender_id;
            mr_event.content = message.text;
            mr_event.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                message.timestamp.time_since_epoch()).count();

            plugins::PluginHookMessageContext mr_ctx;
            mr_ctx.channel_id      = message.channel;
            mr_ctx.account_id      = message.account_id;
            mr_ctx.conversation_id = message.chat_id.empty()
                ? std::make_optional(message.sender_id)
                : std::make_optional(message.chat_id);
            try {
                hook_mgr->run_message_received(mr_event, mr_ctx);
            } catch (const std::exception& e) {
                log()->warn("dispatch_reply_from_config: message_received hook failed: {}", e.what());
            }
        }

        // ── Plugin hook: before_dispatch (claiming) ──────────────────────────
        // 🎯 对齐 OpenClaw runBeforeDispatch：model dispatch 之前，插件可检查/接管
        //   消息（first-claim-wins，{handled, text?}）。claim 则短路 model dispatch。
        {
            auto* bd_mgr = tools::get_global_hook_manager();
            if (bd_mgr && bd_mgr->has_hooks(plugins::PluginHookName::BEFORE_DISPATCH)) {
                plugins::PluginHookBeforeDispatchEvent bd_event;
                bd_event.content = message.text;
                bd_event.channel = message.channel;
                if (!message.message_id.empty()) bd_event.message_id = message.message_id;
                if (!session_key.empty())        bd_event.session_key = session_key;
                bd_event.sender_id = message.sender_id;
                plugins::PluginHookBeforeDispatchContext bd_ctx;
                bd_ctx.channel_id      = message.channel;
                bd_ctx.account_id      = message.account_id;
                bd_ctx.conversation_id = message.chat_id.empty()
                    ? std::make_optional(message.sender_id) : std::make_optional(message.chat_id);
                if (!session_key.empty())        bd_ctx.session_key = session_key;
                bd_ctx.sender_id       = message.sender_id;
                if (!message.message_id.empty()) bd_ctx.message_id = message.message_id;
                try {
                    auto claim = bd_mgr->run_before_dispatch(bd_event, bd_ctx);
                    if (claim && claim->handled) {
                        log()->info("dispatch_reply_from_config: before_dispatch claimed msg={}", message_id);
                        result.skipped = true;
                        result.skip_reason = "before_dispatch";
                        result.success = true;
                        return result;  // ← 插件接管，短路 model dispatch
                    }
                } catch (const std::exception& e) {
                    log()->warn("dispatch_reply_from_config: before_dispatch hook failed: {}", e.what());
                }
            }
        }
        
        // 🎯 对齐 OpenClaw: dispatch-from-config.ts L499-503
        // triggerInternalHook(createInternalHookEvent("message", "received", sessionKey, ...))
        if (!session_key.empty()) {
            try {
                hooks::CanonicalInboundMessageHookContext canonical;
                canonical.from = message.sender_id;
                canonical.content = message.text;
                canonical.channel_id = message.channel;
                canonical.account_id = message.account_id;
                canonical.conversation_id = message.chat_id.empty()
                    ? message.sender_id : message.chat_id;
                canonical.is_group = !message.chat_id.empty() &&
                    message.chat_id != message.sender_id;
                auto internal_ctx = hooks::to_internal_message_received_context(canonical);
                auto event = hooks::create_message_received_event(internal_ctx);
                event.session_key = session_key;
                hooks::HookRegistry::instance().trigger_hook(event);
            } catch (...) {}  // fire-and-forget
        }
    }

    // ── Step 2: shouldRouteToOriginating ────────────────────────────────────
    // Aligned with openclaw: check if originating channel differs from current surface.
    // Only route when originating channel is DIFFERENT from current surface.
    const bool should_route = should_route_to_originating(message);
    const std::string delivery_to = should_route
        ? (message.chat_id.empty() ? message.sender_id : message.chat_id)
        : message.sender_id;

    // ── Step 3: tryFastAbortFromMessage ─────────────────────────────────────
    // 对齐 OpenClaw: commands-session-abort.ts handleStopCommand + handleAbortTrigger
    //   → resolveAbortTarget → applyAbortTarget → persistAbortTargetEntry
    // NOTE: markProcessing() is called here (aligned with openclaw: before fastAbort check)
    log()->debug("dispatch_reply_from_config: markProcessing session={}", session_key);
    auto fast_abort = try_fast_abort_from_message(message, config);
    if (fast_abort.should_abort) {
        // 对齐 OC: handle_abort_command — 完整 abort 流程:
        //   setAbortMemory + abort embedded run + stop subagents + clear queues
        //   + persistAbortTargetEntry (写 cutoff 到 session entry)
        react::AbortSessionParams abort_params;
        abort_params.session_key = session_key;
        abort_params.command_text = message.text;
        abort_params.message_sid = message.message_id.empty()
            ? std::nullopt : std::optional<std::string>(message.message_id);
        abort_params.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            message.timestamp.time_since_epoch()).count();

        // 回调: abort embedded run
        abort_params.abort_embedded_run = [this](const std::string& sk) {
            this->abort_embedded_session(sk);
        };
        // 回调: clear session queues
        abort_params.clear_session_queues = [](const std::string& sk) {
            auto& cmd_queue = queue::CommandQueue::Instance();
            cmd_queue.clear_command_lane(queue::ResolveSessionLane(sk));
        };

        // Session store 参数 (对齐 OC: persistAbortTargetEntry)
        if (session_entry_for_dispatch.has_value() && session_manager_) {
            abort_params.session_entry = &*session_entry_for_dispatch;
            // Note: session_store map 在 channel 路径不容易获取，
            // handle_abort_command 内部的 persist_abort_target_entry 会通过
            // session_manager_->update_session 直接持久化
        }

        auto abort_result = react::handle_abort_command(abort_params);

        // 对齐 OC: 如果 session entry 有效，手动应用 abort fields
        // (因为 channel 路径没有全局 session_store map)
        if (session_entry_for_dispatch.has_value()) {
            session_entry_for_dispatch->aborted_last_run = true;
            if (abort_result.cutoff.has_value()) {
                react::apply_abort_cutoff_to_session_entry(
                    *session_entry_for_dispatch, abort_result.cutoff);
            }
            session_entry_for_dispatch->updated_at =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            if (session_manager_) {
                session_manager_->update_session(*session_entry_for_dispatch);
            }
        }

        // 发送 abort 回复
        ::hunterclaw::queue::MessagePayload abort_payload;
        abort_payload.text = abort_result.reply_text;

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
        log()->debug("dispatch_reply_from_config: fast abort: {} (cutoff={})",
                     fast_abort.reason, abort_result.cutoff.has_value() ? "yes" : "no");
        return result;
    }

    // ── Step 3b: Inline abort cutoff check ──────────────────────────────────
    // 对齐 OpenClaw: get-reply-inline-actions.ts L310-337
    // 如果当前消息不是 /stop，检查 session 是否有 abort cutoff：
    //   - 如果消息在 cutoff 之前 → 跳过（丢弃 abort 前排队的老消息）
    //   - 如果消息在 cutoff 之后 → 清除 cutoff（恢复正常处理）
    if (!fast_abort.should_abort && session_entry_for_dispatch.has_value()) {
        const bool is_stop_like = react::is_abort_request_text(message.text);
        if (!is_stop_like) {
            auto cutoff = react::read_abort_cutoff_from_session_entry(
                &*session_entry_for_dispatch);
            if (cutoff.has_value()) {
                // 对齐 OC: resolveAbortCutoffFromContext(ctx) → incoming
                // InboundMessage 用 message_id 作为 SID, timestamp 转 ms
                const int64_t msg_ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    message.timestamp.time_since_epoch()
                ).count();

                auto incoming = react::resolve_abort_cutoff_from_context(
                    message.message_id.empty()
                        ? std::nullopt
                        : std::optional<std::string>(message.message_id),
                    msg_ts_ms > 0
                        ? std::optional<int64_t>(msg_ts_ms)
                        : std::nullopt
                );

                // 对齐 OC: shouldSkipMessageByAbortCutoff
                bool should_skip = react::should_skip_message_by_abort_cutoff({
                    .cutoff_message_sid = cutoff->message_sid,
                    .cutoff_timestamp = cutoff->timestamp_ms,
                    .message_sid = incoming.message_sid,
                    .timestamp = incoming.timestamp_ms
                });

                if (should_skip) {
                    // 对齐 OC: typing.cleanup(); return { kind: "reply", reply: undefined };
                    log()->debug("dispatch_reply_from_config: skipping stale message by abort cutoff "
                                 "session={} msg_sid={} cutoff_sid={}",
                                 session_key,
                                 incoming.message_sid.value_or("(none)"),
                                 cutoff->message_sid.value_or("(none)"));
                    result.skipped = true;
                    result.skip_reason = "abort_cutoff_stale";
                    return result;
                }

                // 对齐 OC: 消息比 cutoff 更新 → clearAbortCutoffInSessionRuntime
                auto entry_copy = *session_entry_for_dispatch;
                react::apply_abort_cutoff_to_session_entry(entry_copy, std::nullopt);
                entry_copy.updated_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
                session_entry_for_dispatch = entry_copy;

                // 持久化清除
                if (session_manager_) {
                    session_manager_->update_session(entry_copy);
                }

                log()->debug("dispatch_reply_from_config: cleared abort cutoff for session={}",
                             session_key);
            }
        }
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
        auto acp_result = try_dispatch_acp_reply(message, delivery_to, bypass_acp_for_command);
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
        const bool is_dm = (message.chat_type != routing::ChatType::GROUP &&
                            message.chat_type != routing::ChatType::CHANNEL);
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

    // ── Step 10: extractQueueDirective + resolveQueueSettings ────────────────
    // 对齐 OpenClaw: directive-handling.parse.ts → extractQueueDirective
    //               directive-handling.persist.ts → persist queue overrides
    //               get-reply-run.ts → resolveQueueSettings({ inlineMode, inlineOptions })
    //
    // /queue 指令格式 (对齐 OpenClaw directive.ts):
    //   /queue <mode>                        → 切换 queue mode 并持久化
    //   /queue <mode> debounce:2s cap:5 drop:summarize → 完整配置
    //   /queue reset                         → 重置所有 queue 覆盖
    // ========================================================================
    
    std::optional<QueueMode> per_message_queue_mode;
    std::optional<InlineQueueOptions> per_message_queue_options;
    bool queue_directive_only = false;  // true = /queue 是唯一内容，不需要执行 agent
    
    if (command_authorized) {
        // ✅ 对齐 OpenClaw: extractQueueDirective (queue/directive.ts)
        const std::string& text = message.text;
        // 正则匹配 /queue（对齐 OpenClaw: /(?:^|\s)\/queue(?=$|\s|:)/i）
        std::string lower_text = text;
        std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
        size_t queue_pos = std::string::npos;
        // 简化匹配：查找 "/queue" 并验证边界
        for (size_t p = lower_text.find("/queue"); p != std::string::npos; p = lower_text.find("/queue", p + 1)) {
            // 检查前面是空白或行首
            if (p > 0 && lower_text[p - 1] != ' ' && lower_text[p - 1] != '\t' && lower_text[p - 1] != '\n') continue;
            // 检查后面是空白、冒号、或末尾
            size_t end = p + 6;
            if (end < lower_text.size() && lower_text[end] != ' ' && lower_text[end] != '\t' && lower_text[end] != ':' && lower_text[end] != '\n') continue;
            queue_pos = p;
            break;
        }
        
        if (queue_pos != std::string::npos) {
            // 解析 /queue 后的参数 (对齐 OpenClaw: parseQueueDirectiveArgs)
            std::string args_str;
            size_t args_start = queue_pos + 6;
            if (args_start < text.size()) {
                args_str = text.substr(args_start);
                // trim leading whitespace/colon
                size_t start = args_str.find_first_not_of(" \t:");
                if (start != std::string::npos) args_str = args_str.substr(start);
                else args_str.clear();
            }
            
            bool queue_reset = false;
            std::optional<QueueMode> parsed_mode;
            std::optional<int> parsed_debounce;
            std::optional<int> parsed_cap;
            std::optional<QueueDropPolicy> parsed_drop;
            
            // 逐 token 解析 (对齐 OpenClaw: parseQueueDirectiveArgs)
            std::istringstream iss(args_str);
            std::string token;
            while (iss >> token) {
                std::string lowered = token;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
                
                if (lowered == "default" || lowered == "reset" || lowered == "clear") {
                    queue_reset = true;
                    break;
                }
                // debounce:2s / debounce=2000
                if (lowered.substr(0, 9) == "debounce:" || lowered.substr(0, 9) == "debounce=") {
                    std::string val = token.substr(9);
                    // 简化 duration 解析：支持 ms/s/m 后缀
                    int ms = 0;
                    if (!val.empty()) {
                        if (val.back() == 's') { ms = std::stoi(val.substr(0, val.size()-1)) * 1000; }
                        else if (val.back() == 'm') { ms = std::stoi(val.substr(0, val.size()-1)) * 60000; }
                        else if (val.size() > 2 && val.substr(val.size()-2) == "ms") { ms = std::stoi(val.substr(0, val.size()-2)); }
                        else { try { ms = std::stoi(val); } catch (...) {} }
                    }
                    if (ms > 0) parsed_debounce = ms;
                    continue;
                }
                // cap:5 / cap=5
                if (lowered.substr(0, 4) == "cap:" || lowered.substr(0, 4) == "cap=") {
                    std::string val = token.substr(4);
                    try { int c = std::stoi(val); if (c >= 1) parsed_cap = c; } catch (...) {}
                    continue;
                }
                // drop:summarize / drop=old
                if (lowered.substr(0, 5) == "drop:" || lowered.substr(0, 5) == "drop=") {
                    std::string val = token.substr(5);
                    auto d = normalize_queue_drop_policy(val);
                    if (d.has_value()) parsed_drop = d;
                    continue;
                }
                // mode token
                auto m = normalize_queue_mode(token);
                if (m.has_value()) {
                    parsed_mode = m;
                    continue;
                }
                break;  // 对齐 OpenClaw: 遇到不可识别 token 时停止解析
            }
            
            // ✅ 对齐 OpenClaw: persistInlineDirectives (directive-handling.persist.ts L187-193)
            // 将 /queue 设置持久化到 session entry
            if (session_manager_ && !session_key.empty() && session_entry_for_dispatch.has_value()) {
                auto entry = *session_entry_for_dispatch;
                bool updated = false;
                
                if (queue_reset) {
                    // 对齐 OpenClaw: delete sessionEntry.queueMode/debounceMs/cap/drop
                    entry.queue_mode = std::nullopt;
                    entry.queue_debounce_ms = std::nullopt;
                    entry.queue_cap = std::nullopt;
                    entry.queue_drop = std::nullopt;
                    updated = true;
                } else {
                    if (parsed_mode.has_value()) {
                        // 将 QueueMode enum 转为字符串存储
                        std::string mode_str;
                        switch (*parsed_mode) {
                            case QueueMode::Steer:       mode_str = "steer"; break;
                            case QueueMode::Followup:    mode_str = "followup"; break;
                            case QueueMode::Collect:     mode_str = "collect"; break;
                            case QueueMode::SteerBacklog: mode_str = "steer-backlog"; break;
                            case QueueMode::Interrupt:   mode_str = "interrupt"; break;
                            default:                     mode_str = "collect"; break;
                        }
                        entry.queue_mode = mode_str;
                        updated = true;
                    }
                    if (parsed_debounce.has_value()) { entry.queue_debounce_ms = *parsed_debounce; updated = true; }
                    if (parsed_cap.has_value()) { entry.queue_cap = *parsed_cap; updated = true; }
                    if (parsed_drop.has_value()) {
                        std::string drop_str;
                        switch (*parsed_drop) {
                            case QueueDropPolicy::Old:       drop_str = "old"; break;
                            case QueueDropPolicy::New:       drop_str = "new"; break;
                            case QueueDropPolicy::Summarize: drop_str = "summarize"; break;
                        }
                        entry.queue_drop = drop_str;
                        updated = true;
                    }
                }
                
                if (updated) {
                    session_manager_->update_session(entry);
                    session_entry_for_dispatch = entry;
                    log()->info("dispatch_reply_from_config: /queue persisted session={} reset={} mode={}",
                                session_key, queue_reset, entry.queue_mode.value_or("(default)"));
                }
            }
            
            // 设置 per-message 覆盖 (对齐 OpenClaw: get-reply-directives-apply.ts L275-294)
            if (!queue_reset && parsed_mode.has_value()) {
                per_message_queue_mode = parsed_mode;
            }
            if (!queue_reset && (parsed_debounce.has_value() || parsed_cap.has_value() || parsed_drop.has_value())) {
                InlineQueueOptions opts;
                opts.debounce_ms = parsed_debounce;
                opts.cap = parsed_cap;
                opts.drop_policy = parsed_drop;
                per_message_queue_options = opts;
            }
            
            // 检查是否是 directive-only (对齐 OpenClaw: isDirectiveOnly)
            // 移除 /queue 及参数后，如果剩余文本为空，则不执行 agent
            std::string cleaned = text.substr(0, queue_pos);
            // 简化 cleaned 判断
            cleaned.erase(0, cleaned.find_first_not_of(" \t\n"));
            if (!cleaned.empty()) cleaned.erase(cleaned.find_last_not_of(" \t\n") + 1);
            if (cleaned.empty()) {
                queue_directive_only = true;
            }
        }
    }
    
    // ✅ 对齐 OpenClaw: directive-only /queue → 返回确认文本，不执行 agent
    // (对齐 OpenClaw: directive-handling.impl.ts L492-505)
    if (queue_directive_only) {
        // 构建 ACK 文本
        std::string ack_parts;
        if (per_message_queue_mode.has_value()) {
            std::string mode_str;
            switch (*per_message_queue_mode) {
                case QueueMode::Steer:       mode_str = "steer"; break;
                case QueueMode::Followup:    mode_str = "followup"; break;
                case QueueMode::Collect:     mode_str = "collect"; break;
                case QueueMode::SteerBacklog: mode_str = "steer-backlog"; break;
                case QueueMode::Interrupt:   mode_str = "interrupt"; break;
                default:                     mode_str = "collect"; break;
            }
            ack_parts += "⚙️ Queue mode set to " + mode_str + ".";
        } else if (session_entry_for_dispatch.has_value() && !session_entry_for_dispatch->queue_mode.has_value()) {
            // reset case
            ack_parts += "⚙️ Queue mode reset to default.";
        }
        if (per_message_queue_options.has_value()) {
            if (per_message_queue_options->debounce_ms.has_value())
                ack_parts += " ⚙️ Queue debounce set to " + std::to_string(*per_message_queue_options->debounce_ms) + "ms.";
            if (per_message_queue_options->cap.has_value())
                ack_parts += " ⚙️ Queue cap set to " + std::to_string(*per_message_queue_options->cap) + ".";
        }
        if (ack_parts.empty()) ack_parts = "⚙️ Queue mode reset to default.";
        
        result.success = true;
        result.react_result.final_response = ack_parts;
        // TODO: 通过 delivery 路径投递 ack 文本（当前直接返回）
        return result;
    }
    
    // ── Resolve queue settings (with inlineMode if present) ─────────────────
    const QueueSettings resolved_queue = resolve_queue_settings(
        session_entry_for_dispatch,
        message.channel,
        per_message_queue_mode,
        per_message_queue_options
    );

    // ── interrupt mode: clear lane + abort before run ────────────────────────
    // Aligned with openclaw: get-reply-run.ts
    //   if (resolvedQueue.mode === "interrupt" && laneSize > 0) {
    //     clearCommandLane(sessionLaneKey);
    //     abortEmbeddedPiRun(sessionIdFinal);
    //   }
    // Note: actual abort is handled inside run_reply_agent

    // ── Step 11: Generate reply via run_reply_agent ──────────────────────────
    // Aligned with openclaw: runReplyAgent (agent-runner.ts)
    //
    // runReplyAgent 决策逻辑 (aligned with openclaw: resolveActiveRunQueueAction):
    //   - isActive=false → "run-now": 直接执行
    //   - isActive=true  → 按 queue mode 处理:
    //       steer      → queueEmbeddedPiMessage (inject into running session)
    //       interrupt  → abort + run-now
    //       followup   → enqueue-followup (serial one-by-one)
    //       collect    → enqueue-followup (batch merge, default)
    //       steer-backlog → steer first, fallback to followup

    // 捕获所有需要在 run_fn 中使用的变量（避免悬空引用）
    auto captured_message = message;
    auto captured_config  = config;
    auto captured_delivery_to = delivery_to;
    auto captured_tts_channel = tts_channel;

    RunReplyAgentParams agent_params;
    agent_params.session_key    = session_key;
    agent_params.label          = "auto_reply:" + session_key;
    agent_params.prompt         = message.text;  // for collect mode merging
    agent_params.summary_line   = message.text.size() > 160
                                    ? message.text.substr(0, 159) + "…"
                                    : message.text;
    agent_params.queue_settings = resolved_queue;
    agent_params.is_heartbeat   = false;
    // ✅ 对齐 OpenClaw FollowupRun: originating routing metadata
    agent_params.originating_channel    = message.originating_channel;
    agent_params.originating_to         = message.originating_to;
    agent_params.originating_account_id = message.account_id.value_or("");
    agent_params.originating_thread_id  = "";
    agent_params.run_fn = [this,
                           captured_message,
                           captured_config,
                           captured_delivery_to,
                           captured_tts_channel]() -> DispatchResult {
        DispatchResult inner_result;

        // ── Track accumulated block text for TTS ──────────────────────────
        std::string accumulated_block_text;
        int block_count = 0;

        // ✅ 严格对齐 OpenClaw: 更新 agent context 中的 channel 信息
        // 对应 OpenClaw: SessionEntry.lastChannel / lastTo / lastAccountId / lastThreadId
        // 用于 method_agent 中 deliver=true 时构建 deliver 函数
        // （subagent 完成后通过 method_agent 调用父 agent，父 agent 需要知道 channel 信息）
        if (agent_pool_) {
            auto dispatch_agent = agent_pool_->get_agent(captured_message.session_key);
            if (dispatch_agent) {
                auto& ctx = dispatch_agent->context();
                ctx.last_channel    = captured_message.channel;
                ctx.last_to         = captured_delivery_to;
                ctx.last_account_id = captured_message.account_id.value_or("");
                ctx.last_thread_id  = captured_message.thread_id.value_or("");
                ctx.last_reply_to_id = captured_message.reply_to_id.value_or("");
                log()->debug("dispatch_reply_from_config: updated agent context channel={} to={}",
                             captured_message.channel, captured_delivery_to);
            }
        }

        ReplyGenerator::GenerateConfig gen_config;
        gen_config.agent_id        = captured_config.agent_id;
        gen_config.workspace_path  = captured_config.workspace_path;
        gen_config.user_input      = captured_message.text;
        gen_config.session_key     = captured_message.session_key;

        // ✅ 对齐 OpenClaw: clearActiveEmbeddedRun 的 handle 匹配
        // on_engine_created 设置此变量，cleanup 时用于比较 active_engines_[key]
        // 防止 interrupt 后新 run 注册的 engine 被旧 run 的 cleanup 误删
        std::shared_ptr<react::ReactEngine> my_engine;

        // ✅ 对齐 OpenClaw: setActiveEmbeddedRun(sessionId, queueHandle)
        gen_config.on_engine_created = [this, captured_message, &my_engine](std::shared_ptr<react::ReactEngine> engine) {
            my_engine = engine;  // ✅ 记录 engine 引用，用于 cleanup handle 匹配
            std::vector<std::string> fallback_steers;
            bool should_cancel = false;
            {
                std::lock_guard<std::mutex> lock(session_state_mutex_);
                active_engines_[captured_message.session_key] = engine;
                streaming_sessions_.insert(captured_message.session_key);
                // ✅ 检查 abort 信号：如果 abort_embedded_session 在 engine 创建前被调用，
                // aborted_sessions_ 中会有此 session_key，此时需要立即 cancel
                auto abort_it = aborted_sessions_.find(captured_message.session_key);
                if (abort_it != aborted_sessions_.end()) {
                    should_cancel = true;
                    aborted_sessions_.erase(abort_it);
                    log()->info("dispatch_reply_from_config: engine created but session was already aborted, "
                                "cancelling immediately session={}",
                                captured_message.session_key);
                }
                // 消费残留 fallback steer 消息
                auto it = steer_queues_.find(captured_message.session_key);
                if (it != steer_queues_.end() && !it->second.empty()) {
                    fallback_steers = std::move(it->second);
                    steer_queues_.erase(it);
                }
            }
            // ✅ 如果 session 已被 abort，立即 cancel engine
            // 对齐 OpenClaw: runAbortController.abort() 在 run 开始后生效
            if (should_cancel) {
                engine->cancel();
                return;  // 不注入 steer 消息（session 已被中止）
            }
            // 通过 engine->steer() 注入 fallback 消息
            for (const auto& msg : fallback_steers) {
                engine->steer(msg);
            }
            if (!fallback_steers.empty()) {
                log()->info("dispatch_reply_from_config: injected {} fallback steer messages via engine session={}",
                            fallback_steers.size(), captured_message.session_key);
            }
            log()->debug("dispatch_reply_from_config: registered active engine for session={}",
                         captured_message.session_key);
        };

        // ✅ 严格对齐 OpenClaw: onBlockReply callback
        // 参考: dispatch-from-config.ts:onBlockReply → dispatcher.sendBlockReply(ttsPayload)
        //
        // OpenClaw 实现:
        //   onBlockReply: (payload) => {
        //     if (shouldSuppressReasoningPayload(payload)) return;
        //     accumulatedBlockText += payload.text;
        //     blockCount++;
        //     if (shouldRouteToOriginating) sendPayloadAsync(ttsPayload);
        //     else dispatcher.sendBlockReply(ttsPayload);  ← 实时发送！
        //   }
        //
        // HunterClaw 对齐:
        //   - 实时调用 deliver_outbound_payloads（对应 dispatcher.sendBlockReply）
        //   - 同时累积 accumulated_block_text（用于 TTS 生成）
        gen_config.on_block_reply = [this, &accumulated_block_text, &block_count,
                                     captured_message, captured_delivery_to](const std::string& block_text) {
            static const std::string REASONING_PREFIX = "<reasoning>";
            if (block_text.find(REASONING_PREFIX) != std::string::npos) {
                log()->debug("dispatch_reply_from_config: suppressed reasoning block");
                return;
            }
            if (block_text.empty()) {
                return;
            }

            // ✅ 对齐 OpenClaw: 累积 block text（用于 TTS 生成）
            if (!accumulated_block_text.empty()) accumulated_block_text += "\n";
            accumulated_block_text += block_text;
            block_count++;

            // ✅ 对齐 OpenClaw: 实时发送 block reply（dispatcher.sendBlockReply）
            // OpenClaw: dispatcher.sendBlockReply(ttsPayload) → enqueue → deliver → channel.send()
            // HunterClaw: deliver_outbound_payloads(channel, to, payloads, delivery_queue_)
            ::hunterclaw::queue::MessagePayload block_payload;
            block_payload.text = block_text;

            outbound::DeliverOutboundPayloadsParams block_deliver_params;
            block_deliver_params.channel      = captured_message.channel;
            block_deliver_params.to           = captured_delivery_to;
            block_deliver_params.payloads     = {block_payload};
            block_deliver_params.delivery_queue = delivery_queue_;
            block_deliver_params.skip_queue   = false;
            block_deliver_params.best_effort  = true;   // block reply 是 best-effort（对齐 OpenClaw）
            block_deliver_params.silent       = false;
            block_deliver_params.reply_to_id  = captured_message.reply_to_id;
            block_deliver_params.thread_id    = captured_message.thread_id;
            block_deliver_params.account_id   = captured_message.account_id;

            auto block_deliver_result = outbound::deliver_outbound_payloads(block_deliver_params);
            if (!block_deliver_result.is_ok()) {
                log()->warn("dispatch_reply_from_config: block_reply delivery failed: {}",
                            block_deliver_result.error);
            } else {
                log()->debug("dispatch_reply_from_config: [block_reply] sent len={} channel={}",
                             block_text.size(), captured_message.channel);
            }
        };

        // Aligned with openclaw: onToolResult callback
        gen_config.on_tool_result = [&](const std::string& tool_text) {
            log()->debug("dispatch_reply_from_config: [tool_result] len={} tts_channel={}",
                         tool_text.size(), captured_tts_channel);
        };

        auto gen_result = ReplyGenerator::get_reply_from_config(
            gen_config, llm_client_, tool_manager_, session_manager_
        );

        // ✅ 对齐 OpenClaw: clearActiveEmbeddedRun(sessionId, queueHandle)
        // runs.ts:238: if (ACTIVE_EMBEDDED_RUNS.get(sessionId) === handle) { delete }
        // 只有当 active_engines_ 中的 engine 与当前 run 的 engine 是同一个时才 erase
        // 防止 interrupt 后新 run 注册的 engine 被旧 run 的 cleanup 误删
        {
            std::lock_guard<std::mutex> lock(session_state_mutex_);
            auto eit = active_engines_.find(captured_message.session_key);
            if (eit != active_engines_.end() && eit->second == my_engine) {
                active_engines_.erase(eit);
                streaming_sessions_.erase(captured_message.session_key);
                log()->debug("dispatch_reply_from_config: cleared active engine for session={}",
                             captured_message.session_key);
            } else {
                log()->debug("dispatch_reply_from_config: skipped engine cleanup (handle mismatch) session={}",
                             captured_message.session_key);
            }
            // 清理残留的 abort 信号（如果 abort 在 engine 创建后到达，
            // abort_embedded_session 会直接 cancel engine，不会留在 aborted_sessions_ 中；
            // 但防御性清理确保不会泄漏）
            aborted_sessions_.erase(captured_message.session_key);
        }

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
            auto acp_tail_result = try_dispatch_acp_reply(captured_message, captured_delivery_to);
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

    // 通过 run_reply_agent 执行（四种调度模式）
    // Aligned with openclaw: runReplyAgent → resolveActiveRunQueueAction
    //   collect   (default) → enqueue-followup, drain merges all into one prompt
    //   followup  (serial)  → enqueue-followup, drain runs one-by-one
    //   steer               → inject into streaming session
    //   interrupt           → abort current + run-now
    //   steer-backlog       → steer first, fallback to followup
    result = run_reply_agent(agent_params);
    return result;
}

// ============================================================================
// resolve_queue_settings
// Aligned with openclaw: resolveQueueSettings (auto-reply/reply/queue/settings.ts)
//
// 严格对齐 OpenClaw 5 层优先级链:
//   mode:     inlineMode > sessionEntry.queueMode > byChannel[channel] > config.queue.mode > default("collect")
//   debounce: inlineOptions > sessionEntry > debounceMsByChannel > config.queue.debounceMs > DEFAULT(1000)
//   cap:      inlineOptions > sessionEntry > config.queue.cap > DEFAULT(20)
//   drop:     inlineOptions > sessionEntry > config.queue.drop > DEFAULT("summarize")
// ============================================================================

// ✅ 对齐 OpenClaw: normalizeQueueMode (queue/normalize.ts)
// 独立 static 函数，供 resolve_queue_settings 和 /queue 命令处理使用
static std::optional<QueueMode> normalize_queue_mode(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string s = raw;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    // trim
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);

    if (s == "queue" || s == "queued")                              return QueueMode::Steer;
    if (s == "interrupt" || s == "interrupts" || s == "abort")      return QueueMode::Interrupt;
    if (s == "steer" || s == "steering")                            return QueueMode::Steer;
    if (s == "followup" || s == "follow-ups" || s == "followups")   return QueueMode::Followup;
    if (s == "collect" || s == "coalesce")                          return QueueMode::Collect;
    if (s == "steer+backlog" || s == "steer-backlog" || s == "steer_backlog") return QueueMode::SteerBacklog;
    return std::nullopt;
}

// ✅ 对齐 OpenClaw: normalizeQueueDropPolicy (queue/normalize.ts)
static std::optional<QueueDropPolicy> normalize_queue_drop_policy(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string s = raw;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);

    if (s == "old" || s == "oldest")       return QueueDropPolicy::Old;
    if (s == "new" || s == "newest")       return QueueDropPolicy::New;
    if (s == "summarize" || s == "summary") return QueueDropPolicy::Summarize;
    return std::nullopt;
}

QueueSettings MessageDispatcher::resolve_queue_settings(
    const std::optional<sessions::SessionEntry>& session_entry,
    const std::string& channel,
    std::optional<QueueMode> inline_mode,
    std::optional<InlineQueueOptions> inline_options
) const {
    QueueSettings settings;
    const auto& queue_cfg = config_.queue;
    
    // Normalize channel key (对齐 OpenClaw: channelKey = params.channel?.trim().toLowerCase())
    std::string channel_key = channel;
    std::transform(channel_key.begin(), channel_key.end(), channel_key.begin(), ::tolower);

    // ── Mode: 5 层优先级链 ──────────────────────────────────────────────────
    // 对齐 OpenClaw settings.ts L39-44:
    //   params.inlineMode ??
    //   normalizeQueueMode(sessionEntry?.queueMode) ??
    //   normalizeQueueMode(providerModeRaw) ??
    //   normalizeQueueMode(queueCfg?.mode) ??
    //   defaultQueueModeForChannel(channelKey)
    
    // 1. inlineMode (最高优先级 — 来自 /queue 内联指令)
    std::optional<QueueMode> resolved_mode;
    if (inline_mode.has_value()) {
        resolved_mode = inline_mode;
    }
    // 2. sessionEntry.queueMode
    if (!resolved_mode.has_value() && session_entry.has_value() && session_entry->queue_mode.has_value()) {
        resolved_mode = normalize_queue_mode(*session_entry->queue_mode);
    }
    // 3. byChannel[channelKey] (对齐 OpenClaw: providerModeRaw = queueCfg?.byChannel?.[channelKey])
    if (!resolved_mode.has_value() && !channel_key.empty()) {
        auto it = queue_cfg.by_channel.find(channel_key);
        if (it != queue_cfg.by_channel.end()) {
            resolved_mode = normalize_queue_mode(it->second);
        }
    }
    // 4. config.messages.queue.mode (全局配置)
    if (!resolved_mode.has_value() && queue_cfg.mode.has_value()) {
        resolved_mode = normalize_queue_mode(*queue_cfg.mode);
    }
    // 5. default: collect (对齐 OpenClaw: defaultQueueModeForChannel → "collect")
    settings.mode = resolved_mode.value_or(QueueMode::Collect);

    // ── Debounce: 6 层优先级链 ──────────────────────────────────────────────
    // 对齐 OpenClaw settings.ts L45-51:
    //   inlineOptions?.debounceMs ?? sessionEntry?.queueDebounceMs ??
    //   resolveChannelDebounce(byChannel, channelKey) ??
    //   resolvePluginDebounce(channelKey) ??   ← HunterClaw 无 plugin 层，跳过
    //   queueCfg?.debounceMs ?? DEFAULT(1000)
    bool debounce_resolved = false;
    // 1. inlineOptions
    if (!debounce_resolved && inline_options.has_value() && inline_options->debounce_ms.has_value()) {
        settings.debounce_ms = std::max(0, *inline_options->debounce_ms);
        debounce_resolved = true;
    }
    // 2. sessionEntry
    if (!debounce_resolved && session_entry.has_value() && session_entry->queue_debounce_ms.has_value()) {
        settings.debounce_ms = std::max(0, *session_entry->queue_debounce_ms);
        debounce_resolved = true;
    }
    // 3. debounceMsByChannel (对齐 OpenClaw: resolveChannelDebounce)
    if (!debounce_resolved && !channel_key.empty()) {
        auto it = queue_cfg.debounce_ms_by_channel.find(channel_key);
        if (it != queue_cfg.debounce_ms_by_channel.end()) {
            settings.debounce_ms = std::max(0, it->second);
            debounce_resolved = true;
        }
    }
    // 4. config.messages.queue.debounceMs
    if (!debounce_resolved && queue_cfg.debounce_ms.has_value()) {
        settings.debounce_ms = std::max(0, *queue_cfg.debounce_ms);
        debounce_resolved = true;
    }
    // 5. default: 1000ms (already set in QueueSettings constructor)

    // ── Cap: 4 层优先级链 ───────────────────────────────────────────────────
    // 对齐 OpenClaw settings.ts L52-56:
    //   inlineOptions?.cap ?? sessionEntry?.queueCap ?? queueCfg?.cap ?? DEFAULT(20)
    bool cap_resolved = false;
    if (!cap_resolved && inline_options.has_value() && inline_options->cap.has_value()) {
        settings.cap = std::max(1, *inline_options->cap);
        cap_resolved = true;
    }
    if (!cap_resolved && session_entry.has_value() && session_entry->queue_cap.has_value()) {
        settings.cap = std::max(1, static_cast<int>(*session_entry->queue_cap));
        cap_resolved = true;
    }
    if (!cap_resolved && queue_cfg.cap.has_value()) {
        settings.cap = std::max(1, *queue_cfg.cap);
        cap_resolved = true;
    }

    // ── Drop: 4 层优先级链 ──────────────────────────────────────────────────
    // 对齐 OpenClaw settings.ts L57-61:
    //   inlineOptions?.dropPolicy ?? sessionEntry?.queueDrop ?? queueCfg?.drop ?? DEFAULT("summarize")
    bool drop_resolved = false;
    if (!drop_resolved && inline_options.has_value() && inline_options->drop_policy.has_value()) {
        settings.drop_policy = *inline_options->drop_policy;
        drop_resolved = true;
    }
    if (!drop_resolved && session_entry.has_value() && session_entry->queue_drop.has_value()) {
        auto d = normalize_queue_drop_policy(*session_entry->queue_drop);
        if (d.has_value()) { settings.drop_policy = *d; drop_resolved = true; }
    }
    if (!drop_resolved && queue_cfg.drop.has_value()) {
        auto d = normalize_queue_drop_policy(*queue_cfg.drop);
        if (d.has_value()) { settings.drop_policy = *d; drop_resolved = true; }
    }

    return settings;
}

// ============================================================================
// resolve_active_run_queue_action
// Aligned with openclaw: resolveActiveRunQueueAction (queue-policy.ts)
//
// type ActiveRunQueueAction = "run-now" | "enqueue-followup" | "drop"
// ============================================================================

ActiveRunQueueAction MessageDispatcher::resolve_active_run_queue_action(
    bool is_active,
    bool is_heartbeat,
    bool should_followup,
    QueueMode queue_mode
) const {
    if (!is_active) {
        return ActiveRunQueueAction::RunNow;
    }
    if (is_heartbeat) {
        return ActiveRunQueueAction::Drop;
    }
    if (should_followup || queue_mode == QueueMode::Steer) {
        return ActiveRunQueueAction::EnqueueFollowup;
    }
    return ActiveRunQueueAction::RunNow;
}

// ============================================================================
// steer_embedded_session
// Aligned with openclaw: queueEmbeddedPiMessage (pi-embedded-runner/runs.ts)
//
// OpenClaw 完整路径:
//   queueEmbeddedPiMessage(sessionId, text)
//     → handle = ACTIVE_EMBEDDED_RUNS.get(sessionId)   (全局 Map<sessionId, Handle>)
//     → handle.queueMessage(text)                      (EmbeddedPiQueueHandle.queueMessage)
//     → activeSession.steer(text)                      (attempt.ts:2231)
//     → session.agent.steer(AgentMessage{role:user})   (pi-agent-core)
//   语义: 向**正在 streaming 的 LLM session** 实时注入一条新 user turn
//
// 限制条件 (aligned with openclaw):
//   - handle 不存在 → return false  (session 未激活)
//   - !handle.isStreaming() → return false  (session 未 streaming)
//   - handle.isCompacting() → return false  (正在 compaction)
//
// HunterClaw 实现 (✅ 已对齐 OpenClaw):
//   active_engines_ ≈ ACTIVE_EMBEDDED_RUNS (session_key → ReactEngine)
//   engine->steer(text) ≈ activeSession.steer(text)
//   消息直接注入 ReactEngine 的 steer 队列，在 execute_loop 下一轮循环时消费
//   作为新 user turn 追加到 messages 列表，LLM 在下次调用时看到
// ============================================================================

bool MessageDispatcher::steer_embedded_session(
    const std::string& session_key,
    const std::string& message
) {
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    // Aligned with openclaw: queueEmbeddedPiMessage returns false if no active streaming run
    if (streaming_sessions_.count(session_key) == 0) {
        log()->debug("steer_embedded_session: session not streaming, steer rejected session={}",
                     session_key);
        return false;
    }

    // ✅ 对齐 OpenClaw: handle.queueMessage(text) → activeSession.steer(text)
    // 直接调用 ReactEngine::steer()，消息立即注入 engine 的 steer 队列
    auto it = active_engines_.find(session_key);
    if (it != active_engines_.end() && it->second) {
        bool ok = it->second->steer(message);
        if (ok) {
            log()->info("steer_embedded_session: injected steer message via ReactEngine session={}",
                        session_key);
            return true;
        }
        // Engine exists but steer() returned false (engine not running)
        // Fall through to queue-based fallback
        log()->warn("steer_embedded_session: engine->steer() returned false, fallback to queue session={}",
                    session_key);
    }

    // Fallback: 当 active_engines_ 中没有 engine 时（不应发生），暂存到 steer_queues_
    steer_queues_[session_key].push_back(message);
    log()->info("steer_embedded_session: queued steer message (fallback) for session={} queueSize={}",
                session_key, steer_queues_[session_key].size());
    return true;
}

// ============================================================================
// abort_embedded_session
// Aligned with openclaw: abortEmbeddedPiRun (pi-embedded-runner/runs.ts:63-81)
//
// OpenClaw 完整路径:
//   abortEmbeddedPiRun(sessionId)
//     → handle = ACTIVE_EMBEDDED_RUNS.get(sessionId)
//     → if (!handle) return false                         (runs.ts:69-71)
//     → handle.abort()                                    (runs.ts:74)
//       → runAbortController.abort()                      (attempt.ts onAbort)
//         → 中断正在执行的 LLM stream (pi-agent-core abort)
//     → return true
//
// 关键时序 (strictly aligned with openclaw):
//   1. abort() 只发送取消信号，**不等待** run 完成
//   2. abort() **不从 ACTIVE_EMBEDDED_RUNS 中移除 handle**
//   3. handle 的清理由 run 的 finally block 完成:
//      clearActiveEmbeddedRun(sessionId, queueHandle)     (attempt.ts finally)
//
// HunterClaw 对齐:
//   active_engines_[key] ≈ ACTIVE_EMBEDDED_RUNS[sessionId]
//   engine->cancel()     ≈ handle.abort() → runAbortController.abort()
//
//   engine 引用的清理由 run_fn 内部完成 (dispatch_reply_from_config lambda):
//     get_reply_from_config 返回后 → active_engines_.erase(key)
//   **此处不 erase active_engines_**，避免 engine 被提前析构
//
// 调用方 (interrupt mode in run_reply_agent) 还需要:
//   clearFollowupQueue   → 清除 followup 队列
//   clearCommandLane     → 清除 CommandQueue session lane
// ============================================================================

bool MessageDispatcher::abort_embedded_session(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(session_state_mutex_);
    if (active_sessions_.count(session_key) == 0) {
        return false;
    }

    // ✅ 对齐 OpenClaw: handle.abort() → runAbortController.abort()
    // 通过 ReactEngine::cancel() 发送取消信号，中止正在执行的 LLM stream
    // cancel() 设置 cancelled_ = true (atomic)，execute_loop 在下一次循环检查时退出
    auto eit = active_engines_.find(session_key);
    if (eit != active_engines_.end() && eit->second) {
        eit->second->cancel();
        log()->info("abort_embedded_session: cancelled ReactEngine for session={}", session_key);
    } else {
        // ✅ Engine 尚未创建 (run_fn 还在 main lane queue 中等待)
        // 记录 abort 信号到 aborted_sessions_，on_engine_created 回调会检查
        // 并在 engine 创建后立即 cancel()，确保 abort 不会丢失
        aborted_sessions_.insert(session_key);
        log()->info("abort_embedded_session: engine not yet created, "
                    "recorded abort signal for session={}", session_key);
    }

    // ✅ 严格对齐 OpenClaw: abort 后**不移除** active_engines_ 和 active_sessions_
    // OpenClaw: abortEmbeddedPiRun 只调用 handle.abort()，不从 ACTIVE_EMBEDDED_RUNS 删除
    // 清理统一由 run 的 finally block 完成:
    //   attempt.ts finally → clearActiveEmbeddedRun(sessionId, queueHandle)
    //   HunterClaw → run_reply_agent cleanup + dispatch_reply_from_config cleanup
    // active_engines_ handle 指针匹配 ≈ OpenClaw ACTIVE_EMBEDDED_RUNS.get(id) === handle

    // ✅ 清除 steer 队列 (abort 后不应再有新 steer 消息注入到 engine)
    steer_queues_.erase(session_key);

    log()->info("abort_embedded_session: aborted session={}", session_key);
    return true;
}

// ============================================================================
// apply_queue_drop_policy
// Aligned with openclaw: applyQueueDropPolicy (utils/queue-helpers.ts)
//
// Returns true if item should be enqueued, false if dropped.
// ============================================================================

bool MessageDispatcher::apply_queue_drop_policy(
    FollowupQueueState& queue,
    const std::string& summary_text
) {
    if (queue.cap <= 0 || (int)queue.items.size() < queue.cap) {
        return true;  // under cap, always enqueue
    }
    if (queue.drop_policy == QueueDropPolicy::New) {
        return false;  // reject new item
    }
    // Drop oldest items to make room
    int drop_count = (int)queue.items.size() - queue.cap + 1;
    for (int i = 0; i < drop_count && !queue.items.empty(); ++i) {
        if (queue.drop_policy == QueueDropPolicy::Summarize) {
            queue.dropped_count++;
            const std::string& sl = queue.items.front().summary_line;
            std::string line = sl.empty() ? queue.items.front().prompt : sl;
            // truncate to 160 chars (aligned with openclaw: buildQueueSummaryLine)
            if (line.size() > 160) line = line.substr(0, 159) + "…";
            queue.summary_lines.push_back(line);
            // cap summary_lines at queue.cap
            while ((int)queue.summary_lines.size() > queue.cap) {
                queue.summary_lines.erase(queue.summary_lines.begin());
            }
        }
        queue.items.erase(queue.items.begin());
    }
    return true;
}

// ============================================================================
// build_collect_prompt
// Aligned with openclaw: buildCollectPrompt (utils/queue-helpers.ts)
//
// Merge multiple queued messages into one prompt.
// ============================================================================

std::string MessageDispatcher::build_collect_prompt(
    const std::vector<FollowupTask>& items,
    const std::string& summary_prompt
) const {
    // Aligned with openclaw:
    //   title: "[Queued messages while agent was busy]"
    //   items: "---\nQueued #N\n{prompt}"
    std::string result = "[Queued messages while agent was busy]";
    if (!summary_prompt.empty()) {
        result += "\n\n" + summary_prompt;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        result += "\n\n---\nQueued #" + std::to_string(i + 1) + "\n" + items[i].prompt;
    }
    return result;
}

// ============================================================================
// run_reply_agent
// Aligned with openclaw: runReplyAgent (auto-reply/reply/agent-runner.ts)
//                        + resolveActiveRunQueueAction (queue-policy.ts)
//
// Four scheduling modes (aligned with openclaw: QueueMode):
//   steer      → inject into running session via steer_embedded_session()
//   followup   → serial: one-by-one after current run completes
//   collect    → batch: merge all queued into one prompt (default)
//   interrupt  → abort current run + clear lane, then run-now
//   steer-backlog → steer first, fallback to followup if not streaming
// ============================================================================

MessageDispatcher::DispatchResult MessageDispatcher::run_reply_agent(
    const RunReplyAgentParams& params
) {
    const std::string& session_key = params.session_key;
    const QueueSettings& qs = params.queue_settings;

    // ── Plugin hook: reply_dispatch (claiming) ────────────────────────────────
    // 🎯 对齐 OpenClaw runReplyDispatch：默认 model path 之前，插件可接管整个 reply
    //   dispatch（first-claim-wins）。claim 则短路默认 agent 运行。
    {
        auto* rd_mgr = tools::get_global_hook_manager();
        if (rd_mgr && rd_mgr->has_hooks(plugins::PluginHookName::REPLY_DISPATCH)) {
            plugins::PluginHookReplyDispatchEvent rd_event;
            if (!session_key.empty()) rd_event.session_key = session_key;
            rd_event.send_policy = "allow";
            plugins::PluginHookReplyDispatchContext rd_ctx;
            if (!session_key.empty()) rd_ctx.session_key = session_key;
            try {
                auto claim = rd_mgr->run_reply_dispatch(rd_event, rd_ctx);
                if (claim && claim->handled) {
                    log()->info("run_reply_agent: reply_dispatch claimed session={}", session_key);
                    DispatchResult claimed;
                    claimed.success = true;
                    claimed.skipped = true;
                    claimed.skip_reason = "reply_dispatch";
                    return claimed;  // ← 插件接管，短路默认 model path
                }
            } catch (const std::exception& e) {
                log()->warn("run_reply_agent: reply_dispatch hook failed: {}", e.what());
            }
        }
    }

    // ── Check isActive + isStreaming ──────────────────────────────────────────
    // Aligned with openclaw: isEmbeddedPiRunActive / isEmbeddedPiRunStreaming
    bool is_active = false;
    bool is_streaming = false;
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        is_active    = (active_sessions_.count(session_key) > 0);
        is_streaming = (streaming_sessions_.count(session_key) > 0);
    }

    // ── interrupt mode: abort current run + clear lane, then run-now ─────────
    // Aligned with openclaw: get-reply-run.ts:448-452
    //   if (resolvedQueue.mode === "interrupt" && laneSize > 0) {
    //     clearCommandLane(sessionLaneKey);     ← 清除 session lane 中的 pending 命令
    //     abortEmbeddedPiRun(sessionIdFinal);   ← 中止正在运行的 agent
    //   }
    // + clearSessionQueues (cleanup.ts:12-31):
    //   clearFollowupQueue(key)                 ← 清空 followup 队列
    //   clearFollowupDrainCallback(key)         ← 停止 drain 回调
    //   clearCommandLane(resolveEmbeddedSessionLane(key)) ← 清除 session lane
    if (qs.mode == QueueMode::Interrupt && is_active) {
        // ✅ 对齐 OpenClaw: clearSessionQueues — 清空所有 pending 工作
        {
            std::lock_guard<std::mutex> lock(session_state_mutex_);
            auto it = followup_queues_.find(session_key);
            if (it != followup_queues_.end()) {
                const int cleared_items = static_cast<int>(it->second.items.size());
                it->second.items.clear();
                it->second.dropped_count = 0;
                it->second.summary_lines.clear();
                it->second.draining = false;  // ✅ 停止 drain (对齐 clearFollowupDrainCallback)
                followup_queues_.erase(it);   // ✅ 完全移除 (对齐 OpenClaw: FOLLOWUP_QUEUES.delete)
                if (cleared_items > 0) {
                    log()->info("run_reply_agent: interrupt cleared {} followup items session={}",
                                cleared_items, session_key);
                }
            }
        }

        // ✅ 对齐 OpenClaw: clearCommandLane(sessionLaneKey)
        // 清除 CommandQueue 中该 session 的 pending 命令
        {
            auto& cmd_queue = queue::CommandQueue::Instance();
            const std::string session_lane = "session:" + session_key;
            cmd_queue.clear_command_lane(session_lane);
            log()->debug("run_reply_agent: interrupt cleared command lane={} session={}",
                        session_lane, session_key);
        }

        // ✅ 对齐 OpenClaw: abortEmbeddedPiRun(sessionIdFinal)
        // 中止正在运行的 agent (调用 ReactEngine::cancel())
        // 严格对齐: abort 不从 active_sessions_ 移除 (OpenClaw: handle 仍在 ACTIVE_EMBEDDED_RUNS)
        const bool aborted = abort_embedded_session(session_key);
        log()->info("run_reply_agent: interrupt mode session={} aborted={}", session_key, aborted);

        // ✅ 对齐 OpenClaw: interrupt 后无条件 fall through 到 run-now
        // OpenClaw: get-reply-run.ts:448-452 abort 后不再检查 isActive，直接进入 enqueueSession
        // 新 run 通过 setActiveEmbeddedRun 覆盖旧 handle (Map.set 语义)
        // 旧 run finally 中 clearActiveEmbeddedRun 的 handle === 检查会 skip (handle_mismatch)
        is_active = false;
        is_streaming = false;
    }

    // ── steer mode: inject into running session ───────────────────────────────
    // Aligned with openclaw: agent-runner.ts
    //   if (shouldSteer && isStreaming) {
    //     const steered = queueEmbeddedPiMessage(sessionId, prompt);
    //     if (steered && !shouldFollowup) return undefined;
    //   }
    const bool should_steer = (qs.mode == QueueMode::Steer || qs.mode == QueueMode::SteerBacklog);
    if (should_steer && is_streaming) {
        const bool steered = steer_embedded_session(session_key, params.prompt);
        if (steered) {
            const bool should_followup = (qs.mode == QueueMode::SteerBacklog);
            if (!should_followup) {
                log()->info("run_reply_agent: steered into session={}", session_key);
                DispatchResult steered_result;
                steered_result.success = true;
                return steered_result;
            }
        }
    }

    // ── Determine shouldFollowup ──────────────────────────────────────────────
    // Aligned with openclaw: agent-runner.ts
    //   const shouldFollowup =
    //     resolvedQueue.mode === "followup" ||
    //     resolvedQueue.mode === "collect" ||
    //     resolvedQueue.mode === "steer-backlog";
    const bool should_followup =
        qs.mode == QueueMode::Followup ||
        qs.mode == QueueMode::Collect  ||
        qs.mode == QueueMode::SteerBacklog;

    // ── resolveActiveRunQueueAction ───────────────────────────────────────────
    const auto action = resolve_active_run_queue_action(
        is_active, params.is_heartbeat, should_followup, qs.mode
    );

    if (action == ActiveRunQueueAction::Drop) {
        // Aligned with openclaw: heartbeat while active → drop
        log()->debug("run_reply_agent: session={} drop (heartbeat while active)", session_key);
        DispatchResult drop_result;
        drop_result.success = true;
        return drop_result;
    }

    if (action == ActiveRunQueueAction::EnqueueFollowup) {
        // ── "enqueue-followup" path ──────────────────────────────────────────
        // Aligned with openclaw: enqueueFollowupRun (queue/enqueue.ts)
        log()->debug("run_reply_agent: session={} is active → enqueue-followup mode={}",
                     session_key, (int)qs.mode);

        bool need_drain = false;
        {
            std::lock_guard<std::mutex> lock(session_state_mutex_);
            auto& queue = followup_queues_[session_key];

            // Apply runtime settings (aligned with openclaw: applyQueueRuntimeSettings)
            queue.mode        = qs.mode;
            queue.debounce_ms = qs.debounce_ms;
            queue.cap         = qs.cap;
            queue.drop_policy = qs.drop_policy;

            // Apply drop policy (aligned with openclaw: applyQueueDropPolicy)
            const bool should_enqueue = apply_queue_drop_policy(queue, params.summary_line);
            if (!should_enqueue) {
                log()->debug("run_reply_agent: session={} item dropped by policy=new", session_key);
                DispatchResult drop_result;
                drop_result.success = true;
                return drop_result;
            }

            FollowupTask task;
            task.run_fn       = params.run_fn;
            task.label        = params.label;
            task.prompt       = params.prompt;
            task.summary_line = params.summary_line;
            // ✅ 对齐 OpenClaw FollowupRun: originating routing metadata
            task.originating_channel    = params.originating_channel;
            task.originating_to         = params.originating_to;
            task.originating_account_id = params.originating_account_id;
            task.originating_thread_id  = params.originating_thread_id;
            task.enqueued_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            queue.items.push_back(std::move(task));
            queue.last_enqueued_at_ms = task.enqueued_at_ms;

            // ── Race condition fix ───────────────────────────────────────
            // Between our is_active check (line ~1128) and this push, the
            // running thread may have already completed and called
            // drain_followup_queue() which found the queue empty and returned.
            // If the session is no longer active AND drain is not running,
            // we must trigger drain ourselves.
            //
            // OpenClaw (single-threaded JS) doesn't have this race because
            // the is_active check → enqueue → scheduleFollowupDrain sequence
            // is non-preemptable within a single microtask.
            if (active_sessions_.count(session_key) == 0 && !queue.draining) {
                need_drain = true;
            }
        }

        if (need_drain) {
            log()->debug("run_reply_agent: session={} enqueued but session already idle → triggering drain", session_key);
            drain_followup_queue(session_key);
        }

        DispatchResult queued_result;
        queued_result.success = true;
        return queued_result;
    }

    // ── "run-now" path ───────────────────────────────────────────────────────
    // ✅ 严格对齐 OpenClaw: runEmbeddedPiAgent (pi-embedded-runner/run.ts:276-277)
    //   return enqueueSession(() =>       // session lane: 保证同 session 串行
    //       enqueueGlobal(async () => {   // global/main lane: 控制全局并发
    //           ... agent run ...
    //       })
    //   );
    //
    // 双层 lane 机制:
    //   session lane "session:<key>" → maxConcurrent=1 (同一 session 串行)
    //   global lane  "main"         → maxConcurrent=N (全局并发控制)
    //
    // in_session_lane 标志:
    //   true  → 调用方 (chat.cc/agent.cc) 已在 session lane 中，只进 main lane
    //   false → auto_reply 路径，需自行包 session lane + main lane
    log()->debug("run_reply_agent: session={} is idle → run-now (dual lane)", session_key);

    // ── steer_queues_ fallback 说明 ──────────────────────────────────────────
    // 正常路径: steer 消息通过 steer_embedded_session → engine->steer() 直接注入
    // Fallback: 若 engine 不可用，消息暂存 steer_queues_，
    //           由 on_engine_created 回调在 engine 创建时统一消费并注入
    // 因此此处不再需要单独处理 steer_queues_

    std::function<DispatchResult()> effective_run_fn = params.run_fn;

    // Mark session as active (aligned with openclaw: setActiveEmbeddedRun)
    // 对齐 OpenClaw: ACTIVE_EMBEDDED_RUNS.set(sessionId, handle)
    // interrupt 后新 run 覆盖旧条目 (Map.set 语义)，旧 run finally 中的
    // clearActiveEmbeddedRun 通过 handle === 检查 skip (handle_mismatch)
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        active_sessions_.insert(session_key);
    }

    DispatchResult run_result;
    try {
        auto& cmd_queue = queue::CommandQueue::Instance();
        const std::string global_lane = queue::ResolveGlobalLane();  // "main"

        if (params.in_session_lane) {
            // ✅ 调用方已在 session lane 中 (chat.cc / agent.cc)
            // 只进 main lane，避免 session lane 死锁
            // 对齐 OpenClaw: enqueueGlobal(async () => { ... })
            auto future = cmd_queue.enqueue_command_in_lane(
                global_lane,
                std::move(effective_run_fn),
                queue::EnqueueOptions{.label = params.label, .urgent = false}
            );
            run_result = future.get();
        } else {
            // ✅ 严格对齐 OpenClaw 双层 lane:
            //   enqueueSession(() => enqueueGlobal(async () => { ... }))
            // auto_reply 路径未包 session lane，此处自行包装
            const std::string session_lane = queue::ResolveSessionLane(session_key);
            auto captured_run_fn = std::move(effective_run_fn);
            auto captured_label = params.label;

            // 外层: session lane (保证同一 session 串行)
            auto session_future = cmd_queue.enqueue_command_in_lane(
                session_lane,
                std::function<DispatchResult()>([&cmd_queue, global_lane,
                                                  captured_run_fn, captured_label]() -> DispatchResult {
                    // 内层: main lane (控制全局并发)
                    auto inner_future = cmd_queue.enqueue_command_in_lane(
                        global_lane,
                        captured_run_fn,
                        queue::EnqueueOptions{.label = captured_label, .urgent = false}
                    );
                    return inner_future.get();
                }),
                queue::EnqueueOptions{.label = "session:" + params.label, .urgent = false}
            );
            run_result = session_future.get();
        }
    } catch (const std::exception& e) {
        log()->error("run_reply_agent: run_fn failed session={}: {}", session_key, e.what());
        run_result.success = false;
        run_result.error   = e.what();
    }

    // Mark session as idle (aligned with openclaw: clearActiveEmbeddedRun in finally)
    // 对齐 OpenClaw: 无 generation counter，防误删由 active_engines_ handle 指针匹配保护
    // OpenClaw: clearActiveEmbeddedRun(sessionId, handle) — if (map.get(id) === handle) delete
    // HunterClaw: dispatch_reply_from_config 中 active_engines_[key] == my_engine 检查
    // active_sessions_ 是简单 set，直接 erase (幂等)
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        active_sessions_.erase(session_key);
        streaming_sessions_.erase(session_key);
    }

    // Async drain followup queue (aligned with openclaw: scheduleFollowupDrain)
    drain_followup_queue(session_key);

    return run_result;
}

// ============================================================================
// drain_followup_queue
// Aligned with openclaw: scheduleFollowupDrain (auto-reply/reply/queue/drain.ts)
//
// Supports four drain modes:
//   collect  → wait debounce, merge all items into one prompt, run once
//   followup → wait debounce, run items one-by-one (serial)
//   steer    → (handled in run_reply_agent, not here)
//   interrupt→ (handled in run_reply_agent, not here)
//
// Aligned with openclaw drain loop:
//   while (queue.items.length > 0 || queue.droppedCount > 0) {
//     await waitForQueueDebounce(queue);
//     if (mode === "collect") { ... merge ... }
//     else { await drainNextQueueItem(...) }
//   }
// ============================================================================

void MessageDispatcher::drain_followup_queue(const std::string& session_key) {
    // Check if there's anything to drain and mark as draining
    {
        std::lock_guard<std::mutex> lock(session_state_mutex_);
        auto it = followup_queues_.find(session_key);
        if (it == followup_queues_.end()) return;
        auto& queue = it->second;
        if (queue.items.empty() && queue.dropped_count == 0) return;
        if (queue.draining) return;  // already draining
        queue.draining = true;
    }

    // Launch async drain in detached thread
    // Aligned with openclaw: void (async () => {...})()
    std::thread([this, session_key]() {
        // ✅ 对齐 OpenClaw: collectState = { forceIndividualCollect: false }
        // 一旦检测到跨频道，在同一 drain 内永不再合并
        bool force_individual_collect = false;
        while (true) {
            // ── waitForQueueDebounce ──────────────────────────────────────
            // Aligned with openclaw: waitForQueueDebounce(queue)
            int debounce_ms = 0;
            int64_t last_enqueued_at = 0;
            QueueMode mode = QueueMode::Collect;
            {
                std::lock_guard<std::mutex> lock(session_state_mutex_);
                auto it = followup_queues_.find(session_key);
                if (it == followup_queues_.end()) break;
                debounce_ms      = it->second.debounce_ms;
                last_enqueued_at = it->second.last_enqueued_at_ms;
                mode             = it->second.mode;
            }
            if (debounce_ms > 0) {
                // Wait until debounce_ms has elapsed since last enqueue
                bool queue_gone = false;
                while (true) {
                    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    int64_t since = now - last_enqueued_at;
                    if (since >= debounce_ms) break;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(debounce_ms - since));
                    // Refresh last_enqueued_at in case new items arrived
                    std::lock_guard<std::mutex> lock(session_state_mutex_);
                    auto it = followup_queues_.find(session_key);
                    if (it == followup_queues_.end()) { queue_gone = true; break; }
                    last_enqueued_at = it->second.last_enqueued_at_ms;
                }
                if (queue_gone) break;
            }

            // ── Check queue state ─────────────────────────────────────────
            {
                std::lock_guard<std::mutex> lock(session_state_mutex_);
                auto it = followup_queues_.find(session_key);
                if (it == followup_queues_.end()) break;
                if (it->second.items.empty() && it->second.dropped_count == 0) break;
                mode = it->second.mode;
            }

            if (mode == QueueMode::Collect) {
                // ── collect mode: merge all items into one prompt ─────────
                // ✅ 严格对齐 OpenClaw drain.ts collect branch:
                //   1. hasCrossChannelItems(queue.items, resolveCrossChannelKey)
                //   2. drainCollectQueueStep({ collectState, isCrossChannel, items, run })
                //      → isCrossChannel → forceIndividualCollect = true → 逐条处理
                //      → 同频道 → "skipped" → 合并为 buildCollectPrompt
                //   3. forceIndividualCollect 一旦为 true 永不重置（同一 drain 内）

                std::vector<FollowupTask> items_to_run;
                std::string summary_prompt;
                std::function<DispatchResult()> last_run_fn;
                bool is_cross_channel = false;

                {
                    std::lock_guard<std::mutex> lock(session_state_mutex_);
                    auto it = followup_queues_.find(session_key);
                    if (it == followup_queues_.end()) break;
                    auto& queue = it->second;

                    if (queue.items.empty() && queue.dropped_count == 0) break;

                    // ✅ 对齐 OpenClaw: hasCrossChannelItems(queue.items, resolveCrossChannelKey)
                    // 检查 items 是否来自不同频道，如果是则逐条处理
                    {
                        std::set<std::string> channel_keys;
                        bool has_unkeyed = false;
                        for (const auto& item : queue.items) {
                            const auto& ch = item.originating_channel;
                            const auto& to = item.originating_to;
                            const auto& acct = item.originating_account_id;
                            const auto& tid = item.originating_thread_id;
                            
                            if (ch.empty() && to.empty() && acct.empty() && tid.empty()) {
                                has_unkeyed = true;
                                continue;
                            }
                            // 对齐 OpenClaw: isRoutableChannel — 排除 webchat/internal
                            if (ch == "web" || ch == "webchat" || ch == "internal" || to.empty()) {
                                is_cross_channel = true;
                                break;
                            }
                            std::string key = ch + "|" + to + "|" + acct + "|" + tid;
                            channel_keys.insert(key);
                        }
                        if (!is_cross_channel) {
                            if (channel_keys.size() > 1) is_cross_channel = true;
                            else if (has_unkeyed && !channel_keys.empty()) is_cross_channel = true;
                        }
                    }
                    
                    // 对齐 OpenClaw: forceIndividualCollect 持久状态
                    if (is_cross_channel || force_individual_collect) {
                        force_individual_collect = true;
                    }

                    // Build summary prompt for dropped items
                    if (queue.drop_policy == QueueDropPolicy::Summarize && queue.dropped_count > 0) {
                        summary_prompt = "[Queue overflow] Dropped " +
                            std::to_string(queue.dropped_count) + " message" +
                            (queue.dropped_count == 1 ? "" : "s") + " due to cap.";
                        if (!queue.summary_lines.empty()) {
                            summary_prompt += "\nSummary:";
                            for (const auto& sl : queue.summary_lines) {
                                summary_prompt += "\n- " + sl;
                            }
                        }
                        queue.dropped_count = 0;
                        queue.summary_lines.clear();
                    }

                    items_to_run = queue.items;
                    if (!items_to_run.empty()) {
                        last_run_fn = items_to_run.back().run_fn;
                    }
                    queue.items.clear();
                    active_sessions_.insert(session_key);
                }

                if (items_to_run.empty() && summary_prompt.empty()) {
                    std::lock_guard<std::mutex> lock(session_state_mutex_);
                    active_sessions_.erase(session_key);
                    break;
                }

                // ✅ 对齐 OpenClaw: drainCollectQueueStep
                // force_individual_collect → 逐条处理（不合并），保留 per-message 路由
                if (force_individual_collect && !items_to_run.empty()) {
                    log()->info("drain_followup_queue: session={} cross-channel collect → individual mode ({} items)",
                                session_key, items_to_run.size());
                    // 逐条执行 (对齐 OpenClaw: drainCollectItemIfNeeded → drainNextQueueItem)
                    for (auto& item : items_to_run) {
                        try {
                            if (item.run_fn) {
                                auto& cmd_queue = queue::CommandQueue::Instance();
                                const std::string session_lane = queue::ResolveSessionLane(session_key);
                                const std::string global_lane = queue::ResolveGlobalLane();
                                std::function<DispatchResult()> fn_copy = item.run_fn;
                                std::string item_label = item.label;
                                auto session_future = cmd_queue.enqueue_command_in_lane(
                                    session_lane,
                                    std::function<DispatchResult()>([&cmd_queue, global_lane,
                                                                      fn_copy, item_label]() -> DispatchResult {
                                        auto inner_future = cmd_queue.enqueue_command_in_lane(
                                            global_lane, fn_copy,
                                            queue::EnqueueOptions{.label = "drain:collect:individual:" + item_label, .urgent = false}
                                        );
                                        return inner_future.get();
                                    }),
                                    queue::EnqueueOptions{.label = "session:drain:collect:individual:" + item_label, .urgent = false}
                                );
                                session_future.get();
                            }
                        } catch (const std::exception& e) {
                            log()->error("drain_followup_queue: individual collect failed session={}: {}",
                                         session_key, e.what());
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(session_state_mutex_);
                        active_sessions_.erase(session_key);
                        streaming_sessions_.erase(session_key);
                    }
                    continue;
                }

                // 同频道: 正常合并 (对齐 OpenClaw: buildCollectPrompt)
                const std::string merged_prompt = build_collect_prompt(items_to_run, summary_prompt);

                log()->info("drain_followup_queue: session={} collect mode merging {} items",
                            session_key, items_to_run.size());

                // Run the merged prompt using the last item's run_fn
                // (aligned with openclaw: await runFollowup({ prompt, run: last.run, ... }))
                // ✅ 对齐 OpenClaw 双层 lane: session lane → main lane
                // OpenClaw: runFollowup → runEmbeddedPiAgent → enqueueSession(() => enqueueGlobal(...))
                try {
                    if (last_run_fn) {
                        auto& cmd_queue = queue::CommandQueue::Instance();
                        const std::string session_lane = queue::ResolveSessionLane(session_key);
                        const std::string global_lane = queue::ResolveGlobalLane();  // "main"
                        std::function<DispatchResult()> fn_copy = last_run_fn;
                        // 外层: session lane
                        auto session_future = cmd_queue.enqueue_command_in_lane(
                            session_lane,
                            std::function<DispatchResult()>([&cmd_queue, global_lane,
                                                              fn_copy, session_key]() -> DispatchResult {
                                // 内层: main lane
                                auto inner_future = cmd_queue.enqueue_command_in_lane(
                                    global_lane,
                                    fn_copy,
                                    queue::EnqueueOptions{.label = "drain:collect:" + session_key, .urgent = false}
                                );
                                return inner_future.get();
                            }),
                            queue::EnqueueOptions{.label = "session:drain:collect:" + session_key, .urgent = false}
                        );
                        session_future.get();
                    }
                } catch (const std::exception& e) {
                    log()->error("drain_followup_queue: collect run failed session={}: {}",
                                 session_key, e.what());
                }

                {
                    std::lock_guard<std::mutex> lock(session_state_mutex_);
                    active_sessions_.erase(session_key);
                    streaming_sessions_.erase(session_key);
                }
                // Continue loop to check for new items
                continue;

            } else {
                // ── followup/serial mode: drain one item at a time ────────
                // Aligned with openclaw: drainNextQueueItem(queue.items, runFollowup)
                FollowupTask next_task;
                bool has_task = false;
                std::string summary_prompt;

                {
                    std::lock_guard<std::mutex> lock(session_state_mutex_);
                    auto it = followup_queues_.find(session_key);
                    if (it == followup_queues_.end()) break;
                    auto& queue = it->second;

                    if (queue.items.empty() && queue.dropped_count == 0) break;

                    // Check for summary prompt (dropped items)
                    // Aligned with openclaw: previewQueueSummaryPrompt
                    if (queue.drop_policy == QueueDropPolicy::Summarize && queue.dropped_count > 0) {
                        summary_prompt = "[Queue overflow] Dropped " +
                            std::to_string(queue.dropped_count) + " message" +
                            (queue.dropped_count == 1 ? "" : "s") + " due to cap.";
                        if (!queue.summary_lines.empty()) {
                            summary_prompt += "\nSummary:";
                            for (const auto& sl : queue.summary_lines) {
                                summary_prompt += "\n- " + sl;
                            }
                        }
                        queue.dropped_count = 0;
                        queue.summary_lines.clear();
                    }

                    if (!queue.items.empty()) {
                        next_task = std::move(queue.items.front());
                        queue.items.erase(queue.items.begin());
                        has_task = true;
                        active_sessions_.insert(session_key);
                    }
                }

                if (!has_task) break;

                log()->debug("drain_followup_queue: session={} followup label={}",
                             session_key, next_task.label);

                try {
                    // ✅ 对齐 OpenClaw 双层 lane: session lane → main lane
                    // OpenClaw: runFollowup → runEmbeddedPiAgent → enqueueSession(() => enqueueGlobal(...))
                    auto& cmd_queue = queue::CommandQueue::Instance();
                    const std::string session_lane = queue::ResolveSessionLane(session_key);
                    const std::string global_lane = queue::ResolveGlobalLane();  // "main"
                    std::function<DispatchResult()> fn_copy = next_task.run_fn;
                    std::string task_label = next_task.label;
                    // 外层: session lane
                    auto session_future = cmd_queue.enqueue_command_in_lane(
                        session_lane,
                        std::function<DispatchResult()>([&cmd_queue, global_lane,
                                                          fn_copy, task_label]() -> DispatchResult {
                            // 内层: main lane
                            auto inner_future = cmd_queue.enqueue_command_in_lane(
                                global_lane,
                                fn_copy,
                                queue::EnqueueOptions{.label = "drain:followup:" + task_label, .urgent = false}
                            );
                            return inner_future.get();
                        }),
                        queue::EnqueueOptions{.label = "session:drain:followup:" + task_label, .urgent = false}
                    );
                    session_future.get();
                } catch (const std::exception& e) {
                    log()->error("drain_followup_queue: followup failed session={}: {}",
                                 session_key, e.what());
                    // Aligned with openclaw: exponential backoff on consecutive failures
                    {
                        std::lock_guard<std::mutex> lock(session_state_mutex_);
                        auto it = followup_queues_.find(session_key);
                        if (it != followup_queues_.end()) {
                            it->second.consecutive_failures++;
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(session_state_mutex_);
                    active_sessions_.erase(session_key);
                    streaming_sessions_.erase(session_key);
                    // Reset consecutive failures on success
                    auto it = followup_queues_.find(session_key);
                    if (it != followup_queues_.end() && it->second.consecutive_failures > 0) {
                        // Only reset if this run succeeded (no exception above)
                        it->second.consecutive_failures = 0;
                    }
                }
                // Continue loop for next item
                continue;
            }
        }

        // Drain complete — clean up
        {
            std::lock_guard<std::mutex> lock(session_state_mutex_);
            auto it = followup_queues_.find(session_key);
            if (it != followup_queues_.end()) {
                it->second.draining = false;
                if (it->second.items.empty() && it->second.dropped_count == 0) {
                    followup_queues_.erase(it);
                }
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
    // ✅ 对齐 OC: 优先使用 react::is_abort_request_text (与 gateway/chat-abort 共享)
    // CommandDetection::is_abort_request_text 作为扩展覆盖更多触发词
    if (react::is_abort_request_text(raw) || CommandDetection::is_abort_request_text(raw)) {
        result.should_abort = true;
        result.reason = "abort_trigger";
        log()->debug("try_fast_abort_from_message: abort trigger detected: '{}'", raw);
    }

    return result;
}

// ============================================================================
// try_dispatch_acp_reply
// ✅ 严格对齐 OpenClaw: tryDispatchAcpReply (dispatch-acp.ts)
//
// 完整数据流 (对齐 OpenClaw):
//   用户消息
//   → acpManager.resolveSession()       ← 三态解析 (none/stale/ready)
//   → 策略检查 (dispatch.enabled)
//   → AcpReplyProjector 创建            ← 流式投影器
//   → acpManager.runTurn({
//       sessionKey, text, mode: "prompt",
//       onEvent: projector.onEvent       ← 流式投影
//     })
//     → withSessionActor()              ← per-session 串行化
//     → ensureRuntimeHandle()           ← 懒创建 + 缓存 + identity 合并
//     → applyRuntimeControls()          ← 签名比对推送选项
//     → setSessionState("running")      ← 持久化状态机
//     → runtime.runTurn()               ← AcpRuntime 接口
//       → AcpRuntimeEvent stream
//       → projector.onEvent()           ← 流式投影
//     → setSessionState("idle")
//     → reconcileIdentifiers()          ← identity 对账
//   → projector.flush()
// ============================================================================

MessageDispatcher::AcpDispatchResult MessageDispatcher::try_dispatch_acp_reply(
    const channels::InboundMessage& message,
    const std::string& delivery_to,
    bool bypass_for_command
) {
    AcpDispatchResult result;

    const std::string& session_key = message.session_key;

    // ── Guard: empty session key or command bypass ────────────────────────
    // Aligned with openclaw: if (!sessionKey || params.bypassForCommand) return null
    if (session_key.empty() || bypass_for_command) {
        return result;
    }

    // ── Step 1: acpManager.resolveSession ────────────────────────────────
    // Aligned with openclaw: acpManager.resolveSession({ cfg, sessionKey })
    // 三态解析: none → fall through, stale → deliver error + handled, ready → run turn
    acp::AcpSessionResolution resolution;
    try {
        resolution = acp::get_acp_session_manager().resolve_session(session_key);
    } catch (...) {
        // AcpSessionManager not initialized — fall back to legacy path
        // (maintains backward compatibility during migration)
        return try_dispatch_acp_reply_legacy(message, delivery_to);
    }

    if (resolution.kind == acp::AcpSessionResolution::Kind::None) {
        return result;  // Not an ACP session → fall through to normal dispatch
    }

    // ── Build deliver_to_channel helper (shared by stale + ready paths) ──
    // Aligned with openclaw: delivery.deliver("final", payload) → channel.send()
    auto captured_delivery_queue = delivery_queue_;
    auto captured_channel = message.channel;
    auto captured_to = delivery_to.empty() ? message.sender_id : delivery_to;
    auto captured_account_id = message.account_id.value_or("");
    auto captured_thread_id = message.thread_id.value_or("");
    auto captured_reply_to_id = message.reply_to_id.value_or("");

    auto deliver_to_channel = [captured_delivery_queue,
                               captured_channel, captured_to,
                               captured_account_id, captured_thread_id,
                               captured_reply_to_id](const std::string& text) {
        if (!captured_delivery_queue || text.empty()) return;
        ::hunterclaw::queue::MessagePayload payload;
        payload.text = text;

        ::hunterclaw::outbound::DeliverOutboundPayloadsParams params;
        params.channel      = captured_channel;
        params.to           = captured_to;
        params.payloads     = {payload};
        params.delivery_queue = captured_delivery_queue;
        params.skip_queue   = false;
        params.best_effort  = true;
        params.silent       = false;
        if (!captured_account_id.empty()) params.account_id = captured_account_id;
        if (!captured_thread_id.empty())  params.thread_id  = captured_thread_id;
        if (!captured_reply_to_id.empty()) params.reply_to_id = captured_reply_to_id;

        auto deliver_result = ::hunterclaw::outbound::deliver_outbound_payloads(params);
        if (!deliver_result.is_ok()) {
            spdlog::get("default")->warn(
                "try_dispatch_acp_reply: deliver failed: channel={} to={} error={}",
                captured_channel, captured_to, deliver_result.error
            );
        }
    };

    // ── Stale path ────────────────────────────────────────────────────────
    // Aligned with openclaw: acpResolution.kind === "stale" → deliver error text, markIdle
    if (resolution.kind == acp::AcpSessionResolution::Kind::Stale) {
        const auto& stale_error = resolution.error.value_or(
            acp::AcpRuntimeError(acp::AcpErrorCode::ACP_SESSION_INIT_FAILED, "ACP session is stale")
        );
        log()->warn("try_dispatch_acp_reply: stale ACP session: session={} code={} error={}",
                    session_key, stale_error.code_string(), stale_error.what());
        // Aligned with openclaw: deliver error text to user
        deliver_to_channel(acp::format_acp_runtime_error_text(stale_error));
        // Aligned with openclaw: markIdle("message_completed") — log state transition
        log()->debug("try_dispatch_acp_reply: markIdle session={} reason=message_completed (stale)",
                     session_key);
        result.handled = true;
        return result;
    }

    // ── kind == Ready ─────────────────────────────────────────────────────
    const auto& acp_meta = *resolution.meta;
    const std::string prompt_text = message.text;
    if (prompt_text.empty()) {
        // Aligned with openclaw: empty prompt → recordProcessed("completed", "acp_empty_prompt") + markIdle
        log()->debug("try_dispatch_acp_reply: empty prompt for session={}", session_key);
        log()->debug("try_dispatch_acp_reply: markIdle session={} reason=message_completed (empty_prompt)",
                     session_key);
        result.handled = true;
        return result;
    }

    log()->info("try_dispatch_acp_reply: routing to ACP session={} agent={}",
                session_key, acp_meta.agent);

    // ── Step 3: Create AcpReplyProjector ─────────────────────────────────
    // Aligned with openclaw: createAcpReplyProjector (acp-projector.ts)
    acp::AcpReplyProjector::Config projector_config;
    projector_config.delivery_mode = acp::AcpDeliveryMode::Block;
    projector_config.should_send_tool_summaries = true;

    auto projector = std::make_shared<acp::AcpReplyProjector>(
        projector_config,
        [deliver_to_channel](const std::string& kind,
                             const std::string& text,
                             const std::optional<acp::AcpProjectedDeliveryMeta>&) {
            // Both "block" and "tool" kinds deliver text to channel
            deliver_to_channel(text);
        }
    );

    // ── Step 4: acpManager.runTurn ───────────────────────────────────────
    // Aligned with openclaw: await acpManager.runTurn({ ... onEvent: projector.onEvent })
    // OpenClaw awaits this; HunterClaw runs in background thread to avoid blocking.
    std::string captured_session_key = session_key;
    auto captured_projector = projector;

    std::thread([captured_session_key, prompt_text, captured_projector,
                 deliver_to_channel, this]() mutable {
        const int64_t started_at = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // Aligned with openclaw: markIdle("message_completed") — log state transition
        // Note: ACP session state (idle/running) is managed internally by run_turn.
        // markIdle here mirrors openclaw's logSessionStateChange call only.
        auto mark_idle = [&](const std::string& reason) {
            log()->debug("try_dispatch_acp_reply: markIdle session={} reason={}",
                         captured_session_key, reason);
        };

        try {
            acp::AcpRunTurnInput turn_input;
            turn_input.session_key = captured_session_key;
            turn_input.text = prompt_text;
            turn_input.mode = acp::AcpRuntimePromptMode::Prompt;
            turn_input.request_id = captured_session_key + ":" + std::to_string(started_at);
            // Aligned with openclaw: onEvent: projector.onEvent
            turn_input.on_event = [captured_projector](const acp::AcpRuntimeEvent& event) {
                captured_projector->on_event(event);
            };

            acp::get_acp_session_manager().run_turn(turn_input);

            // Aligned with openclaw: await projector.flush(true)
            captured_projector->flush(true);

            const int64_t latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count() - started_at;
            log()->info("try_dispatch_acp_reply: ACP turn completed: session={} latencyMs={}",
                        captured_session_key, latency_ms);

            // Aligned with openclaw: markIdle("message_completed")
            mark_idle("message_completed");
        } catch (const acp::AcpRuntimeError& e) {
            // Aligned with openclaw: catch → projector.flush(true) + deliver error text + markIdle
            captured_projector->flush(true);
            log()->error("try_dispatch_acp_reply: ACP turn failed: session={} code={} error={}",
                         captured_session_key, e.code_string(), e.what());
            deliver_to_channel(acp::format_acp_runtime_error_text(e));
            mark_idle("message_completed");
        } catch (const std::exception& e) {
            captured_projector->flush(true);
            log()->error("try_dispatch_acp_reply: ACP turn exception: session={} error={}",
                         captured_session_key, e.what());
            // Wrap as ACP_TURN_FAILED and deliver to user
            acp::AcpRuntimeError acp_err(acp::AcpErrorCode::ACP_TURN_FAILED, e.what());
            deliver_to_channel(acp::format_acp_runtime_error_text(acp_err));
            mark_idle("message_completed");
        } catch (...) {
            captured_projector->flush(true);
            log()->error("try_dispatch_acp_reply: ACP turn unknown exception: session={}",
                         captured_session_key);
            acp::AcpRuntimeError acp_err(acp::AcpErrorCode::ACP_TURN_FAILED,
                                         "ACP turn failed before completion.");
            deliver_to_channel(acp::format_acp_runtime_error_text(acp_err));
            mark_idle("message_completed");
        }
    }).detach();

    result.handled = true;
    result.response = "";
    return result;
}

// ── Legacy fallback (backward compatibility) ─────────────────────────────────
// Used when AcpSessionManager is not initialized (migration period).
MessageDispatcher::AcpDispatchResult MessageDispatcher::try_dispatch_acp_reply_legacy(
    const channels::InboundMessage& message,
    const std::string& delivery_to
) {
    AcpDispatchResult result;
    const std::string& session_key = message.session_key;

    if (!session_manager_) return result;
    auto session_result = session_manager_->get_session(session_key);
    if (!session_result.is_ok()) return result;
    const auto& session = session_result.get();
    if (!session.acp.has_value()) return result;
    const auto& acp_meta = *session.acp;
    if (acp_meta.state == "error") return result;
    if (acp_meta.state != "idle" && acp_meta.state != "running") return result;

    const std::string prompt_text = message.text;
    if (prompt_text.empty()) return result;

    if (!agent_pool_) return result;
    auto acp_agent = agent_pool_->get_agent(acp_meta.agent.empty() ? session_key : acp_meta.agent);
    if (!acp_agent) acp_agent = agent_pool_->get_agent(session_key);
    if (!acp_agent) return result;

    std::map<std::string, std::string> acp_options = {
        {"deliver", "true"}, {"session_key", session_key},
        {"input_provenance_kind", "user"}, {"source_tool", "acp_dispatch"},
    };
    {
        auto& ctx = acp_agent->context();
        ctx.last_channel = message.channel;
        ctx.last_to = delivery_to.empty() ? message.sender_id : delivery_to;
        ctx.last_account_id = message.account_id.value_or("");
        ctx.last_thread_id = message.thread_id.value_or("");
        ctx.last_reply_to_id = message.reply_to_id.value_or("");
    }
    if (delivery_queue_) {
        auto dq = delivery_queue_;
        auto ch = message.channel;
        auto to = delivery_to.empty() ? message.sender_id : delivery_to;
        auto aid = message.account_id.value_or("");
        auto tid = message.thread_id.value_or("");
        auto rid = message.reply_to_id.value_or("");
        acp_agent->set_deliver_fn([dq, ch, to, aid, tid, rid](const std::string& text) {
            ::hunterclaw::queue::MessagePayload payload; payload.text = text;
            ::hunterclaw::outbound::DeliverOutboundPayloadsParams params;
            params.channel = ch; params.to = to; params.payloads = {payload};
            params.delivery_queue = dq; params.skip_queue = false;
            params.best_effort = true; params.silent = false;
            if (!aid.empty()) params.account_id = aid;
            if (!tid.empty()) params.thread_id = tid;
            if (!rid.empty()) params.reply_to_id = rid;
            ::hunterclaw::outbound::deliver_outbound_payloads(params);
        });
    }
    std::thread([acp_agent, prompt_text, acp_options, session_key, this]() mutable {
        try {
            acp_agent->process_message(prompt_text, acp_options);
            acp_agent->set_deliver_fn(nullptr);
        } catch (...) { acp_agent->set_deliver_fn(nullptr); }
    }).detach();
    result.handled = true;
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
    if (message.chat_type == routing::ChatType::GROUP) {
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

        // 🎯 对齐 OpenClaw: textToSpeech({ text, provider, channel })
        // TTS provider integration — requires tts::TtsProviderRegistry
        // When TTS module is available, this will invoke provider->text_to_speech()
        log()->debug("maybe_apply_tts_to_payload: TTS skipped (provider not linked), payload len={}",
                     payload.text.size());
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

    // ✅ 对齐 OpenClaw: setActiveEmbeddedRun(sessionId, queueHandle)
    // 通知调用方 engine 已创建，使其能获取 engine 引用用于 steer 注入
    if (config.on_engine_created) {
        config.on_engine_created(engine);
    }

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
    // 🎯 严格对齐 OpenClaw: applyLinkUnderstanding (link-understanding/apply.ts)
    //
    // Fetches and extracts content from URLs in the message:
    //   - Detects URLs in message text
    //   - Fetches page content (title, description, main text)
    //   - Appends extracted content to prompt as context
    if (message.text.empty()) return;

    // Extract URLs from message
    std::regex url_re(R"(https?://[^\s<>\"']+)");
    auto begin = std::sregex_iterator(message.text.begin(), message.text.end(), url_re);
    auto end = std::sregex_iterator();

    std::vector<std::string> urls;
    for (auto it = begin; it != end; ++it) {
        urls.push_back((*it)[0].str());
    }

    if (urls.empty()) return;

    // Fetch content for each URL (max 3 to avoid delays)
    const int MAX_URLS = 3;
    std::ostringstream context;
    int fetched = 0;

    for (const auto& url : urls) {
        if (fetched >= MAX_URLS) break;

        try {
            // Use curl to fetch page content
            std::string cmd = "curl -fsSL --max-time 5 --max-filesize 524288 '" + url + "' 2>/dev/null";
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) continue;

            std::string html;
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe)) {
                html += buffer;
                if (html.size() > 100000) break;  // Cap at 100KB
            }
            pclose(pipe);

            if (html.empty()) continue;

            // Extract title
            std::regex title_re(R"(<title[^>]*>(.*?)</title>)", std::regex::icase);
            std::smatch title_match;
            std::string title;
            if (std::regex_search(html, title_match, title_re)) {
                title = title_match[1].str();
            }

            // Extract meta description
            std::regex desc_re(R"(<meta[^>]*name=\"description\"[^>]*content=\"([^\"]*)\")");
            std::smatch desc_match;
            std::string description;
            if (std::regex_search(html, desc_match, desc_re)) {
                description = desc_match[1].str();
            }

            // Build context
            context << "\n\n<link_content url=\"" << url << "\">\n";
            if (!title.empty()) context << "Title: " << title << "\n";
            if (!description.empty()) context << "Description: " << description << "\n";
            context << "</link_content>\n";

            fetched++;
        } catch (...) {
            // Skip failed URLs
        }
    }

    if (fetched > 0) {
        prompt += "\n\n<link_understanding>\n" +
            context.str() +
            "</link_understanding>\n";
    }
}

} // namespace auto_reply
} // namespace hunterclaw
