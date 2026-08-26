// ============================================================================
// sessions_send A2A Flow 实现 — 严格对齐 OpenClaw sessions-send-tool.a2a.ts
// ============================================================================
// 対齐要点（逐条标注真源行号，改动前请先核对真源）：
//   * wait 结果分三态（ok / 可恢复错误 / 投递失败），只有**投递失败**才通知
//     requester —— 対齐 a2a.ts:46-51 isDeliveryFailureWait + :119-138
//   * round-1 回复须与 baseline 比对，**未更新则视为无回复** —— 対齐 :114-117
//     hasUpdatedAssistantReplySnapshot，防止把上一轮的旧回复当成新结果重发
//   * announce target 先按 session key 就地解析，仅在 DM / 插件要求时才查
//     sessions.list —— 対齐 sessions-announce-target.ts:26-45
//   * same-session 直接投递分支的三个条件顺序不能动 —— 対齐 :158-176
// ============================================================================

#include "hunterclaw/tools/sessions_send_a2a.h"
#include "hunterclaw/tools/agent_step.h"
#include "hunterclaw/tools/a2a_context.h"
#include "hunterclaw/tools/support_tokens.h"
#include "hunterclaw/agents/run_wait.h"
#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/channels/session_conversation.h"
#include "hunterclaw/sessions/session_key_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <sstream>
#include <thread>
#include <spdlog/spdlog.h>

namespace {
    std::shared_ptr<spdlog::logger> a2a_log() {
        static auto logger = spdlog::get("sessions.a2a");
        if (!logger) logger = spdlog::stdout_logger_mt("sessions.a2a");
        return logger;
    }
}

namespace hunterclaw {
namespace tools {

namespace {

    const std::string SESSIONS_SEND_TOOL = "sessions_send";

    // ── 小工具 ──────────────────────────────────────────────────────────

    bool is_space_ch(unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }

    std::string trim(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && is_space_ch(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && is_space_ch(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    }

    std::string to_lower(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    std::string generate_uuid_v4() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<int> hex_dis(0, 15);
        const char* hex = "0123456789abcdef";
        std::string uuid(36, '-');
        for (size_t i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) continue;
            if (i == 14) { uuid[i] = '4'; continue; }
            if (i == 19) { uuid[i] = hex[(hex_dis(gen) & 0x3) | 0x8]; continue; }
            uuid[i] = hex[hex_dis(gen)];
        }
        return uuid;
    }

    // ── Wait 与回复快照：复用 agents/run_wait ────────────────────────────
    // 対齐 run-wait.ts。此前本文件与 agent_step.cc 各写一份简化版，
    // 都只是"倒着找最后一条 assistant"且把 wait 压成 bool，两处独立漂移。
    // 现统一收拢到 agents::run_wait，两个调用方共享同一份对齐实现。

    using agents::AgentWaitResult;
    using agents::AgentWaitStatus;
    using agents::AssistantReplySnapshot;

    /// 対齐 a2a.ts:46-51 isDeliveryFailureWait
    ///   (error 且不可恢复) 或 (timeout 且 pendingError)
    /// 只有这两种才说明"消息可能没送到"，才值得打断 requester；
    /// 连接抖动等可恢复错误必须静默，否则会误报投递失败。
    bool is_delivery_failure_wait(const AgentWaitResult& w) {
        return (w.status == AgentWaitStatus::Error &&
                !agents::is_recoverable_agent_wait_error(w.error)) ||
               (w.status == AgentWaitStatus::Timeout && w.pending_error);
    }

    // ── Announce target ─────────────────────────────────────────────────

    struct AnnounceTarget {
        std::string channel;
        std::string to;
        std::string account_id;
        std::string thread_id;
        bool valid() const { return !channel.empty() && !to.empty(); }
    };

    /// 対齐 helpers.ts:31-77 resolveAnnounceTargetFromKey
    /// 直接从 session key 推出投递地址，不需要网络往返。
    /// 原实现完全缺失这一步，只会查 sessions.list —— 一旦目标会话尚未出现在
    /// 列表里（新建会话、超出 limit=200、或 list 失败），announce 就静默丢弃。
    AnnounceTarget resolve_announce_target_from_key(const std::string& session_key) {
        AnnounceTarget out;
        if (trim(session_key).empty()) return out;

        // ① 群/频道会话：走 session-conversation 解析
        auto conv = channels::resolve_session_conversation_ref(session_key);
        if (conv.has_value()) {
            out.channel = conv->channel;
            if (conv->thread_id.has_value()) out.thread_id = *conv->thread_id;

            // 対齐 helpers.ts:64 —— generic target 形如 "channel:<id>" / "group:<id>"
            const std::string generic =
                (conv->kind == "channel" ? "channel:" : "group:") + conv->id;

            // 対齐 helpers.ts:66-71 —— 优先让插件做归一化，保住频道特有 ID 形态。
            // 顺序不能颠倒：resolveSessionTarget 能感知 thread，比
            // normalizeTarget 更精确，故先试它再退 normalizeTarget。
            std::string normalized;
            auto plugin = channels::ChannelPluginRegistry::instance()
                              .get_plugin(conv->channel);
            if (plugin.has_value() && plugin->messaging.has_value()) {
                const auto& msg = *plugin->messaging;
                // 対齐:67-71 —— resolveSessionTarget({kind, id, threadId})
                if (msg.resolve_session_target) {
                    channels::ChannelMessagingAdapter::SessionTargetRef ref;
                    ref.kind = conv->kind;
                    ref.id = conv->id;
                    if (conv->thread_id.has_value()) ref.thread_id = *conv->thread_id;
                    auto r = msg.resolve_session_target(ref);
                    if (r.has_value() && !r->empty()) normalized = *r;
                }
                // 対齐:71 —— `?? plugin?.messaging?.normalizeTarget?.(genericTarget)`
                if (normalized.empty() && msg.normalize_target) {
                    auto n = msg.normalize_target(generic);
                    if (n.has_value() && !n->empty()) normalized = *n;
                }
            }
            // 対齐 helpers.ts:74 —— 插件缺失时退回 generic；channel 未识别时退回裸 id
            out.to = !normalized.empty()
                ? normalized
                : (plugin.has_value() ? generic : conv->id);
            return out;
        }

        // ② DM/direct 会话：走 delivery route 解析
        auto route = sessions::parse_session_delivery_route(session_key);
        if (!route.has_value()) return out;
        const std::string peer_kind = to_lower(route->peer_kind);
        // 対齐 helpers.ts:35-37 —— 仅 direct/dm 可从 key 推地址，其余返回空
        if (peer_kind != "direct" && peer_kind != "dm") return out;

        out.channel = route->channel;
        if (route->account_id.has_value()) out.account_id = *route->account_id;
        if (route->thread_id.has_value())  out.thread_id  = *route->thread_id;

        // 対齐 helpers.ts:45-57 —— direct 目标形如 "user:<peerId>"。
        const std::string direct_target = "user:" + route->peer_id;
        std::string resolved_to;
        std::string resolved_thread;
        auto plugin = channels::ChannelPluginRegistry::instance()
                          .get_plugin(route->channel);
        if (plugin.has_value() && plugin->messaging.has_value()) {
            const auto& msg = *plugin->messaging;
            // 対齐:47-50 —— directTargetStyle == "user-prefixed" 时**跳过**
            // resolveDeliveryTarget：该频道的私聊地址本就是 user: 形态，
            // 再过一次房间解析器会把明确的私聊对象错当成群组。
            const bool user_prefixed = (msg.direct_target_style == "user-prefixed");
            if (!user_prefixed && msg.resolve_delivery_target) {
                auto r = msg.resolve_delivery_target(route->peer_id);
                if (r.has_value()) {
                    resolved_to = trim(r->to);
                    resolved_thread = r->thread_id;
                }
            }
            // 対齐:55 —— `resolvedTarget?.to?.trim() || normalizeTarget?.(t) || t`
            if (resolved_to.empty() && msg.normalize_target) {
                auto n = msg.normalize_target(direct_target);
                if (n.has_value() && !n->empty()) resolved_to = *n;
            }
        }
        out.to = resolved_to.empty() ? direct_target : resolved_to;
        // 対齐:57 —— `threadId: resolvedTarget?.threadId ?? directRoute.threadId`
        if (!resolved_thread.empty()) out.thread_id = resolved_thread;
        return out;
    }

    /// 対齐 sessions-announce-target.ts:22-71 resolveAnnounceTarget
    ///
    /// 顺序很关键：
    ///   1. 先按 key 就地解析（sessionKey 优先，displayKey 兜底）
    ///   2. 非 DM 且插件未要求会话查表 → **直接返回**，不发网络请求
    ///   3. 否则查 sessions.list 取权威的 deliveryContext
    ///   4. 查表无果 → 回落到步骤 1 的结果
    /// 原实现跳过了 1、2、4，只做 3，且失败时返回空。
    AnnounceTarget resolve_announce_target(
        std::shared_ptr<gateway::GatewayClient> gw,
        const std::string& session_key,
        const std::string& display_key)
    {
        // ── ① 就地解析 ──
        AnnounceTarget fallback = resolve_announce_target_from_key(session_key);
        if (!fallback.valid()) {
            AnnounceTarget by_display = resolve_announce_target_from_key(display_key);
            if (by_display.valid()) fallback = by_display;
        }

        // 対齐 sessions-announce-target.ts:29-32 fallbackThreadId
        //   fallback?.threadId
        //     ?? parseThreadSessionSuffix(sessionKey).threadId
        //     ?? parseThreadSessionSuffix(displayKey).threadId
        // 缺这一步会丢掉论坛/话题会话的 threadId，announce 投到主频道而非原话题。
        std::string fallback_thread_id = fallback.thread_id;
        if (fallback_thread_id.empty()) {
            const auto t1 = sessions::parse_thread_session_suffix(session_key);
            if (t1.thread_id.has_value()) fallback_thread_id = *t1.thread_id;
        }
        if (fallback_thread_id.empty()) {
            const auto t2 = sessions::parse_thread_session_suffix(display_key);
            if (t2.thread_id.has_value()) fallback_thread_id = *t2.thread_id;
        }
        // 就地解析结果自身也补上 thread（真源 fallback 返回时带的就是它自己的
        // threadId，但下游查表分支要用 fallbackThreadId 兜底）
        if (fallback.thread_id.empty()) fallback.thread_id = fallback_thread_id;

        // ── ② 判断是否需要查表 ──
        auto route = sessions::parse_session_delivery_route(session_key);
        if (!route.has_value()) {
            route = sessions::parse_session_delivery_route(display_key);
        }
        const std::string peer_kind = route.has_value() ? to_lower(route->peer_kind) : "";
        const bool is_direct_route = (peer_kind == "direct" || peer_kind == "dm");

        if (fallback.valid()) {
            bool prefer_lookup = false;
            auto plugin = channels::ChannelPluginRegistry::instance()
                              .get_plugin(fallback.channel);
            if (plugin.has_value()) {
                prefer_lookup = plugin->meta.prefer_session_lookup_for_announce_target;
            }
            // 対齐 sessions-announce-target.ts:42-44 —— 群/频道且插件未要求查表时，
            // 解析结果已经权威，直接用，省掉一次 sessions.list 往返。
            if (!is_direct_route && !prefer_lookup) return fallback;
        }

        // ── ③ 查 sessions.list 取权威 deliveryContext ──
        if (!gw) return fallback;
        try {
            auto list_result = gw->request<nlohmann::json>("sessions.list", nlohmann::json{
                {"includeGlobal", true},
                {"includeUnknown", true},
                {"limit", 200}
            });
            if (list_result.is_ok()) {
                const auto sessions =
                    list_result.get().value("sessions", nlohmann::json::array());
                nlohmann::json match;
                for (const auto& entry : sessions) {
                    if (entry.is_object() && entry.value("key", "") == session_key) {
                        match = entry;
                        break;
                    }
                }
                if (match.is_null()) {
                    for (const auto& entry : sessions) {
                        if (entry.is_object() && entry.value("key", "") == display_key) {
                            match = entry;
                            break;
                        }
                    }
                }
                if (match.is_object() && match.contains("deliveryContext") &&
                    match["deliveryContext"].is_object()) {
                    const auto& dc = match["deliveryContext"];
                    const std::string ch = dc.value("channel", "");
                    const std::string to = dc.value("to", "");
                    // 対齐:63-65 —— 仅当 channel 与 to 双双存在才采用查表结果
                    if (!ch.empty() && !to.empty()) {
                        AnnounceTarget out;
                        out.channel = ch;
                        out.to = to;
                        out.account_id = dc.value("accountId", "");
                        // 対齐:62 —— threadId 缺失时回落到 fallbackThreadId
                        // （它已包含按 key 解析 + thread 后缀两级兜底）
                        out.thread_id = dc.contains("threadId") &&
                                        !dc["threadId"].is_null()
                            ? dc.value("threadId", "")
                            : fallback_thread_id;
                        if (out.thread_id.empty()) out.thread_id = fallback_thread_id;
                        return out;
                    }
                }
            }
        } catch (const std::exception& e) {
            // 対齐:66-68 —— 查表失败不算致命，回落到就地解析结果
            a2a_log()->warn("a2a_flow: sessions.list lookup failed: {}", e.what());
        }

        // ── ④ 回落 ──
        return fallback;
    }

    /// 対齐 a2a.ts:53-83 deliverAnnounceReply
    void deliver_announce_reply(
        std::shared_ptr<gateway::GatewayClient> gw,
        const AnnounceTarget& target,
        const std::string& message,
        const std::string& run_context_id)
    {
        if (!gw) return;
        // 対齐:58-61 —— trim 后为空则不投递
        const std::string trimmed = trim(message);
        if (trimmed.empty()) return;
        if (!target.valid()) {
            a2a_log()->warn("a2a_flow: cannot deliver announce — no channel/to (run={})",
                            run_context_id);
            return;
        }

        // ⚠ 対齐:58,67 —— 真源只做 trim，**不剥 token**。
        // 此前这里额外调了 strip_support_tokens，属反向偏差：混合正文若恰好
        // 以 token 结尾（如"处理完了，无需回复 NO_REPLY"），真源原样投递，
        // 剥了会把用户可见的尾部截掉。是否投递已由上游 is_announce_skip /
        // is_non_deliverable_sessions_reply 决定，到这一步就不该再改写正文。
        nlohmann::json send_params = {
            {"to",             target.to},
            {"message",        trimmed},
            {"channel",        target.channel},
            {"idempotencyKey", generate_uuid_v4()}
        };
        if (!target.account_id.empty()) send_params["accountId"] = target.account_id;
        if (!target.thread_id.empty())  send_params["threadId"]  = target.thread_id;

        try {
            auto result = gw->request<nlohmann::json>("send", send_params);
            if (!result.is_ok()) {
                // 対齐:75-82 —— 投递失败只 warn，不向上抛（外层不因此中断）
                a2a_log()->warn("a2a_flow: announce delivery failed (run={} channel={} to={}): {}",
                                run_context_id, target.channel, target.to, result.error);
            }
        } catch (const std::exception& e) {
            a2a_log()->warn("a2a_flow: announce delivery exception (run={}): {}",
                            run_context_id, e.what());
        }
    }

    /// 対齐 a2a.ts:119-138 —— 仅在 wait 属**投递失败**时通知 requester。
    void notify_requester_of_wait_failure(
        std::shared_ptr<gateway::GatewayClient> gw,
        const SessionsSendA2AParams& params,
        const std::string& error_text)
    {
        // 対齐:124-125 —— 有具体错误文本时拼到消息里，否则留空
        const std::string trimmed_err = trim(error_text);
        const std::string err_suffix = trimmed_err.empty() ? "" : (": " + trimmed_err);
        const std::string notify_msg =
            "sessions_send delivery to " + params.display_key + " failed" + err_suffix + ". "
            "The target may not have received the message; retry or report the failure "
            "instead of assuming delivery succeeded.";
        const std::string extra_context =
            "A previous sessions_send delivery failed after it was accepted. "
            "Decide whether to retry, use another route, or report the failure. "
            "Do not assume the target received the message.";

        // 対齐:126-137 —— provenance 指向 target 会话，sourceTool 为 sessions_send；
        // lane 留空即取 nested（等价 resolveNestedAgentLaneForSession）。
        AgentStepParams step;
        step.session_key        = params.requester_session_key;
        step.message            = notify_msg;
        step.extra_system_prompt = extra_context;
        step.timeout_ms         = params.announce_timeout_ms;
        step.source_session_key  = params.target_session_key;
        step.source_tool         = SESSIONS_SEND_TOOL;
        run_agent_step(gw, step);
    }

}  // anonymous namespace

// ============================================================================
// Main A2A Flow — 対齐 a2a.ts:85-259 runSessionsSendA2AFlow
// ============================================================================

void run_sessions_send_a2a_flow(
    std::shared_ptr<gateway::GatewayClient> gw,
    const SessionsSendA2AParams& params)
{
    const std::string run_context_id = params.wait_run_id.empty()
        ? "unknown" : params.wait_run_id;

    try {
        // ── Step 1: 取 round-1 回复（対齐 :100-141）────────────────────
        std::string primary_reply = params.round_one_reply;
        std::string latest_reply  = params.round_one_reply;

        if (primary_reply.empty() && !params.wait_run_id.empty()) {
            // 対齐:103-117 —— waitForAgentRun + readLatestAssistantReplySnapshot
            // + hasUpdatedAssistantReplySnapshot 三步。wait 超时上限 60s，
            // 且不超过 announce 预算。
            //
            // baseline 比对不可省：少了它会把上一轮的旧回复当成本轮结果重复投递。
            const agents::WaitAndReadResult result =
                agents::wait_for_agent_run_and_read_updated_assistant_reply(
                    gw, params.wait_run_id, params.target_session_key,
                    std::min(params.announce_timeout_ms, 60000), params.baseline);

            if (result.wait.status == AgentWaitStatus::Ok) {
                // reply_text 已由共享实现做过 baseline 比对，未更新即为空
                primary_reply = result.reply_text;
                latest_reply = primary_reply;
            } else {
                // 対齐:119-139 —— 只有"投递失败"才打断 requester；
                // 连接抖动等可恢复错误静默返回，避免误报。
                if (params.notify_requester_on_wait_failure &&
                    !params.requester_session_key.empty() &&
                    is_delivery_failure_wait(result.wait)) {
                    notify_requester_of_wait_failure(gw, params, result.wait.error);
                }
                return;
            }
        }

        // 対齐:142-147
        if (latest_reply.empty()) return;
        if (support_tokens::is_non_deliverable_sessions_reply(latest_reply)) return;

        // ── Step 2: 解析 announce target（対齐 :149-153）───────────────
        const AnnounceTarget announce_target = resolve_announce_target(
            gw, params.target_session_key, params.display_key);
        const std::string target_channel =
            announce_target.valid() ? announce_target.channel : std::string("unknown");

        // ── Step 3: same-session 源回复（対齐 :155-176）────────────────
        // 同会话发送本质是"面向人的源频道回复"，不是真正的 A2A 公告。
        // 让同一会话再决定是否公告会重跑同一 prompt，造成源回复重复。
        const bool same_session_source_reply =
            !params.requester_session_key.empty() &&
            params.requester_session_key == params.target_session_key;
        // 対齐:160-162 —— requesterChannel 为空视为"不限制"，可直投
        const bool can_direct_deliver_same_session_reply =
            announce_target.valid() &&
            (params.requester_channel.empty() ||
             params.requester_channel == announce_target.channel);

        if (same_session_source_reply && can_direct_deliver_same_session_reply) {
            // 対齐:164-166 —— 走了 wait 分支、既无预取回复也无 baseline 时，
            // 无法判断回复是否属于本轮，宁可不投递。
            if (!params.wait_run_id.empty() && params.round_one_reply.empty() &&
                params.baseline.empty() && params.baseline.fingerprint.empty()) {
                return;
            }
            deliver_announce_reply(gw, announce_target, latest_reply, run_context_id);
            return;
        }
        // 対齐:174-176 —— 同会话但无投递目标，直接结束（不进 ping-pong/announce）
        if (same_session_source_reply && !announce_target.valid()) return;

        // ── Step 4: Ping-pong（対齐 :178-218）─────────────────────────
        if (params.max_ping_pong_turns > 0 &&
            !params.requester_session_key.empty() &&
            params.requester_session_key != params.target_session_key)
        {
            std::string current_key = params.requester_session_key;
            std::string next_key    = params.target_session_key;
            std::string incoming    = latest_reply;

            for (int turn = 1; turn <= params.max_ping_pong_turns; ++turn) {
                // 対齐:187-188 —— 按当前 session key 判定角色
                const A2ARole current_role = (current_key == params.requester_session_key)
                    ? A2ARole::Requester : A2ARole::Target;

                // 対齐:189-197 —— target 侧传 displayKey；session 在 context 内
                // 被占位符化，仅 channel 保留具体值（保护 prompt-cache）。
                const std::string reply_context = build_agent_to_agent_reply_context(
                    params.requester_session_key,
                    params.requester_channel,
                    params.display_key,
                    target_channel,
                    current_role,
                    turn,
                    params.max_ping_pong_turns);

                // 対齐:198-208 —— provenance 指向"对方"会话，sourceChannel 随之判定。
                // lane 留空即取 nested，等价 resolveNestedAgentLaneForSession。
                AgentStepParams step;
                step.session_key         = current_key;
                step.message             = incoming;
                step.extra_system_prompt = reply_context;
                step.timeout_ms          = params.announce_timeout_ms;
                step.source_session_key  = next_key;
                step.source_channel      = (next_key == params.requester_session_key)
                    ? params.requester_channel : target_channel;
                step.source_tool         = SESSIONS_SEND_TOOL;

                auto step_result = run_agent_step(gw, step);
                if (!step_result.is_ok()) {
                    a2a_log()->warn("a2a_flow: ping-pong step failed turn {}: {}",
                                    turn, step_result.error);
                    break;
                }

                const std::string reply_text = step_result.get();
                // 対齐:209-211
                if (reply_text.empty() ||
                    support_tokens::is_reply_skip(reply_text) ||
                    support_tokens::is_non_deliverable_sessions_reply(reply_text)) {
                    break;
                }

                latest_reply = reply_text;
                incoming = reply_text;
                // 対齐:214-216 —— 交换双方，下一轮换手
                std::swap(current_key, next_key);
            }
        }

        // ── Step 5: Announce（対齐 :220-252）──────────────────────────
        const std::string announce_prompt = build_agent_to_agent_announce_context(
            params.requester_session_key,
            params.requester_channel,
            params.display_key,
            target_channel,
            params.message,
            // 対齐:226 —— 这里传的是 primaryReply（可能来自 wait 后的快照），
            // 不是入参 round_one_reply；两者在走 wait 分支时并不相同。
            primary_reply,
            latest_reply);

        // 対齐:229-239 —— transcriptMessage="" 走保留 transcript 的路径；
        // provenance 指向 requester。
        AgentStepParams announce_step;
        announce_step.session_key         = params.target_session_key;
        announce_step.message             = "Agent-to-agent announce step.";
        announce_step.extra_system_prompt = announce_prompt;
        announce_step.timeout_ms          = params.announce_timeout_ms;
        announce_step.transcript_message  = std::string("");
        announce_step.source_session_key  = params.requester_session_key;
        announce_step.source_channel      = params.requester_channel;
        announce_step.source_tool         = SESSIONS_SEND_TOOL;

        auto announce_result = run_agent_step(gw, announce_step);
        if (announce_result.is_ok()) {
            const std::string reply = announce_result.get();
            // 対齐:240-246 —— 需 announceTarget 有效、trim 后非空、且非跳过 token
            if (announce_target.valid() &&
                !trim(reply).empty() &&
                !support_tokens::is_announce_skip(reply) &&
                !support_tokens::is_non_deliverable_sessions_reply(reply)) {
                deliver_announce_reply(gw, announce_target, reply, run_context_id);
            }
        }

    } catch (const std::exception& e) {
        // 対齐:253-258 —— 整个流程是尽力而为，异常只 warn 不外抛
        a2a_log()->warn("a2a_flow: runSessionsSendA2AFlow failed (run={}): {}",
                        run_context_id, e.what());
    }
}

}  // namespace tools
}  // namespace hunterclaw
