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
#include <cmath>
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
/// 対齐 infra/fixed-window-rate-limit.ts:33-74 createFixedWindowRateLimiter
///
/// ⚠ 此前的 SimpleRateLimiter 有四处偏差：
///   ① `return count_++ < max_;` —— 先比较后自增，且**拒绝时也会自增**，
///      导致窗口内被拒的请求继续累加计数；真源在 `count >= maxRequests` 时
///      直接 return，**不动 count**（:56-63）。
///   ② 无锁。ACP 的 stdio 请求与 gateway 事件回流在不同线程，
///      count_/start_ 存在数据竞争。
///   ③ 不返回 retryAfterMs，调用方无法按真源格式生成
///      "retry after Ns" 的错误文案（:1821-1823）。
///   ④ 初始 start_=0 配合 steady_clock：首次调用时 `ms - 0 >= window_`
///      恒真所以能重置，行为上侥幸正确，但语义脆弱。真源用 Date.now()
///      同样从 0 起，此处保留该语义并显式注明。
class FixedWindowRateLimiter {
public:
    struct Budget {
        bool allowed = false;
        int64_t retry_after_ms = 0;
        int remaining = 0;
    };

    FixedWindowRateLimiter(int max_req, int64_t window_ms)
        // 対齐:41-42 resolveFixedWindowRateLimitInteger(..., { min: 1 })
        : max_(max_req > 0 ? max_req : 1),
          window_(window_ms > 0 ? window_ms : 1) {}

    Budget consume() {
        std::lock_guard<std::mutex> lk(mu_);
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        // 対齐:51-55 —— 固定窗口：过期后第一个请求重置全部配额
        if (now_ms - window_start_ms_ >= window_) {
            window_start_ms_ = now_ms;
            count_ = 0;
        }
        // 対齐:56-63 —— 超限时**不自增** count
        if (count_ >= max_) {
            Budget b;
            b.allowed = false;
            // 対齐:60 —— Math.max(0, windowStart + window - now)
            b.retry_after_ms = std::max<int64_t>(0, window_start_ms_ + window_ - now_ms);
            b.remaining = 0;
            return b;
        }
        // 対齐:64-69
        count_ += 1;
        Budget b;
        b.allowed = true;
        b.retry_after_ms = 0;
        b.remaining = std::max(0, max_ - count_);
        return b;
    }

    /// 対齐:71-74 reset()
    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        count_ = 0;
        window_start_ms_ = 0;
    }

private:
    std::mutex mu_;
    int max_;
    int64_t window_;
    int64_t window_start_ms_ = 0;
    int count_ = 0;
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
    std::unique_ptr<FixedWindowRateLimiter> rate_limiter;
    AcpAgentInfo info{"hunterclaw-acp", "HunterClaw ACP Gateway", HUNTERCLAW_VERSION};
    // 🎯 stdio 连接（session/update 回推 + request_permission）
AcpStdioConnection* connection = nullptr;
    // 🎯 注入的派发回调（接 gateway，避免循环依赖）
    PromptDispatchFn prompt_dispatch;
    CancelDispatchFn cancel_dispatch;
    // 🎯 approval 中继回调（接 gateway exec.approval.get/resolve）
    ApprovalDetailsFn approval_details;
    ApprovalResolveFn approval_resolve;
    // 🎯 sessions.patch 回调（接 gateway，对齐 OC translator.ts:587/617）
    SessionPatchFn session_patch;
    // 🎯 对齐 OC approvalRelays（approvalId → relay）
    std::map<std::string, PendingApprovalRelay> approval_relays;
};

AcpGatewayAgent::AcpGatewayAgent(gateway::GatewayClient* /*gw*/,
   const AcpServerOptions& opts,
      AcpSessionStore* store)
    : impl_(std::make_shared<Impl>()) {
    impl_->opts = opts;
    impl_->session_store = store;
    int mr = opts.session_create_rate_limit_max_requests > 0
        ? opts.session_create_rate_limit_max_requests : RATE_DEFAULT_MAX;
    int64_t wm = opts.session_create_rate_limit_window_ms > 0
        ? opts.session_create_rate_limit_window_ms : RATE_DEFAULT_WINDOW_MS;
    impl_->rate_limiter = std::make_unique<FixedWindowRateLimiter>(mr, wm);
}

// ── 会话创建的两道闸（対齐 translator.ts:1806-1825）─────────────────────────
// ⚠ 此前 assert_supported_session_setup **完全缺失**，
//   enforce_session_create_rate_limit 只在 newSession 手写了一次 ——
//   loadSession / resumeSession 两条同样能创建会话的路径完全裸奔。
//   真源在这三处**都**调了双闸（:376-377、:408-410、:526-531）。

int resolve_list_sessions_page_size(const nlohmann::json& meta) {
    // 対齐 translator.session-list.ts:73-83
    if (!meta.is_object()) return ACP_LIST_SESSIONS_DEFAULT_PAGE_SIZE;
    // 対齐 readNumber(meta, ["limit","pageSize"]) —— 逐个别名试，
    // 仅接受有限数值（对齐 meta.ts:42-44 的 Number.isFinite 校验）
    for (const char* key : {"limit", "pageSize"}) {
        if (!meta.contains(key) || !meta[key].is_number()) continue;
        const double raw = meta[key].get<double>();
        if (!std::isfinite(raw)) continue;
        // 対齐:81 —— Math.min(MAX, Math.max(1, Math.floor(requested)))
        const long long floored = static_cast<long long>(std::floor(raw));
        const long long clamped_low = std::max<long long>(1, floored);
        return static_cast<int>(
            std::min<long long>(ACP_LIST_SESSIONS_MAX_PAGE_SIZE, clamped_low));
    }
    // 対齐:78-80 —— 未提供则用默认值
    return ACP_LIST_SESSIONS_DEFAULT_PAGE_SIZE;
}

// ⚠ 此前 assert_supported_session_setup **完全缺失**，
//   enforce_session_create_rate_limit 只在 newSession 手写了一次 ——
//   loadSession / resumeSession 两条同样能创建会话的路径完全裸奔。
//   真源在这三处**都**调了双闸（:376-377、:408-410、:526-531）。

void AcpGatewayAgent::assert_supported_session_setup(
    const nlohmann::json& mcp_servers) {
    // 対齐:1806-1813 —— 空数组放行，非空一律拒绝。
    // 桥接模式下 per-session MCP 无法生效（MCP 配在 gateway/agent 层），
    // 静默忽略会让客户端以为自己的 MCP server 已注册，实际工具永不可用。
    if (mcp_servers.is_null()) return;
    if (mcp_servers.is_array() && mcp_servers.empty()) return;
    if (!mcp_servers.is_array()) return;   // 非数组视为未提供
    throw AcpRuntimeError(
        AcpErrorCode::ACP_TURN_FAILED,
        "ACP bridge mode does not support per-session MCP servers. "
        "Configure MCP on the HunterClaw gateway or agent instead.");
}

void AcpGatewayAgent::enforce_session_create_rate_limit(const std::string& method) {
    // 対齐:1815-1825
    const auto budget = impl_->rate_limiter->consume();
    if (budget.allowed) return;
    // 対齐:1821-1823 —— 文案含 method 与向上取整的秒数，便于客户端退避
    const int64_t retry_s = (budget.retry_after_ms + 999) / 1000;
    throw AcpRuntimeError(
        AcpErrorCode::ACP_TURN_FAILED,
        "ACP session creation rate limit exceeded for " + method +
            "; retry after " + std::to_string(retry_s) + "s.");
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

void AcpGatewayAgent::set_session_patch(SessionPatchFn fn) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->session_patch = std::move(fn);
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
    // 対齐:376-377 —— 顺序不可颠倒：先拒绝不支持的会话配置，再消耗限流配额。
    // 反过来会让非法请求白白吃掉配额，被用来做配额耗尽攻击。
    assert_supported_session_setup(req.mcp_servers);
    enforce_session_create_rate_limit("newSession");

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
    // 対齐:408 —— MCP 闸无条件执行
    assert_supported_session_setup(req.mcp_servers);
    // 対齐:409-411 —— 限流**有条件**：仅在会话尚不存在（即本次调用会创建）时消耗配额。
    // 对已存在会话的 load 是纯读取，不应吃创建配额，否则正常的重连 replay
    // 会被限流误伤。
    if (!impl_->session_store->get_session(req.session_id)) {
        enforce_session_create_rate_limit("loadSession");
    }

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
    // 対齐:526 —— `params.mcpServers ?? []`，缺失按空数组处理
    assert_supported_session_setup(req.mcp_servers);
    // 対齐:527 assertAbsoluteCwd(params.cwd, "session/resume")
    // ⚠ 此前完全缺失。resume 允许带 cwd，相对路径会被后续按 gateway 进程的
    // 工作目录解析 —— 客户端以为自己指定了工作区，实际落到别处。
    if (!req.cwd.empty() && req.cwd[0] != '/') {
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                              "ACP session/resume requires an absolute cwd.");
    }

    auto session = impl_->session_store->get_session(req.session_id);
    // 対齐:529-532 —— 同 loadSession，限流仅在会话不存在时消耗
    if (!session) {
        enforce_session_create_rate_limit("resumeSession");
    }
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
    //
    // ★★ 捕获 impl_ 的 shared_ptr 自保活，并把它【作为参数】传进去 ——
    //   ⚠ 只捕 this 是不够的: run_approval_relay 内部若访问 this->impl_，
    //     那个 shared_ptr 成员本身随对象析构，读它就是 UAF。
    //     必须让 relay 全程使用局部 shared_ptr，不碰 this。
    //   ⚠ send_request 在 stdin EOF 时会被 conn.stop() 唤醒并返回
    //     "connection closed"，紧接着 serve_acp_stdio 就析构 agent，
    //     而 relay 后半段（isApprovalRelayActive 与 finally 标记）必然
    //     还要访问 impl_->mu —— 这是每次 EOF 都会走到的路径，不是窄竞态。
    auto keep = impl_;
    std::thread([keep, target_sid, correlated]() {
        run_approval_relay_impl(keep, target_sid, correlated.approval_id, correlated);
    }).detach();
}

// ★★ 静态实现: 全程只用传入的 impl，【不】接触 this ——
//   这是 UAF 防线的核心。见 start_approval_relay 处的说明。
// ⚠ 新增任何步骤时都不要引入 this/成员访问，否则防线失效。
void AcpGatewayAgent::run_approval_relay_impl(
        const std::shared_ptr<Impl>& impl,
        const std::string& session_id,
        const std::string& approval_id,
        const GatewayApprovalEvent& event) {
    ApprovalDetailsFn details_fn;
    ApprovalResolveFn resolve_fn;
    AcpStdioConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl->mu);
      details_fn = impl->approval_details;
      resolve_fn = impl->approval_resolve;
        conn = impl->connection;
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
        std::lock_guard<std::mutex> lk(impl->mu);
  auto it = impl->approval_relays.find(approval_id);
      still_active = (it != impl->approval_relays.end() && it->second.active);
    }
    std::string final_decision = still_active ? decision : "deny";

    // 7) 回写 Gateway（对齐 OC resolveGatewayApproval: exec.approval.resolve）。
    if (resolve_fn) {
        try { resolve_fn(approval_id, final_decision); } catch (...) {}
    }

    // 8) 标记完成（保留至 prompt 清理，作为去重哨兵；对齐 OC finally）。
    {
        std::lock_guard<std::mutex> lk(impl->mu);
        auto it = impl->approval_relays.find(approval_id);
        if (it != impl->approval_relays.end()) it->second.active = false;
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
// 🎯 严格対齐 OC translator.ts:578-604
//
// ⚠ 此前只做"查 session + 返回 default presentation"，
//   【完全没有】把 mode_id 写到任何地方 —— 用户在编辑器里切 thinking level，
//   接口返回成功，但 Gateway 侧毫无变化，presentation 里 current_mode_id
//   永远是 "adaptive"。静默失效：客户端以为设置生效了。
//
// 真源三个动作，顺序不可换：
//   ① modeId 为空 → 直接返回 {}（対齐:583-585，【不】发请求）
//   ② sessions.patch { key, thinkingLevel }（対齐:587-590）
//   ③ 用 override 重算 presentation 并 session/update 推送（対齐:592-598）
SetSessionModeResponse AcpGatewayAgent::handle_set_session_mode(
    const SetSessionModeRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session)
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED, "Session not found");

    // ★ 対齐:583-585 —— 空 modeId 提前返回，且【不】发任何 gateway 请求。
    //   ⚠ 不能当成错误抛出: 真源明确 resolves 成 {}（见
    //     translator.set-session-mode.test.ts:86-92 "returns early for empty modeId"，
    //     并断言 calls 为空）。
    if (req.mode_id.empty()) return SetSessionModeResponse{};

    SessionPatchFn patch_fn;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        patch_fn = impl_->session_patch;
    }
    // ⚠ 没有注入通道时必须报错，【不能】返回成功 ——
    //   静默成功会让客户端以为已生效（这正是修复前的行为）。
    if (!patch_fn)
        throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
                              "sessions.patch channel not wired: cannot apply session mode");

    // ★ 対齐:587-590 —— thinkingLevel 就是 modeId 本身（不做映射）。
    //   ⚠ 失败必须向上传播（対齐:599-602），吞掉等于谎报成功。
    patch_fn(session->session_key, nlohmann::json{{"thinkingLevel", req.mode_id}});

    // ★ 対齐:592-598 —— 带 override 重算 presentation 并推送 session/update。
    //   ⚠ 用 override 而非再查一次 gateway: 真源传
    //     { thinkingLevel: params.modeId } 作为 overrides，
    //     因为 patch 刚生效、快照可能还是旧值（读己之写不保证）。
    auto pres = build_session_presentation_override(req.mode_id);
    send_session_presentation_update(session->session_id, pres);

    return SetSessionModeResponse{pres.modes, pres.config_options};
}

// ── handleSetSessionConfigOption ─────────────────────────────────────────────
// 🎯 严格対齐 OC translator.ts:606-640 + resolveSessionConfigPatch(:1637-1700)
//
// ⚠ 此前同样只校验参数就返回 default presentation，设置从不落地。
SetSessionConfigOptionResponse AcpGatewayAgent::handle_set_session_config_option(
    const SetSessionConfigOptionRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session)
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED, "Session not found");

    // ★ 対齐 resolveSessionConfigPatch —— configId → gateway 字段名的映射。
    //   ⚠ 字段名【不等于】configId: thought_level → thinkingLevel、
    //     verbose_level → verboseLevel …… 直接把 configId 当字段名发过去，
    //     gateway 会静默忽略未知字段（又是一次"看起来成功"）。
    nlohmann::json patch;
    std::string override_thinking;
    if (req.option_id == CFG_THOUGHT_LEVEL) {
        patch["thinkingLevel"] = req.value;
        override_thinking = req.value;
    } else if (req.option_id == CFG_FAST_MODE) {
        patch["fastMode"] = req.value;
    } else if (req.option_id == CFG_VERBOSE_LEVEL) {
        patch["verboseLevel"] = req.value;
    } else if (req.option_id == CFG_REASONING) {
        patch["reasoningLevel"] = req.value;
    } else if (req.option_id == CFG_USAGE) {
        // ★ 対齐:1680-1685 —— "inherit" 映射为 JSON null（清除覆盖），
        //   ⚠ 不是字符串 "inherit"。发字符串过去会被存成字面值，
        //     从此该会话的 usage 永远显示 "inherit" 而非继承全局设置。
        if (req.value == "inherit") patch["responseUsage"] = nullptr;
        else patch["responseUsage"] = req.value;
    } else if (req.option_id == CFG_ELEVATED) {
        patch["elevatedLevel"] = req.value;
    } else {
        // ★ 対齐:1697-1698 default 分支 —— 未知 configId 直接拒绝。
        throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
                              "Unknown config option: " + req.option_id);
    }

    SessionPatchFn patch_fn;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        patch_fn = impl_->session_patch;
    }
    if (!patch_fn)
        throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
                              "sessions.patch channel not wired: cannot apply config option");

    // ★ 対齐:616-621 —— 仅当 patch 非空才发请求。
    //   （真源里 timeout 类 configId 只有 overrides、没有 patch，故需此判断）
    if (!patch.empty()) patch_fn(session->session_key, patch);

    auto pres = override_thinking.empty()
                    ? build_session_presentation_default()
                    : build_session_presentation_override(override_thinking);
    send_session_presentation_update(session->session_id, pres);

    return SetSessionConfigOptionResponse{pres.config_options, pres.modes};
}

// ── buildSessionPresentation ─────────────────────────────────────────────────
// 🎯 対齐 OC getSessionSnapshot(sessionKey, overrides)
//
// ★ 単一実装 + override 参数，【不】复制两份 level 列表 ——
//   复制会让"新增一个 thinking level"必须改两处，漏一处就出现
//   "模式列表里有、但切过去 current_mode_id 对不上"的诡异现象。
SessionPresentation AcpGatewayAgent::build_session_presentation_default() {
    return build_session_presentation_override("");
}

SessionPresentation AcpGatewayAgent::build_session_presentation_override(
    const std::string& thinking_override) {
    SessionPresentation p;
    std::vector<std::string> lvls = {"adaptive","off","minimal","low","medium","high","xhigh"};

    // ★ override 生效的前提是它确实在合法集合内 ——
    //   ⚠ 不校验就直接采用，客户端传一个 "turbo" 会让 current_mode_id
    //     指向一个 available_modes 里不存在的 id，编辑器 UI 会显示空选中态。
    std::string cm = "adaptive";
    if (!thinking_override.empty() &&
        std::find(lvls.begin(), lvls.end(), thinking_override) != lvls.end()) {
        cm = thinking_override;
    }

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

// 🎯 対齐 OC sendSessionSnapshotUpdate(session, snapshot, {includeControls:true})
//
// ⚠ 真源在 setSessionMode / setSessionConfigOption 成功后【必须】推这条
//   session/update —— 否则客户端界面上的选中态不会刷新，用户看到自己刚
//   点的选项没被选中，会以为设置失败并反复点击。
void AcpGatewayAgent::send_session_presentation_update(
    const std::string& session_id, const SessionPresentation& pres) {
    nlohmann::json modes = nlohmann::json::object();
    modes["currentModeId"] = pres.modes.current_mode_id;
    nlohmann::json avail = nlohmann::json::array();
    for (const auto& m : pres.modes.available_modes) {
        avail.push_back({{"id", m.id}, {"name", m.name}, {"description", m.description}});
    }
    modes["availableModes"] = std::move(avail);

    nlohmann::json opts = nlohmann::json::array();
    for (const auto& o : pres.config_options) {
        nlohmann::json vals = nlohmann::json::array();
        for (const auto& v : o.options) {
            vals.push_back({{"value", v.value}, {"name", v.name}});
        }
        opts.push_back({{"type", o.type}, {"id", o.id}, {"name", o.name},
                        {"category", o.category}, {"description", o.description},
                        {"currentValue", o.current_value}, {"options", std::move(vals)}});
    }

    emit_session_update(session_id, nlohmann::json{
        {"sessionUpdate", "current_mode_update"},
        {"modes", std::move(modes)},
        {"configOptions", std::move(opts)}});
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

// ✅ 严格对齐 OC translator.ts:1157-1171:
//   Gateway → ACP StopReason 映射。ACP spec 枚举（snake_case）:
//     "end_turn" | "max_tokens" | "cancelled" | "refusal"
//
// 真源逻辑（**按 state 分支**，不是一张扁平字符串表）：
//   state === "final"   → rawStopReason === "max_tokens" ? "max_tokens" : "end_turn"
//   state === "aborted" → "cancelled"                       （无条件）
//   state === "error"   → errorKind === "refusal" ? "refusal" : "end_turn"
//
// ⚠ 此前的实现虽然注释抄对了，但代码把三个 state 压成了一张扁平表，
//   导致两处语义错误：
//     ① final 状态下 rawStopReason=="refusal"/"content_filter" 会被映射成
//        "refusal" —— 真源在 final 只认 max_tokens，其余**全部**归 end_turn。
//        refusal 只能来自 state=="error" 且 errorKind=="refusal"。
//     ② aborted 状态被要求 rawStopReason 也长得像 abort 才返回 cancelled；
//        真源在 state=="aborted" 时**无条件** cancelled，与 rawStopReason 无关。
//   后果：客户端收到错误的 turn 结局，UI 上"被拒绝"与"正常结束"互相串台。
std::string map_acp_stop_reason_for_state(
    const std::string& state,
    const std::string& raw_stop_reason,
    const std::string& error_kind) {
    // 対齐:1163-1166 —— aborted 无条件 cancelled
    if (state == "aborted") return "cancelled";
    // 対齐:1168-1170 —— 仅 error 状态下 errorKind=="refusal" 才是 refusal
    if (state == "error") {
        return error_kind == "refusal" ? "refusal" : "end_turn";
    }
    // 対齐:1158-1159 —— final（及其余状态）只认字面 "max_tokens"
    return raw_stop_reason == "max_tokens" ? "max_tokens" : "end_turn";
}

// 兼容旧调用点的薄封装：只有 rawStopReason 可用（无 state 上下文）时使用。
// 按真源 final 分支语义处理 —— 这是最常见的场景（正常收尾）。
std::string AcpGatewayAgent::map_stop_reason(const std::string& g) {
    return map_acp_stop_reason_for_state(/*state=*/"final", /*raw_stop_reason=*/g,
                                        /*error_kind=*/std::string());
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
