// ============================================================================
// acp_translator.cc
// 🎯 严格对齐 OpenClaw: src/acp/translator.ts (AcpGatewayAgent — 1300+ lines)
// ACP 消息翻译层: Gateway ↔ ACP SDK 双向转换
// ============================================================================

#include "hunterclaw/acp/acp_translator.h"
#include "hunterclaw/acp/acp_commands.h"
#include "hunterclaw/acp/acp_event_mapper.h"
#include "hunterclaw/acp/acp_stdio_transport.h"
#include "hunterclaw/acp/runtime_errors.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>

namespace hunterclaw {
namespace acp {

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr size_t MAX_PROMPT_BYTES = 2 * 1024 * 1024;
static const std::string CFG_THOUGHT_LEVEL = "thought_level";
static const std::string CFG_FAST_MODE     = "fast_mode";
static const std::string CFG_VERBOSE_LEVEL = "verbose_level";
static const std::string CFG_REASONING     = "reasoning_level";
static const std::string CFG_USAGE         = "response_usage";
static const std::string CFG_ELEVATED      = "elevated_level";
static constexpr int REPLAY_LIMIT = 1000000;
static constexpr int RATE_DEFAULT_MAX = 120;
static constexpr int64_t RATE_DEFAULT_WINDOW_MS = 10000;

// ── Inline rate limiter ──────────────────────────────────────────────────────
namespace {
class SimpleRateLimiter {
public:
    SimpleRateLimiter(int max_req, int64_t window_ms)
        : max_(max_req), window_(window_ms) {}
    bool try_acquire() {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (ms - start_ >= window_) { start_ = ms; count_ = 0; }
        return count_++ < max_;
    }
private:
    int max_; int64_t window_; int64_t start_ = 0; int count_ = 0;
};

struct PendingToolCallEntry {
    std::string kind;
    std::vector<ToolCallLocation> locations;
    nlohmann::json raw_input;
    std::string title;
};

struct PendingPromptEntry {
    std::string session_id;
    std::string session_key;
  std::string idempotency_key;
    bool send_accepted = false;
    std::string sent_text;
    std::string sent_thought;
    std::map<std::string, PendingToolCallEntry> tool_calls;
};

// 🎯 对齐 OC PendingApprovalRelay（translator.ts:158-164）。
struct PendingApprovalRelay {
    std::string approval_id;
    std::string session_id;
    std::string session_key;
    bool active = true;   // active → completed
};

// 🎯 对齐 OC loadSession/resumeSession 的 transcript replay。
// OC 从 gateway 拉取 transcript/ledger 后逐条转 replay chunk。
// HunterClaw 当前无本地 transcript 数据源（gateway 事件流即时消费），
// 故返回空集合：结构与推送路径对齐，待 runtime 接入 transcript 后填充。
inline std::vector<ReplayChunk> r_replay_chunks_for(const AcpSession& /*session*/) {
    return {};
}
} // anonymous namespace

// ── Impl ─────────────────────────────────────────────────────────────────────
struct AcpGatewayAgent::Impl {
    AcpServerOptions opts;
    AcpSessionStore* session_store = nullptr;
    std::mutex mu;
    std::map<std::string, PendingPromptEntry> pending;
    int disconnect_gen = 0;
    bool gw_connected = false;
    std::unique_ptr<SimpleRateLimiter> rate_limiter;
    AcpAgentInfo info{"hunterclaw-acp", "HunterClaw ACP Gateway", HUNTERCLAW_VERSION};
    // 🎯 stdio 连接（session/update 回推 + request_permission）
AcpStdioConnection* connection = nullptr;
    // 🎯 注入的派发回调（接 gateway，避免循环依赖）
    PromptDispatchFn prompt_dispatch;
    CancelDispatchFn cancel_dispatch;
    // 🎯 approval 中继回调（接 gateway exec.approval.get/resolve）
    ApprovalDetailsFn approval_details;
    ApprovalResolveFn approval_resolve;
    // 🎯 对齐 OC approvalRelays（approvalId → relay）
    std::map<std::string, PendingApprovalRelay> approval_relays;
};

AcpGatewayAgent::AcpGatewayAgent(gateway::GatewayClient* /*gw*/,
   const AcpServerOptions& opts,
      AcpSessionStore* store)
    : impl_(std::make_unique<Impl>()) {
    impl_->opts = opts;
    impl_->session_store = store;
    int mr = opts.session_create_rate_limit_max_requests > 0
        ? opts.session_create_rate_limit_max_requests : RATE_DEFAULT_MAX;
    int64_t wm = opts.session_create_rate_limit_window_ms > 0
        ? opts.session_create_rate_limit_window_ms : RATE_DEFAULT_WINDOW_MS;
    impl_->rate_limiter = std::make_unique<SimpleRateLimiter>(mr, wm);
}

AcpGatewayAgent::~AcpGatewayAgent() = default;

void AcpGatewayAgent::set_connection(AcpStdioConnection* conn) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->connection = conn;
}

void AcpGatewayAgent::set_prompt_dispatch(PromptDispatchFn fn) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->prompt_dispatch = std::move(fn);
}

void AcpGatewayAgent::set_cancel_dispatch(CancelDispatchFn fn) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->cancel_dispatch = std::move(fn);
}

void AcpGatewayAgent::set_approval_details(ApprovalDetailsFn fn) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->approval_details = std::move(fn);
}

void AcpGatewayAgent::set_approval_resolve(ApprovalResolveFn fn) {
 std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->approval_resolve = std::move(fn);
}

// 🎯 对齐 OC: sessionUpdates.emit — 发 session/update notification
void AcpGatewayAgent::emit_session_update(const std::string& session_id,
 const nlohmann::json& update) {
    AcpStdioConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
      conn = impl_->connection;
    }
    if (!conn) return;
    conn->send_notification("session/update", nlohmann::json{
        {"sessionId", session_id},
        {"update", update},
    });
}

// 🎯 对齐 OC sendAvailableCommands: new/load/resume 后发 available_commands_update
void AcpGatewayAgent::emit_available_commands(const std::string& session_id) {
    nlohmann::json cmds = nlohmann::json::array();
    for (auto& c : get_available_commands_vector()) cmds.push_back(c);
    emit_session_update(session_id, nlohmann::json{
     {"sessionUpdate", "available_commands_update"},
        {"availableCommands", cmds},
    });
}

// ── handleInitialize ─────────────────────────────────────────────────────────
// ✅ 严格对齐 OC translator.ts:503-524 initialize response
InitializeResponse AcpGatewayAgent::handle_initialize(const InitializeRequest& /*req*/) {
    InitializeResponse r;
    r.protocol_version = ACP_PROTOCOL_VERSION;  // integer = 1
    r.agent = impl_->info;                        // agentInfo
    // agentCapabilities 默认值（loadSession=true, prompt image/embedded=true）
    // HunterClaw 目前不支持 audio/mcp-http/mcp-sse，与 OC 保持一致
    r.agent_capabilities = AcpAgentCapabilities{};
    r.auth_methods = {};                          // authMethods: [] (对齐 OC)
    r.commands = get_available_commands_vector();        // 兼容旧 HC 字段（OC 通过独立 sendAvailableCommands RPC 发送）
    return r;
}

// ── handleAuthenticate ───────────────────────────────────────────────────────
AuthenticateResponse AcpGatewayAgent::handle_authenticate(const AuthenticateRequest& /*req*/) {
    return AuthenticateResponse{true};
}

// ── handleNewSession ─────────────────────────────────────────────────────────
NewSessionResponse AcpGatewayAgent::handle_new_session(const NewSessionRequest& req) {
    if (!impl_->rate_limiter->try_acquire())
        throw AcpRuntimeError(AcpErrorCode::ACP_TURN_FAILED,
                              "Session creation rate limit exceeded");

    auto meta = parse_session_meta(req.meta);
    std::string sk = meta.session_key.value_or(
        impl_->opts.default_session_key.value_or("default"));
    std::string cwd = req.cwd.empty() ? "." : req.cwd;
    auto session = impl_->session_store->create_session(sk, cwd);

    auto pres = build_session_presentation_default();
    NewSessionResponse r;
    r.session_id = session.session_id;
    r.config_options = pres.config_options;
    r.modes = pres.modes;
    // 🎯 对齐 OC: newSession 后推送 available_commands_update
    emit_available_commands(session.session_id);
    return r;
}

// ── handleListSessions ───────────────────────────────────────────────────────
ListSessionsResponse AcpGatewayAgent::handle_list_sessions(const ListSessionsRequest& /*req*/) {
    // Gateway interaction deferred to runtime wiring
    return ListSessionsResponse{};
}

// ── handleLoadSession ────────────────────────────────────────────────────────
// 🎯 严格对齐 OC loadSession: 返回 { configOptions, modes }（不含 sessionId），
//   replay 通过 session/update 逐条推送（user/agent message/thought chunk），
//   随后推送 available_commands_update。
LoadSessionResponse AcpGatewayAgent::handle_load_session(const LoadSessionRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session)
 throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
     "Session not found: " + req.session_id);
    auto pres = build_session_presentation_default();

    // 🎯 对齐 OC: replay 通过 session/update 流式推送，而非塞进响应。
    if (req.replay) {
        for (auto& c : r_replay_chunks_for(*session)) {
          emit_session_update(session->session_id, nlohmann::json{
     {"sessionUpdate", c.session_update},
      {"content", {{"type", "text"}, {"text", c.text}}},
   });
    }
    }
    // 🎯 对齐 OC: load 后推送 available_commands_update
    emit_available_commands(session->session_id);

    LoadSessionResponse r;
    r.session_id = session->session_id;  // 保留字段但 stdio_server 不再回传
    r.config_options = pres.config_options;
    r.modes = pres.modes;
    return r;
}

// ── handleResumeSession ──────────────────────────────────────────────────────
// 🎯 严格对齐 OC resumeSession: 与 load 类似，返回 { configOptions, modes }，
//   replay 通过 session/update 推送，随后推送 available_commands_update。
ResumeSessionResponse AcpGatewayAgent::handle_resume_session(const ResumeSessionRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session)
     throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
      "Session not found: " + req.session_id);
    auto pres = build_session_presentation_default();

    if (req.replay) {
        for (auto& c : r_replay_chunks_for(*session)) {
   emit_session_update(session->session_id, nlohmann::json{
 {"sessionUpdate", c.session_update},
   {"content", {{"type", "text"}, {"text", c.text}}},
     });
        }
}
    emit_available_commands(session->session_id);

    ResumeSessionResponse r;
    r.config_options = pres.config_options;
    r.modes = pres.modes;
    return r;
}

// ── handleCloseSession ───────────────────────────────────────────────────────
// 🎯 严格对齐 OC closeSession: cancel 进行中的 run 并释放 session，返回 {}。
CloseSessionResponse AcpGatewayAgent::handle_close_session(const CloseSessionRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session) {
  // OC: close 未知 session 视为幂等无操作。
        return CloseSessionResponse{};
    }
    // 先 cancel 正在进行的 run（对齐 OC 的 cancel + delete）。
    handle_cancel(req.session_id);
    // 清理 pending / approval relay 状态。
    {
     std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->pending.erase(req.session_id);
        for (auto it = impl_->approval_relays.begin(); it != impl_->approval_relays.end();) {
     if (it->second.session_id == req.session_id) it = impl_->approval_relays.erase(it);
    else ++it;
        }
    }
    return CloseSessionResponse{};
}

// ── handlePrompt ─────────────────────────────────────────────────────────────
PromptResponse AcpGatewayAgent::handle_prompt(const PromptRequest& req,
  PromptStreamCallback cb) {
    size_t bytes = req.text.size();
    for (auto& a : req.attachments) bytes += a.content.size();
    if (bytes > MAX_PROMPT_BYTES)
        throw AcpRuntimeError(AcpErrorCode::ACP_TURN_FAILED,
                              "Prompt exceeds maximum size of 2MB");

    auto session = impl_->session_store->get_session(req.session_id);
    if (!session)
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                              "Session not found: " + req.session_id);

  // Register pending
    PromptDispatchFn dispatch;
    {
  std::lock_guard<std::mutex> lk(impl_->mu);
     PendingPromptEntry pe;
        pe.session_id = req.session_id;
        pe.session_key = session->session_key;
        pe.idempotency_key = req.idempotency_key;
        impl_->pending[req.session_id] = std::move(pe);
        dispatch = impl_->prompt_dispatch;
    }

    nlohmann::json atts = nlohmann::json::array();
    for (auto& a : req.attachments)
        atts.push_back({{"type", a.type}, {"mimeType", a.mime_type}, {"content", a.content}});

    if (!dispatch) {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->pending.erase(req.session_id);
        PromptResponse r0;
        r0.stop_reason = "end_turn";
        return r0;
    }

    ChatResult cr;
    try {
        cr = dispatch(session->session_key, req.text, atts, req.idempotency_key);
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
   impl_->pending.erase(req.session_id);
        }
        throw AcpRuntimeError(AcpErrorCode::ACP_TURN_FAILED,
       std::string("prompt dispatch failed: ") + e.what());
    }

 if (!cr.run_id.empty())
        impl_->session_store->set_active_run(req.session_id, cr.run_id);
    if (cb && !cr.text.empty()) cb("agent_message_chunk", cr.text);
 {
std::lock_guard<std::mutex> lk(impl_->mu);
      impl_->pending.erase(req.session_id);
   // 🎯 对齐 OC clearApprovalRelaysForPrompt: prompt 结束清理该 session 的中继。
   for (auto it = impl_->approval_relays.begin(); it != impl_->approval_relays.end();) {
     if (it->second.session_id == req.session_id) it = impl_->approval_relays.erase(it);
   else ++it;
   }
    }
    impl_->session_store->clear_active_run(req.session_id);

    PromptResponse r;
    r.stop_reason = map_stop_reason(cr.stop_reason.empty() ? "end_turn" : cr.stop_reason);
    r.text = cr.text;
    return r;
}

// ── handleCancel (session/cancel notification, 对齐 OC cancel) ────────────────
void AcpGatewayAgent::handle_cancel(const std::string& session_id) {
    auto session = impl_->session_store->get_session(session_id);
    if (!session) return;
CancelDispatchFn cancel;
 std::string run_id;
    {
 std::lock_guard<std::mutex> lk(impl_->mu);
   cancel = impl_->cancel_dispatch;
        run_id = session->active_run_id;
    }
    impl_->session_store->cancel_active_run(session_id);
    if (cancel) {
        try { cancel(session->session_key, run_id); } catch (...) {}
    }
}

// ── handleGatewayEvent ───────────────────────────────────────────────────────
// 归一化 gateway 事件：兼容 EventFrame({event,seq,payload}) 与扁平 json。
//   若为 EventFrame 形态（含 event/payload），把 payload 提升为顶层字段，
//   并把 event 作为回退 type（对齐 OC handleGatewayEvent 先看 evt.event）。
void AcpGatewayAgent::handle_gateway_event(const nlohmann::json& evt_in) {
    nlohmann::json evt = evt_in;
    if (evt_in.contains("payload") && evt_in["payload"].is_object()) {
      evt = evt_in["payload"];
   // 保留顶层 event 作为 type/stream 判定的回退来源。
        if (!evt.contains("event") && evt_in.contains("event")) evt["event"] = evt_in["event"];
    }
    std::string sk = evt.value("sessionKey", "");
    std::string type = evt.value("type", "");
    // OC: evt.event === "exec.approval.requested" / "chat" 等；此处作为 type 回退。
    if (type.empty()) type = evt.value("event", "");

    // 找到匹配的 pending session（避免持锁期间回调 connection 造成死锁：
    // 先在锁内更新累积状态并收集要 emit 的 update，再解锁后发送）。
    std::string target_sid;
    std::vector<nlohmann::json> updates;  // 待发送的 session/update.update
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& [sid, pe] : impl_->pending) {
 if (pe.session_key != sk) continue;
        target_sid = sid;
            if (type == "agent_text") {
 std::string t = evt.value("text", "");
     pe.sent_text += t;
                if (!t.empty())
     updates.push_back({{"sessionUpdate", "agent_message_chunk"},
      {"content", {{"type", "text"}, {"text", t}}}});
       } else if (type == "agent_thought") {
  std::string t = evt.value("text", "");
      pe.sent_thought += t;
 if (!t.empty())
    updates.push_back({{"sessionUpdate", "agent_thought_chunk"},
    {"content", {{"type", "text"}, {"text", t}}}});
     } else if (type == "tool_call_start") {
      PendingToolCallEntry tc;
   std::string tn = evt.value("toolName", "");
        std::string tcid = evt.value("toolCallId", "");
    tc.kind = infer_tool_kind(tn);
      tc.raw_input = evt.value("toolInput", nlohmann::json{});
   tc.title = format_tool_title(tn, tc.raw_input);
   tc.locations = extract_tool_call_locations(tc.raw_input);
   // 🎯 对齐 OC: sessionUpdate=tool_call, status=in_progress
    nlohmann::json locs = nlohmann::json::array();
     for (auto& l : tc.locations) {
        nlohmann::json lj{{"path", l.path}};
     if (l.line > 0) lj["line"] = l.line;
      locs.push_back(std::move(lj));
        }
     nlohmann::json u{{"sessionUpdate", "tool_call"},
       {"toolCallId", tcid},
 {"title", tc.title},
        {"kind", tc.kind},
             {"status", "in_progress"},
   {"rawInput", tc.raw_input}};
      if (!locs.empty()) u["locations"] = locs;
      updates.push_back(std::move(u));
     pe.tool_calls[tcid] = std::move(tc);
      } else if (type == "tool_call_update") {
       // 🎯 对齐 OC handleAgentEvent phase "update": 中间态进度。
     //   sessionUpdate=tool_call_update, status=in_progress, rawOutput=partialResult,
     //   content + locations（每次 update 重新携带）。
     std::string tcid = evt.value("toolCallId", "");
        nlohmann::json partial = evt.contains("partialResult") ? evt["partialResult"]
     : evt.value("toolResult", nlohmann::json{});
     nlohmann::json u{{"sessionUpdate", "tool_call_update"},
  {"toolCallId", tcid},
   {"status", "in_progress"}};
   if (!partial.is_null()) u["rawOutput"] = partial;
     // 结构化 content（record/text 兼容）
       if (partial.is_string() && !partial.get<std::string>().empty()) {
     u["content"] = nlohmann::json::array({
 nlohmann::json{{"type", "content"},
     {"content", {{"type", "text"}, {"text", partial.get<std::string>()}}}}});
    }
     if (auto it = pe.tool_calls.find(tcid); it != pe.tool_calls.end() && !it->second.locations.empty()) {
 nlohmann::json locs = nlohmann::json::array();
   for (auto& l : it->second.locations) {
    nlohmann::json lj{{"path", l.path}};
        if (l.line > 0) lj["line"] = l.line;
     locs.push_back(std::move(lj));
   }
   u["locations"] = locs;
     }
     updates.push_back(std::move(u));
   } else if (type == "tool_call_end") {
 std::string tcid = evt.value("toolCallId", "");
      // 🎯 对齐 OC: rawOutput 传原始结构化 result（任意 JSON），非纯 string。
    nlohmann::json raw_result = evt.contains("result") ? evt["result"]
  : evt.value("toolResult", nlohmann::json{});
     std::string result_text = raw_result.is_string() ? raw_result.get<std::string>() : "";
     bool is_error = evt.value("isError", false);
       if (auto it = pe.tool_calls.find(tcid); it != pe.tool_calls.end()) {
      auto rlocs = extract_tool_call_locations_from_result(result_text);
         for (auto& l : rlocs) it->second.locations.push_back(std::move(l));
     }
   // 🎯 对齐 OC: sessionUpdate=tool_call_update, status=completed|failed
          nlohmann::json u{{"sessionUpdate", "tool_call_update"},
    {"toolCallId", tcid},
   {"status", is_error ? "failed" : "completed"}};
   if (!raw_result.is_null()) u["rawOutput"] = raw_result;
     // 🎯 对齐 OC extractToolCallContent: 优先结构化 content blocks，回退 text。
   if (raw_result.is_array()) {
      u["content"] = raw_result;  // 已是 ToolCallContent[] 形态
     } else if (raw_result.is_object() && raw_result.contains("content")) {
        u["content"] = nlohmann::json::array({raw_result});
     } else if (!result_text.empty()) {
     u["content"] = nlohmann::json::array({
        nlohmann::json{{"type", "content"},
 {"content", {{"type", "text"}, {"text", result_text}}}}});
     }
   // 🎯 对齐 OC: end 事件携带 locations
     if (auto it = pe.tool_calls.find(tcid); it != pe.tool_calls.end() && !it->second.locations.empty()) {
      nlohmann::json locs = nlohmann::json::array();
   for (auto& l : it->second.locations) {
       nlohmann::json lj{{"path", l.path}};
   if (l.line > 0) lj["line"] = l.line;
   locs.push_back(std::move(lj));
 }
    u["locations"] = locs;
   }
  updates.push_back(std::move(u));
  } else if (type == "run_accepted") {
       pe.send_accepted = true;
      }
 break;
        }
    }

    // 解锁后发送 session/update notification（避免死锁）。
    if (!target_sid.empty()) {
        for (auto& u : updates) emit_session_update(target_sid, u);
    }

  // 🎯 对齐 OC: exec approval 事件 → 启动 session/request_permission 中继。
    //   OC 触发点: stream==="approval"（handleApprovalEvent）
    //   或 event==="exec.approval.requested"（handleExecApprovalRequestEvent）。
    {
   std::string stream = evt.value("stream", "");
   bool is_approval = (type == "approval") || (stream == "approval") ||
      (type == "exec.approval.requested");
   // 兼容结构化 payload（data / request 嵌套）。
        if (is_approval) {
       const nlohmann::json* data = &evt;
      if (evt.contains("data") && evt["data"].is_object()) data = &evt["data"];
         // 解析 approvalId（对齐 OC parseGatewayExecApprovalEventData:
       // phase=="requested" && kind=="exec" && status=="pending"）。
     std::string phase = data->value("phase", "");
  std::string kind = data->value("kind", "");
    std::string status = data->value("status", "");
       std::string approval_id = data->value("approvalId", "");
       // 结构化 request payload 形态（parseGatewayExecApprovalRequestEventPayload）。
        if (approval_id.empty() && evt.contains("request") && evt["request"].is_object()) {
      approval_id = evt.value("id", "");
    }
      bool legacy_ok = (phase == "requested" && kind == "exec" && status == "pending" && !approval_id.empty());
     bool payload_ok = (!approval_id.empty() && evt.contains("request"));
        if (legacy_ok || payload_ok) {
      GatewayApprovalEvent ae;
    ae.approval_id = approval_id;
     ae.command = data->value("command", "");
       ae.host = data->value("host", "");
     ae.title = data->value("title", "");
     ae.tool_call_id = data->value("toolCallId", "");
       std::string run_id = evt.value("runId", "");
      start_approval_relay(sk, run_id, ae);
        }
        }
    }
}

// ── startApprovalRelay / runApprovalRelay ────────────────────────────────────
// 🎯 严格对齐 OC startApprovalRelay（translator.ts:969）+ runApprovalRelay（1031）。
void AcpGatewayAgent::start_approval_relay(const std::string& session_key,
    const std::string& /*run_id*/,
   const GatewayApprovalEvent& event) {
    std::string target_sid;
    std::string title = event.title;
    {
      std::lock_guard<std::mutex> lk(impl_->mu);
        // 去重：同一 approvalId 只中继一次（对齐 OC approvalRelays.has）。
if (impl_->approval_relays.count(event.approval_id)) return;
        // 找到匹配该 session_key 的 pending prompt（对齐 OC findPendingForApproval）。
        for (auto& [sid, pe] : impl_->pending) {
if (pe.session_key != session_key) continue;
       target_sid = sid;
   // 关联 tool call title（对齐 OC correlatedApprovalEvent）。
      if (title.empty() && !event.tool_call_id.empty()) {
     auto it = pe.tool_calls.find(event.tool_call_id);
   if (it != pe.tool_calls.end() && it->second.kind == "execute")
      title = it->second.title;
   }
    break;
  }
   if (target_sid.empty()) return;  // 无匹配 pending，忽略（对齐 OC）。
        PendingApprovalRelay relay;
   relay.approval_id = event.approval_id;
        relay.session_id = target_sid;
 relay.session_key = session_key;
        relay.active = true;
        impl_->approval_relays[event.approval_id] = relay;
    }
    GatewayApprovalEvent correlated = event;
    correlated.title = title;
    // 在独立线程运行中继（send_request 会阻塞等待 client 响应）。
    std::thread([this, target_sid, correlated]() {
 run_approval_relay(target_sid, correlated.approval_id, correlated);
    }).detach();
}

void AcpGatewayAgent::run_approval_relay(const std::string& session_id,
     const std::string& approval_id,
     const GatewayApprovalEvent& event) {
    ApprovalDetailsFn details_fn;
    ApprovalResolveFn resolve_fn;
    AcpStdioConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
      details_fn = impl_->approval_details;
      resolve_fn = impl_->approval_resolve;
        conn = impl_->connection;
}

    // 1) 查询 approval 详情（对齐 OC getGatewayApprovalDetails: exec.approval.get）。
    nlohmann::json details;
    if (details_fn) {
   try { details = details_fn(approval_id); } catch (...) {}
  }
    if (!details.is_object()) details = nlohmann::json::object();

    // 2) 解析允许的决策集合（对齐 OC normalizeGatewayExecApprovalDecisions）。
    std::vector<std::string> decisions;
    if (details.contains("allowedDecisions") && details["allowedDecisions"].is_array()) {
        for (auto& d : details["allowedDecisions"]) {
   if (d.is_string()) {
       std::string s = d.get<std::string>();
    if (s == "allow-once" || s == "allow-always" || s == "deny")
       decisions.push_back(s);
    }
        }
    }
    if (decisions.empty()) { decisions = {"allow-once", "deny"}; }  // OC FALLBACK

    // 3) 构造 PermissionOption[]（对齐 OC buildAcpPermissionOptions）。
    nlohmann::json options = nlohmann::json::array();
    auto has = [&](const std::string& d) {
        return std::find(decisions.begin(), decisions.end(), d) != decisions.end();
    };
    if (has("allow-once"))
        options.push_back({{"optionId", "allow-once"}, {"name", "Allow once"}, {"kind", "allow_once"}});
 if (has("allow-always"))
    options.push_back({{"optionId", "allow-always"}, {"name", "Allow always"}, {"kind", "allow_always"}});
    if (has("deny"))
        options.push_back({{"optionId", "deny"}, {"name", "Deny"}, {"kind", "reject_once"}});

    // 4) 构造 request_permission 请求（对齐 OC buildAcpPermissionRequest）。
    std::string command = details.value("commandText",
        details.value("commandPreview", event.command));
    std::string host = details.value("host", event.host);
    nlohmann::json raw_input{{"name", "exec"}, {"approvalId", approval_id}};
    if (!command.empty()) raw_input["command"] = command;
    if (!host.empty()) raw_input["host"] = host;
    std::string tool_call_id = event.tool_call_id.empty()
        ? ("exec:" + approval_id) : event.tool_call_id;
    nlohmann::json request{
        {"sessionId", session_id},
 {"toolCall", {
       {"toolCallId", tool_call_id},
            {"title", event.title.empty() ? "Command approval requested" : event.title},
        {"kind", "execute"},
  {"status", "pending"},
   {"rawInput", raw_input},
    {"_meta", {{"toolName", "exec"}, {"approvalId", approval_id}}},
     }},
   {"options", options},
    };

    // 5) server→client 发起 session/request_permission，等待决策。
    std::string decision = "deny";  // OC 默认拒绝
    if (conn) {
      nlohmann::json resp = conn->send_request("session/request_permission", request);
        // 对齐 OC resolveGatewayDecisionFromPermissionOutcome:
     //   outcome.outcome === "selected" → optionId。
        if (resp.is_object() && !resp.value("__acp_error", false)) {
      nlohmann::json outcome = resp.value("outcome", nlohmann::json{});
       if (outcome.is_object() && outcome.value("outcome", "") == "selected") {
       std::string opt = outcome.value("optionId", "");
   if (opt == "allow-once" || opt == "allow-always" || opt == "deny")
     decision = opt;
        }
  }
    }

    // 6) 中继仍活跃才采用 client 决策，否则 deny（对齐 OC isApprovalRelayActive）。
bool still_active = false;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->approval_relays.find(approval_id);
      still_active = (it != impl_->approval_relays.end() && it->second.active);
    }
    std::string final_decision = still_active ? decision : "deny";

    // 7) 回写 Gateway（对齐 OC resolveGatewayApproval: exec.approval.resolve）。
    if (resolve_fn) {
        try { resolve_fn(approval_id, final_decision); } catch (...) {}
    }

    // 8) 标记完成（保留至 prompt 清理，作为去重哨兵；对齐 OC finally）。
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto it = impl_->approval_relays.find(approval_id);
        if (it != impl_->approval_relays.end()) it->second.active = false;
    }
}

// ── handleGatewayReconnect / Disconnect ──────────────────────────────────────
void AcpGatewayAgent::handle_gateway_reconnect() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->gw_connected = true;
    impl_->disconnect_gen++;
}

void AcpGatewayAgent::handle_gateway_disconnect(const std::string& /*reason*/) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->gw_connected = false;
    impl_->disconnect_gen++;
}

// ── handleSetSessionMode ─────────────────────────────────────────────────────
SetSessionModeResponse AcpGatewayAgent::handle_set_session_mode(
    const SetSessionModeRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session)
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED, "Session not found");
    auto pres = build_session_presentation_default();
    return SetSessionModeResponse{pres.modes, pres.config_options};
}

// ── handleSetSessionConfigOption ─────────────────────────────────────────────
SetSessionConfigOptionResponse AcpGatewayAgent::handle_set_session_config_option(
    const SetSessionConfigOptionRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session)
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED, "Session not found");
    if (req.option_id != CFG_THOUGHT_LEVEL && req.option_id != CFG_FAST_MODE &&
        req.option_id != CFG_VERBOSE_LEVEL && req.option_id != CFG_REASONING &&
        req.option_id != CFG_USAGE && req.option_id != CFG_ELEVATED)
        throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
                              "Unknown config option: " + req.option_id);
    auto pres = build_session_presentation_default();
    return SetSessionConfigOptionResponse{pres.config_options, pres.modes};
}

// ── buildSessionPresentation (default, no gateway query) ─────────────────────
SessionPresentation AcpGatewayAgent::build_session_presentation_default() {
    SessionPresentation p;
    std::string cm = "adaptive";
    std::vector<std::string> lvls = {"adaptive","off","minimal","low","medium","high","xhigh"};
    p.modes.current_mode_id = cm;
    for (auto& l : lvls) {
        SessionMode m;
        m.id = l;
        m.name = format_thinking_level_name(l);
        m.description = build_thinking_mode_description(l);
        p.modes.available_modes.push_back(std::move(m));
    }
    p.config_options.push_back(build_select_config_option(CFG_THOUGHT_LEVEL,
        "Thought level", "thought_level",
        "Controls deliberate reasoning level.", cm, lvls));
    p.config_options.push_back(build_select_config_option(CFG_FAST_MODE,
        "Fast mode", "", "Controls fast-mode profile.", "off", {"off","on"}));
    p.config_options.push_back(build_select_config_option(CFG_VERBOSE_LEVEL,
        "Tool verbosity", "", "Controls tool progress detail.", "off", {"off","on","full"}));
    p.config_options.push_back(build_select_config_option(CFG_REASONING,
        "Reasoning stream", "", "Controls reasoning text.", "off", {"off","on","stream"}));
    p.config_options.push_back(build_select_config_option(CFG_USAGE,
        "Usage detail", "", "Controls usage info.", "off", {"off","tokens","full"}));
    p.config_options.push_back(build_select_config_option(CFG_ELEVATED,
        "Elevated actions", "", "Controls elevated execution.", "off", {"off","on","ask","full"}));
    return p;
}

SessionConfigOption AcpGatewayAgent::build_select_config_option(
    const std::string& id, const std::string& name, const std::string& cat,
    const std::string& desc, const std::string& cur,
    const std::vector<std::string>& vals) {
    SessionConfigOption o;
    o.type = "select"; o.id = id; o.name = name; o.category = cat;
    o.description = desc; o.current_value = cur;
    for (auto& v : vals) o.options.push_back({v, format_config_value_name(v)});
    return o;
}

std::string AcpGatewayAgent::format_thinking_level_name(const std::string& l) {
    if (l == "xhigh") return "Extra High";
    if (l == "adaptive") return "Adaptive";
    if (l.empty()) return "Unknown";
    std::string r = l; r[0] = static_cast<char>(toupper(r[0])); return r;
}

std::string AcpGatewayAgent::build_thinking_mode_description(const std::string& l) {
    return l == "adaptive" ? "Use the Gateway session default thought level." : "";
}

std::string AcpGatewayAgent::format_config_value_name(const std::string& v) {
    if (v == "xhigh") return "Extra High";
    if (v.empty()) return "Unknown";
    std::string r = v; r[0] = static_cast<char>(toupper(r[0])); return r;
}

// ✅ 严格对齐 OC translator.ts:957-971:
//   Gateway → ACP StopReason 映射。ACP spec 枚举（snake_case）:
//     "end_turn" | "max_tokens" | "cancelled" | "refusal"
//
// OC 逻辑:
//   state === "final"   → rawStopReason === "max_tokens" ? "max_tokens" : "end_turn"
//   state === "aborted" → "cancelled"
//   state === "error"   → errorKind === "refusal" ? "refusal" : "end_turn"
//
// 此 helper 用于 Gateway 事件（chat.state / chat.stopReason）→ ACP StopReason 转换
std::string AcpGatewayAgent::map_stop_reason(const std::string& g) {
    // aborted / cancelled
    if (g == "aborted" || g == "cancelled" || g == "abort") return "cancelled";
    // max_tokens
    if (g == "max_tokens" || g == "length") return "max_tokens";
    // refusal
    if (g == "refusal" || g == "content_filtered" || g == "content_filter") return "refusal";
    // end_turn (默认，对齐 OC 最终 fallback)
    //   OC: "stop", "end_turn", "endTurn" 全归为 "end_turn"
    //   tool_use 在 ACP spec 中不是合法 StopReason，按 OC 统一归到 "end_turn"
    return "end_turn";
}

std::vector<ReplayChunk> AcpGatewayAgent::extract_replay_chunks(
    const TranscriptMessage& msg) {
    std::vector<ReplayChunk> out;
    if (msg.role != "user" && msg.role != "assistant") return out;
    std::string ut = (msg.role == "user") ? "user_message_chunk" : "agent_message_chunk";
    if (!msg.text_content.empty()) out.push_back({ut, msg.text_content});
    for (auto& b : msg.content_blocks) {
        if (b.type == "thinking" && !b.thinking.empty())
            out.push_back({"agent_thought_chunk", b.thinking});
        else if (b.type == "text" && !b.text.empty())
            out.push_back({ut, b.text});
    }
    return out;
}

// 対齐 session-mapper.ts:15-27 parseSessionMeta
//   + packages/acp-core/src/meta.ts:4-35 readMetaValue / readString / readBool
//
// ⚠ 两处语义此前是错的：
//   ① readMetaValue（meta.ts:12-17）在归一化失败时**继续试下一个别名**，
//      只有成功才 return。此前用的 `if (contains && is_string) {...; break; }`
//      是"第一个存在的键就停"—— 若 meta 里 sessionKey 是空串或数字，
//      就再也不会去看 session / key，别名链直接断掉。
//   ② readString 走 normalizeOptionalString：**trim 后为空视为不存在**。
//      此前原样取值，`sessionKey: "   "` 会被当成合法 key 传下去。
// readBool 只认真正的 boolean（meta.ts:34），字符串 "true" 不算 —— 这点原实现是对的。
namespace {

/// 対齐 readString(meta, keys) —— 逐个别名试，trim 非空才采用
std::string read_meta_string(const nlohmann::json& meta,
                             const std::vector<std::string>& keys) {
    for (const auto& k : keys) {
        if (!meta.contains(k) || !meta[k].is_string()) continue;
        std::string v = meta[k].get<std::string>();
        const char* ws = " \t\r\n\f\v";
        const size_t b = v.find_first_not_of(ws);
        if (b == std::string::npos) continue;   // 空白串 → 视为不存在，继续下一别名
        const size_t e = v.find_last_not_of(ws);
        return v.substr(b, e - b + 1);
    }
    return std::string();
}

/// 対齐 readBool(meta, keys) —— 逐个别名试，仅接受真正的 boolean。
/// 用 optional 表达"未提供"，不能退化成 false：调用方需要区分
/// `resetSession: false`（显式关闭）与未提供（走 opts 默认值）。
std::optional<bool> read_meta_bool(const nlohmann::json& meta,
                                   const std::vector<std::string>& keys) {
    for (const auto& k : keys) {
        if (meta.contains(k) && meta[k].is_boolean()) return meta[k].get<bool>();
    }
    return std::nullopt;
}

}  // namespace

AcpSessionMeta_Request AcpGatewayAgent::parse_session_meta(const nlohmann::json& meta) {
    AcpSessionMeta_Request r;
    if (meta.is_null() || !meta.is_object()) return r;   // 対齐:16-18

    // 対齐:21-25 —— 键别名顺序即"当前名 → 历史名"优先级，不可调换。
    // session_key / session_label 是 optional<string>：空串须保持 nullopt，
    // 否则下游 `if (r.session_key)` 会对空串成真。
    if (auto s = read_meta_string(meta, {"sessionKey", "session", "key"}); !s.empty()) {
        r.session_key = s;
    }
    if (auto s = read_meta_string(meta, {"sessionLabel", "label"}); !s.empty()) {
        r.session_label = s;
    }
    if (auto v = read_meta_bool(meta, {"resetSession", "reset"})) {
        r.reset_session = *v;
    }
    if (auto v = read_meta_bool(meta, {"requireExistingSession", "requireExisting"})) {
        r.require_existing = *v;
    }
    // 対齐:25 —— prefixCwd 此前完全缺失，导致该开关在 ACP 侧永久失效
    if (auto v = read_meta_bool(meta, {"prefixCwd"})) {
        r.prefix_cwd = *v;
    }
    return r;
}

// ── getAvailableCommands ─────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: 命令定义统一在 acp_commands.h 中维护。
// 此处不再重复定义，调用方请使用 get_available_commands_vector().

} // namespace acp
} // namespace hunterclaw
