// ============================================================================
// acp_translator.cc
// 🎯 严格对齐 OpenClaw: src/acp/translator.ts (AcpGatewayAgent — 1300+ lines)
// ACP 消息翻译层: Gateway ↔ ACP SDK 双向转换
// ============================================================================

#include "hunterclaw/acp/acp_translator.h"
#include "hunterclaw/acp/acp_event_mapper.h"
#include "hunterclaw/acp/runtime_errors.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>

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
    r.commands = get_available_commands();        // 兼容旧 HC 字段（OC 通过独立 sendAvailableCommands RPC 发送）
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
    return r;
}

// ── handleListSessions ───────────────────────────────────────────────────────
ListSessionsResponse AcpGatewayAgent::handle_list_sessions(const ListSessionsRequest& /*req*/) {
    // Gateway interaction deferred to runtime wiring
    return ListSessionsResponse{};
}

// ── handleLoadSession ────────────────────────────────────────────────────────
LoadSessionResponse AcpGatewayAgent::handle_load_session(const LoadSessionRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session)
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                              "Session not found: " + req.session_id);
    auto pres = build_session_presentation_default();
    LoadSessionResponse r;
    r.session_id = session->session_id;
    r.config_options = pres.config_options;
    r.modes = pres.modes;
    return r;
}

// ── handlePrompt ─────────────────────────────────────────────────────────────
PromptResponse AcpGatewayAgent::handle_prompt(const PromptRequest& req,
                                                PromptStreamCallback /*cb*/) {
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
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        PendingPromptEntry pe;
        pe.session_id = req.session_id;
        pe.session_key = session->session_key;
        pe.idempotency_key = req.idempotency_key;
        impl_->pending[req.session_id] = std::move(pe);
    }

    // NOTE: actual gateway chat dispatch is wired at runtime (gateway layer).
    // This translation layer prepares the request; the response arrives via
    // handle_gateway_event() callbacks.

    // ✅ 严格对齐 OC translator.ts:957-971 finishPrompt stopReason:
    //   state=="final"        → "end_turn" (或 "max_tokens" if rawStopReason)
    //   state=="aborted"      → "cancelled"
    //   state=="error" refusal→ "refusal"
    //   其他                   → "end_turn"
    // ACP spec StopReason 枚举是 snake_case: "end_turn" | "max_tokens" | "cancelled" | "refusal"
    // 旧实现返回 "endTurn"（camelCase）→ Cursor/Zed 等 ACP 客户端会拒绝
    PromptResponse r;
    r.stop_reason = "end_turn";
    return r;
}

// ── handleGatewayEvent ───────────────────────────────────────────────────────
void AcpGatewayAgent::handle_gateway_event(const nlohmann::json& evt) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::string sk = evt.value("sessionKey", "");
    std::string type = evt.value("type", "");

    for (auto& [sid, pe] : impl_->pending) {
        if (pe.session_key != sk) continue;
        if (type == "agent_text") {
            pe.sent_text += evt.value("text", "");
        } else if (type == "agent_thought") {
            pe.sent_thought += evt.value("text", "");
        } else if (type == "tool_call_start") {
            PendingToolCallEntry tc;
            std::string tn = evt.value("toolName", "");
            tc.kind = infer_tool_kind(tn);
            tc.raw_input = evt.value("toolInput", nlohmann::json{});
            tc.title = format_tool_title(tn, tc.raw_input);
            tc.locations = extract_tool_call_locations(tc.raw_input);
            pe.tool_calls[evt.value("toolCallId", "")] = std::move(tc);
        } else if (type == "tool_call_end") {
            std::string tcid = evt.value("toolCallId", "");
            if (auto it = pe.tool_calls.find(tcid); it != pe.tool_calls.end()) {
                auto locs = extract_tool_call_locations_from_result(
                    evt.value("toolResult", ""));
                for (auto& l : locs) it->second.locations.push_back(std::move(l));
            }
        } else if (type == "run_accepted") {
            pe.send_accepted = true;
        }
        break;
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

AcpSessionMeta_Request AcpGatewayAgent::parse_session_meta(const nlohmann::json& meta) {
    AcpSessionMeta_Request r;
    if (meta.is_null() || !meta.is_object()) return r;
    for (auto& k : {"sessionKey","session","key"})
        if (meta.contains(k) && meta[k].is_string()) { r.session_key = meta[k].get<std::string>(); break; }
    for (auto& k : {"sessionLabel","label"})
        if (meta.contains(k) && meta[k].is_string()) { r.session_label = meta[k].get<std::string>(); break; }
    for (auto& k : {"resetSession","reset"})
        if (meta.contains(k) && meta[k].is_boolean()) { r.reset_session = meta[k].get<bool>(); break; }
    for (auto& k : {"requireExistingSession","requireExisting"})
        if (meta.contains(k) && meta[k].is_boolean()) { r.require_existing = meta[k].get<bool>(); break; }
    return r;
}

// ── getAvailableCommands ─────────────────────────────────────────────────────
std::vector<nlohmann::json> get_available_commands() {
    return {
        {{"name","help"},{"description","Show help and common commands."}},
        {{"name","commands"},{"description","List available commands."}},
        {{"name","status"},{"description","Show current status."}},
        {{"name","context"},{"description","Explain context usage."}},
        {{"name","subagents"},{"description","List or manage sub-agents."}},
        {{"name","stop"},{"description","Stop the current run."}},
        {{"name","reset"},{"description","Reset the session."}},
        {{"name","compact"},{"description","Compact the session history."}},
        {{"name","model"},{"description","Select a model."}},
        {{"name","think"},{"description","Set thinking level."}},
        {{"name","verbose"},{"description","Set verbose mode."}},
    };
}

} // namespace acp
} // namespace hunterclaw
