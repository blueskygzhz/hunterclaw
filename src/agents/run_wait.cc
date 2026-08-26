// ============================================================================
// Agent Run Wait 实现 — 严格对齐 OpenClaw agents/run-wait.ts
// ============================================================================

#include "hunterclaw/agents/run_wait.h"

#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>

namespace {
    std::shared_ptr<spdlog::logger> rw_log() {
        static auto logger = spdlog::get("agents.run_wait");
        if (!logger) logger = spdlog::stdout_logger_mt("agents.run_wait");
        return logger;
    }
}

namespace hunterclaw {
namespace agents {

namespace {

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

/// 対齐 chat-history-text.ts:10-18 stripToolMessages
bool is_tool_message(const nlohmann::json& msg) {
    const std::string role = msg.value("role", "");
    return role == "toolResult" || role == "tool";
}

/// 対齐 run-wait.ts:179-190 isInterSessionInputMessage
/// 跨会话注入的输入本身是轮次边界，不能被当作 assistant 回复。
bool is_inter_session_message(const nlohmann::json& msg) {
    if (!msg.contains("provenance") || !msg["provenance"].is_object()) return false;
    return msg["provenance"].value("kind", "") == "inter_session";
}

// ── Transcript artifact 判定 ─────────────────────────────────────────────
// 対齐 shared/transcript-only-openclaw-assistant.ts
//
// ⚠ 这不是可选项：HunterClaw 的 transcript_io.cc:216-219 会给注入的
// assistant 行打上 provider="hunterclaw" + model="gateway-injected"。
// 不过滤这些行，它们就会被当成模型的真实回复投递出去 ——
// 用户会收到系统注入的 transcript 文本。

/// 対齐:5 OPENCLAW_TRANSCRIPT_ARTIFACT_PROVIDER
/// HunterClaw 侧的 provider 名是 "hunterclaw"（见 transcript_io.cc:218），
/// 同时兼容上游的 "openclaw"，避免混合来源的历史数据漏判。
bool is_transcript_artifact_provider(const std::string& provider) {
    return provider == "hunterclaw" || provider == "openclaw";
}

/// 対齐:9-12 TRANSCRIPT_ONLY_OPENCLAW_ASSISTANT_MODELS
bool is_transcript_only_model(const std::string& model) {
    return model == "delivery-mirror" || model == "gateway-injected";
}

/// 対齐:13-25 isOpenClawDeliveryMirrorMarker
bool is_delivery_mirror_marker(const nlohmann::json& msg, const char* field) {
    if (!msg.contains(field) || !msg[field].is_object()) return false;
    const std::string kind = msg[field].value("kind", "");
    return kind == "channel-final" || kind == "channel-final-suppressed" ||
           kind == "message-tool-source-reply";
}

/// 対齐:43-60 isTranscriptOnlyOpenClawAssistantMessage
/// 主判定：provider + model 命中已知的 transcript-only 集合。
/// 兜底：deliveryMirror 标记 —— 覆盖 provider/model 被剥掉的历史行（#99470）。
bool is_transcript_only_assistant(const nlohmann::json& msg) {
    if (msg.value("role", "") != "assistant") return false;
    if (is_transcript_artifact_provider(msg.value("provider", "")) &&
        is_transcript_only_model(msg.value("model", ""))) {
        return true;
    }
    return is_delivery_mirror_marker(msg, "hunterclawDeliveryMirror") ||
           is_delivery_mirror_marker(msg, "openclawDeliveryMirror");
}

/// 対齐:62-68 isOpenClawMessageToolMirrorAssistantMessage
/// 只要该标记**存在**（哪怕是 null）即算 mirror —— 対齐 `!== undefined`。
bool is_message_tool_mirror_assistant(const nlohmann::json& msg) {
    if (msg.value("role", "") != "assistant") return false;
    return msg.contains("hunterclawMessageToolMirror") ||
           msg.contains("openclawMessageToolMirror");
}

/// 対齐 run-wait.ts:171-177 isWaitedReplyTranscriptArtifact
bool is_waited_reply_transcript_artifact(const nlohmann::json& msg) {
    return is_transcript_only_assistant(msg) ||
           is_message_tool_mirror_assistant(msg) ||
           is_inter_session_message(msg);
}

/// 対齐 run-wait.ts:192-197 isWaitedReplyTurnBoundary
bool is_turn_boundary(const nlohmann::json& msg) {
    return msg.value("role", "") == "user" || is_inter_session_message(msg);
}

/// 対齐 chat-message-content.ts:36-38 isAssistantTextContentBlockType
/// ⚠ 三种类型都算文本块。此前只认 "text"，会漏掉 responses API 风格的
/// input_text / output_text，导致这些消息被当成"无文本"而整条跳过。
bool is_assistant_text_block_type(const std::string& type) {
    return type == "text" || type == "input_text" || type == "output_text";
}

/// 対齐 chat-history-text.ts:28-51 extractAssistantText
///   → shared/chat-message-content.ts:130-216 extractAssistantTextForPhase
///
/// 実装范围说明（不是偷懒，是标记齐备度所限）：
///   * phase / textSignature 过滤**未实现** —— HunterClaw 的消息不带
///     `phase` 也不带 `textSignature`（已核实：全仓无 final_answer/
///     textSignature 的产出点）。真源用它们把 commentary（思考）与
///     final_answer 分开；标记不存在时真源的 shouldIncludeContent 亦退化为
///     "resolvedPhase === undefined" 恒真，与本实现等价。
///     一旦将来 chat.history 开始输出 phase，必须回来补这一层，否则
///     模型的思考文本会被当成回复投递出去。
///   * sanitizeAssistantVisibleTextWithProfile / sanitizeUserFacingText
///     未实现（属独立的文本清洗子系统，不在 A2A 对齐范围）。
std::string extract_assistant_text(const nlohmann::json& msg) {
    // 対齐:132-134 —— 非 assistant 一律返回空
    if (msg.value("role", "") != "assistant") return std::string();

    // 対齐:158-163 —— 优先取顶层 text 字段。此前完全缺失该分支，
    // 只看 content，带 text 字段的消息会被误判为空。
    if (msg.contains("text") && msg["text"].is_string()) {
        return msg["text"].get<std::string>();
    }
    // 対齐:165-170 —— content 为字符串
    if (msg.contains("content") && msg["content"].is_string()) {
        return msg["content"].get<std::string>();
    }
    // 対齐:172-174 —— 非数组则无文本
    if (!msg.contains("content") || !msg["content"].is_array()) {
        return std::string();
    }
    // 対齐:192-216 —— 逐块取文本，joinWith: ""（直接相接，不加分隔）。
    // 対齐:203 —— 空白块被 filter 掉，不参与拼接。
    std::string text;
    for (const auto& block : msg["content"]) {
        if (!block.is_object()) continue;
        if (!is_assistant_text_block_type(block.value("type", ""))) continue;
        if (!block.contains("text") || !block["text"].is_string()) continue;
        const std::string t = block["text"].get<std::string>();
        if (trim(t).empty()) continue;
        text += t;
    }
    return text;
}

/// 対齐 run-wait.ts:199-211 snapshotAssistantReply
AssistantReplySnapshot snapshot_assistant_reply(const nlohmann::json& msg) {
    AssistantReplySnapshot snap;
    const std::string text = extract_assistant_text(msg);
    // 対齐:201-203 —— trim 后为空视为无快照
    if (trim(text).empty()) return snap;
    snap.text = text;
    try {
        // 対齐:206 —— 指纹取整条消息的序列化结果
        snap.fingerprint = msg.dump();
    } catch (...) {
        // 対齐:207-208 —— 序列化失败时退回文本
        snap.fingerprint = text;
    }
    return snap;
}

/// 対齐 run-wait.ts:235-311 resolveLatestAssistantReplySnapshot
/// （stopAtTranscriptArtifact = true 的分支）
///
/// 語義要点：
///   * 倒序扫描，遇轮次边界（user / inter_session 注入）即停；
///   * transcript artifact（gateway 注入行 / message-tool mirror）必须**跳过**，
///     且一旦见过就把整个结果作废（返回空）—— 対齐:302-309。
///     这条很关键：见过 artifact 说明本轮的真实回复尚未落库，
///     此时若返回边界内更早的那条 assistant，就会把**上一轮**的回复
///     当成本轮结果重复投递。
///
/// 未实现部分（标记齐备度所限，已核实）：
///   真源还借 __openclaw.seq / openclawMessageToolMirror.sourceMessageSeq
///   把 "internal source-reply mirror" 从普通 artifact 里摘出来单独返回
///   （:250-284）。HunterClaw 的 chat.history 不输出这两个 seq 标记，
///   无法判断 mirror 与边界的先后，故统一按 artifact 作废处理 ——
///   这是**偏保守**的方向（宁可不投递，不会误投旧内容）。
AssistantReplySnapshot resolve_latest_assistant_reply_snapshot(
    const nlohmann::json& messages)
{
    AssistantReplySnapshot latest;
    if (!messages.is_array()) return latest;

    bool saw_transcript_artifact = false;

    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        const auto& msg = messages[static_cast<size_t>(i)];
        // 対齐:246-249 —— 非对象直接跳过
        if (!msg.is_object()) continue;
        // 対齐 stripToolMessages —— tool 消息先被过滤掉
        if (is_tool_message(msg)) continue;
        // 対齐:250-265 —— 触及轮次边界即停止向前查找
        if (is_turn_boundary(msg)) break;
        // 対齐:266-268
        if (msg.value("role", "") != "assistant") continue;

        // 対齐:285-290 —— transcript artifact 跳过并置标志
        if (is_waited_reply_transcript_artifact(msg)) {
            saw_transcript_artifact = true;
            continue;
        }

        const AssistantReplySnapshot snap = snapshot_assistant_reply(msg);
        if (snap.empty()) continue;
        // 対齐:298-300 —— 只保留边界内最新的那条
        if (latest.empty()) latest = snap;
    }

    // 対齐:302-309 —— 见过 artifact 则整体作废，防止把上一轮回复当本轮结果
    if (saw_transcript_artifact) return AssistantReplySnapshot();
    return latest;
}

}  // namespace

AgentWaitResult wait_for_agent_run(std::shared_ptr<gateway::GatewayClient> gw,
                                   const std::string& run_id,
                                   int timeout_ms)
{
    AgentWaitResult out;
    if (!gw || run_id.empty()) {
        out.status = AgentWaitStatus::Error;
        out.error = "gateway unavailable or empty runId";
        return out;
    }
    try {
        auto result = gw->request<nlohmann::json>("agent.wait", nlohmann::json{
            {"runId", run_id},
            {"timeoutMs", timeout_ms}
        });
        if (!result.is_ok()) {
            // 対齐:394-400 —— 传输层异常归为 error/timeout，
            // 保留原文供 is_recoverable_agent_wait_error 判定。
            out.error = result.error;
            out.status = (result.error.find("gateway timeout") != std::string::npos)
                ? AgentWaitStatus::Timeout : AgentWaitStatus::Error;
            return out;
        }
        const auto& w = result.get();
        const std::string status = w.value("status", "");
        out.error = w.value("error", "");
        out.pending_error = w.value("pendingError", false);

        // 対齐:107-112 —— startedAt / endedAt 仅在确为 number 时才算存在；
        // presence 本身就是 isTerminalAgentWaitTimeout 的判定依据之一。
        if (w.contains("startedAt") && w["startedAt"].is_number()) {
            out.has_started_at = true;
            out.started_at = w["startedAt"].get<double>();
        }
        if (w.contains("endedAt") && w["endedAt"].is_number()) {
            out.has_ended_at = true;
            out.ended_at = w["endedAt"].get<double>();
        }
        // 対齐:101,110 —— stopReason / livenessState 仅取字符串形态
        if (w.contains("stopReason") && w["stopReason"].is_string()) {
            out.stop_reason = w["stopReason"].get<std::string>();
        }
        if (w.contains("livenessState") && w["livenessState"].is_string()) {
            out.liveness_state = w["livenessState"].get<std::string>();
        }
        // 対齐:111 —— 严格 === true
        out.yielded = w.contains("yielded") && w["yielded"].is_boolean() &&
                      w["yielded"].get<bool>();
        // 対齐:113 —— normalizeAgentRunTimeoutPhase
        if (w.contains("timeoutPhase") && w["timeoutPhase"].is_string()) {
            out.timeout_phase = normalize_agent_run_timeout_phase(
                w["timeoutPhase"].get<std::string>());
        }
        // 対齐:114 —— providerStarted 是三态：未提供 / true / false。
        // 这里必须区分"未提供"与 false —— hard_timeout 只在明确 true 时成立。
        if (w.contains("providerStarted") && w["providerStarted"].is_boolean()) {
            out.has_provider_started = true;
            out.provider_started = w["providerStarted"].get<bool>();
        }
        // 対齐:115 + agent-run-terminal-reply.ts:38-57
        // normalizeAgentRunTerminalReplySnapshot
        if (w.contains("terminalReply") && w["terminalReply"].is_object()) {
            const auto& tr = w["terminalReply"];
            const std::string disp = tr.value("disposition", "");
            if (disp == "silent") {
                out.terminal_reply = TerminalReplyDisposition::Silent;
            } else if (disp == "empty") {
                out.terminal_reply = TerminalReplyDisposition::Empty;
            } else if (disp == "visible" && tr.contains("text") &&
                       tr["text"].is_string()) {
                // 対齐:51-56 —— text 必须是字符串；sanitize 后为空则降级为 empty
                const std::string text = trim(tr["text"].get<std::string>());
                out.terminal_reply = text.empty()
                    ? TerminalReplyDisposition::Empty
                    : TerminalReplyDisposition::Visible;
                out.terminal_reply_text = text;
            }
        }

        // 対齐:384-393 —— 先按 gateway 原始 status 定初值
        if (status == "timeout")      out.status = AgentWaitStatus::Timeout;
        else if (status == "pending") out.status = AgentWaitStatus::Pending;
        else if (status == "error")   out.status = AgentWaitStatus::Error;
        else                          out.status = AgentWaitStatus::Ok;

        // 対齐:102-103 —— 再过终态归一化（buildAgentRunTerminalOutcomeFromWaitResult
        // + normalizeTerminalOutcomeForWait）。这一步会依据
        // stopReason / livenessState / timeoutPhase / providerStarted
        // 改写 status 与 error，是 hard_timeout / blocked / aborted / cancelled
        // 能被正确识别的关键。
        //
        // 対齐:699-701 —— pending 不参与终态归一化（返回 undefined），
        // 保持原状态不动。
        if (out.status != AgentWaitStatus::Pending) {
            normalize_agent_wait_result(&out);
        }
        return out;
    } catch (const std::exception& e) {
        rw_log()->warn("run_wait: agent.wait failed for {}: {}", run_id, e.what());
        out.status = AgentWaitStatus::Error;
        out.error = e.what();
        return out;
    }
}

AssistantReplySnapshot read_latest_assistant_reply_snapshot(
    std::shared_ptr<gateway::GatewayClient> gw,
    const std::string& session_key,
    int limit)
{
    AssistantReplySnapshot snap;
    if (!gw) return snap;
    try {
        // 対齐:341-346 —— limit 缺省 50
        auto result = gw->request<nlohmann::json>("chat.history", nlohmann::json{
            {"sessionKey", session_key},
            {"limit", limit}
        });
        if (!result.is_ok()) return snap;
        const auto msgs = result.get().value("messages", nlohmann::json::array());
        return resolve_latest_assistant_reply_snapshot(msgs);
    } catch (const std::exception& e) {
        rw_log()->warn("run_wait: chat.history failed for {}: {}",
                       session_key, e.what());
    }
    return snap;
}

WaitAndReadResult wait_for_agent_run_and_read_updated_assistant_reply(
    std::shared_ptr<gateway::GatewayClient> gw,
    const std::string& run_id,
    const std::string& session_key,
    int timeout_ms,
    const AssistantReplySnapshot& baseline)
{
    WaitAndReadResult out;
    // 対齐:412-416
    out.wait = wait_for_agent_run(gw, run_id, timeout_ms);
    // 対齐:417-419 —— 非 ok 直接返回，不去读回复
    if (out.wait.status != AgentWaitStatus::Ok) return out;

    // 対齐:421-429 —— 读快照并与 baseline 比对，未更新则 reply_text 留空
    const AssistantReplySnapshot latest =
        read_latest_assistant_reply_snapshot(gw, session_key);
    if (has_updated_assistant_reply_snapshot(latest, baseline)) {
        out.reply_text = latest.text;
    }
    return out;
}

}  // namespace agents
}  // namespace hunterclaw
