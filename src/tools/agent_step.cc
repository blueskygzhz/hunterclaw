// ============================================================================
// Agent Step 实现 — 严格对齐 OpenClaw agents/tools/agent-step.ts
// ============================================================================
// 対齐要点（逐条标注真源行号）：
//   * message 必须先经 annotateInterSessionPromptText 加安全信封（:79）
//   * channel 缺省 "webchat"（:81），lane 缺省 per-session（:80）
//   * runId 取响应 runId，缺失时回退 idempotencyKey（:119-120）
//   * wait 复用 waitForAgentRunAndReadUpdatedAssistantReply（:122-126）
//   * 仅 status==ok 才返回回复（:133-136）
// ============================================================================

#include "hunterclaw/tools/agent_step.h"
#include "hunterclaw/agents/run_wait.h"
#include "hunterclaw/sessions/input_provenance.h"

#include <algorithm>
#include <random>
#include <spdlog/spdlog.h>

namespace {
    std::shared_ptr<spdlog::logger> as_log() {
        static auto logger = spdlog::get("sessions.agent_step");
        if (!logger) logger = spdlog::stdout_logger_mt("sessions.agent_step");
        return logger;
    }
}

namespace hunterclaw {
namespace tools {

namespace {
    const std::string TOOL_NAME = "sessions_send";

    bool is_space_ch(unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }

    std::string trim(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && is_space_ch(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && is_space_ch(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    }

    /// 対齐 agent-step.ts:71 crypto.randomUUID()
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
}  // anonymous namespace

core::Result<std::string> run_agent_step(
    std::shared_ptr<gateway::GatewayClient> gw,
    const AgentStepParams& params)
{
    if (!gw) {
        return core::Result<std::string>::Err("Gateway not available");
    }
    if (params.session_key.empty()) {
        return core::Result<std::string>::Err("Session key is empty");
    }

    // 対齐:71 —— stepIdem 同时用作 idempotencyKey 与 runId 的回退值
    const std::string step_idem = generate_uuid_v4();

    // 対齐:72-77 inputProvenance
    sessions::InputProvenance provenance;
    provenance.kind = sessions::InputProvenanceKind::InterSession;
    provenance.source_session_key = params.source_session_key;
    provenance.source_channel = params.source_channel;
    provenance.source_tool = params.source_tool.empty() ? TOOL_NAME : params.source_tool;

    // 対齐:78-79 —— 必须给跨会话正文加安全信封，明确告知模型这不是本会话
    // 最终用户的直接指令。缺了这一步，A 会话用户可借 sessions_send
    // 让 B 会话把注入内容当作 B 的用户指令执行（prompt 注入 / 越权）。
    const std::string message =
        sessions::annotate_inter_session_prompt_text(params.message, provenance);

    // 対齐:80-81 —— lane 缺省 per-session（不是全局 "nested"，否则 A2A
    // ping-pong 两会话互等会自我死锁）；channel 缺省 "webchat"。
    const std::string lane = params.lane.empty()
        ? resolve_nested_agent_lane_for_session(params.session_key)
        : params.lane;
    const std::string channel = params.channel.empty()
        ? INTERNAL_MESSAGE_CHANNEL : params.channel;

    // 対齐:84-96 与 :103-117 —— 两条路径的参数集几乎一致，
    // 差别是 transcriptMessage 模式额外带 transcriptMessage/runId/allowModelOverride。
    nlohmann::json send_params = {
        {"message",       message},
        {"sessionKey",    params.session_key},
        {"deliver",       false},
        // 対齐:89,110 —— A2A 内部步骤的回复不得自动外发，
        // 仅允许 message tool 显式投递。
        {"sourceReplyDeliveryMode", "message_tool_only"},
        {"channel",       channel},
        {"lane",          lane},
        {"extraSystemPrompt", params.extra_system_prompt},
        {"inputProvenance", nlohmann::json{
            {"kind",             sessions::to_string(provenance.kind)},
            {"sourceSessionKey", provenance.source_session_key},
            {"sourceChannel",    provenance.source_channel},
            {"sourceTool",       provenance.source_tool}
        }}
    };

    if (params.transcript_message.has_value()) {
        // 対齐:82-96 —— transcriptMessage 模式：保留 transcript 文本。
        // 真源走 in-process 命令路径（agentCommandFromIngress）；HunterClaw
        // 无对应的进程内命令层，故仍走 gateway，但把 transcriptMessage 与
        // runId 一并透传，语义等价。
        send_params["transcriptMessage"] = *params.transcript_message;
        send_params["runId"] = step_idem;              // 対齐:92
        send_params["allowModelOverride"] = false;     // 対齐:95
    } else {
        send_params["idempotencyKey"] = step_idem;     // 対齐:108
    }

    // 発起 agent run
    std::string response_run_id;
    try {
        auto response = gw->request<nlohmann::json>("agent", send_params);
        if (!response.is_ok()) {
            return core::Result<std::string>::Err(
                "agent_step: agent command failed: " + response.error);
        }
        const auto& resp = response.get();
        // 対齐:119 —— 只有非空字符串才算有效 runId
        if (resp.contains("runId") && resp["runId"].is_string()) {
            response_run_id = resp["runId"].get<std::string>();
        }
        // gateway 侧同时可能以 snake_case 回传（见 agent.cc 的 run_id 字段）
        if (response_run_id.empty() && resp.contains("run_id") &&
            resp["run_id"].is_string()) {
            response_run_id = resp["run_id"].get<std::string>();
        }
    } catch (const std::exception& e) {
        return core::Result<std::string>::Err(
            std::string("agent_step: agent command exception: ") + e.what());
    }

    // 対齐:120 —— runId 缺失时回退 stepIdem。
    // 这一步不能省：gateway 的 agent 方法在 dedupe 命中等路径下可能不回
    // runId，此时用 idempotencyKey 仍能正确 wait（gateway 侧 runId = idem）。
    const std::string resolved_run_id =
        response_run_id.empty() ? step_idem : response_run_id;

    // 対齐:121-126 —— gateway 的 agent 调用可能在 assistant 回复落库前就返回，
    // 故必须 wait；且 wait 超时上限 60s。
    agents::AssistantReplySnapshot baseline;
    baseline.text = params.baseline_reply;
    const agents::WaitAndReadResult result =
        agents::wait_for_agent_run_and_read_updated_assistant_reply(
            gw, resolved_run_id, params.session_key,
            std::min(params.timeout_ms, 60000), baseline);

    // 対齐:133-135 —— 非 ok 一律视为无回复
    if (result.wait.status != agents::AgentWaitStatus::Ok) {
        as_log()->warn("agent_step: run not ok for {} (run={}): {}",
                       params.session_key, resolved_run_id, result.wait.error);
        return core::Result<std::string>::Err(
            "agent_step: run did not complete for " + params.session_key);
    }
    // 対齐:136 —— replyText 可能为空（回复未相对 baseline 更新）
    if (result.reply_text.empty()) {
        return core::Result<std::string>::Err(
            "agent_step: no updated reply from " + params.session_key);
    }
    return core::Result<std::string>::Ok(result.reply_text);
}

// 兼容旧调用点的薄封装。
core::Result<std::string> run_agent_step(
    std::shared_ptr<gateway::GatewayClient> gw,
    const std::string& session_key,
    const std::string& message,
    const std::string& extra_context,
    int timeout_ms)
{
    AgentStepParams params;
    params.session_key = session_key;
    params.message = message;
    params.extra_system_prompt = extra_context;
    params.timeout_ms = timeout_ms;
    return run_agent_step(gw, params);
}

}  // namespace tools
}  // namespace hunterclaw
