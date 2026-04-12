// ============================================================================
// acp_translator.cc
// 🎯 严格对齐 OpenClaw: src/acp/translator.ts (AcpGatewayAgent — 1300+ lines)
// ACP 消息翻译层: Gateway ↔ ACP SDK 双向转换
// ============================================================================

#include "hunterclaw/acp/acp_translator.h"
#include "hunterclaw/acp/acp_event_mapper.h"
#include "hunterclaw/acp/acp_session_manager.h"
#include "hunterclaw/core/config.h"
#include "hunterclaw/gateway/gateway_client.h"
#include "hunterclaw/infra/rate_limiter.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>

namespace hunterclaw {
namespace acp {

// ── Constants ────────────────────────────────────────────────────────────────
// 🎯 对齐 translator.ts 顶层常量

static constexpr size_t MAX_PROMPT_BYTES = 2 * 1024 * 1024;  // 2MB DoS guard
static const std::string ACP_THOUGHT_LEVEL_CONFIG_ID = "thought_level";
static const std::string ACP_FAST_MODE_CONFIG_ID = "fast_mode";
static const std::string ACP_VERBOSE_LEVEL_CONFIG_ID = "verbose_level";
static const std::string ACP_REASONING_LEVEL_CONFIG_ID = "reasoning_level";
static const std::string ACP_RESPONSE_USAGE_CONFIG_ID = "response_usage";
static const std::string ACP_ELEVATED_LEVEL_CONFIG_ID = "elevated_level";
static constexpr int ACP_LOAD_SESSION_REPLAY_LIMIT = 1000000;
static constexpr int64_t ACP_GATEWAY_DISCONNECT_GRACE_MS = 5000;

static constexpr int SESSION_CREATE_RATE_LIMIT_DEFAULT_MAX_REQUESTS = 120;
static constexpr int64_t SESSION_CREATE_RATE_LIMIT_DEFAULT_WINDOW_MS = 10000;

// ── Helper types ─────────────────────────────────────────────────────────────
// 🎯 对齐 translator.ts PendingPrompt, PendingToolCall, DisconnectContext

struct DisconnectContext {
    int generation = 0;
    std::string reason;
};

struct PendingToolCall {
    std::string kind;  // "read", "write", "exec", "other"
    std::vector<ToolCallLocation> locations;
    nlohmann::json raw_input;
    std::string title;
};

struct PendingPrompt {
    std::string session_id;
    std::string session_key;
    std::string idempotency_key;
    bool send_accepted = false;
    std::optional<DisconnectContext> disconnect_context;
    size_t sent_text_length = 0;
    std::string sent_text;
    size_t sent_thought_length = 0;
    std::string sent_thought;
    std::map<std::string, PendingToolCall> tool_calls;
    // Promise-like resolve/reject handled via callback
    std::function<void(PromptResponse)> on_resolve;
    std::function<void(std::string)> on_reject;
};

// ── AcpGatewayAgent Implementation ──────────────────────────────────────────
// 🎯 对齐 translator.ts class AcpGatewayAgent implements Agent

struct AcpGatewayAgent::Impl {
    gateway::GatewayClient* gw = nullptr;
    AcpServerOptions opts;
    AcpSessionStore* session_store = nullptr;

    // 🎯 对齐: session state tracking
    std::mutex mu;
    std::map<std::string, PendingPrompt> pending_prompts;  // session_id -> prompt
    int disconnect_generation = 0;
    bool gateway_connected = false;

    // 🎯 对齐: rate limiter for session creation
    std::unique_ptr<infra::FixedWindowRateLimiter> session_create_rate_limiter;

    // 🎯 对齐: agent info
    AcpAgentInfo agent_info;

    Impl() {
        agent_info.name = "hunterclaw-acp";
        agent_info.title = "HunterClaw ACP Gateway";
        agent_info.version = HUNTERCLAW_VERSION;
    }
};

AcpGatewayAgent::AcpGatewayAgent(gateway::GatewayClient* gw, const AcpServerOptions& opts,
                                   AcpSessionStore* store)
    : impl_(std::make_unique<Impl>()) {
    impl_->gw = gw;
    impl_->opts = opts;
    impl_->session_store = store;

    int max_req = opts.session_create_rate_limit_max_requests > 0
        ? opts.session_create_rate_limit_max_requests
        : SESSION_CREATE_RATE_LIMIT_DEFAULT_MAX_REQUESTS;
    int64_t window_ms = opts.session_create_rate_limit_window_ms > 0
        ? opts.session_create_rate_limit_window_ms
        : SESSION_CREATE_RATE_LIMIT_DEFAULT_WINDOW_MS;

    impl_->session_create_rate_limiter =
        std::make_unique<infra::FixedWindowRateLimiter>(max_req, window_ms);
}

AcpGatewayAgent::~AcpGatewayAgent() = default;

// ── handleInitialize ─────────────────────────────────────────────────────────
// 🎯 对齐 translator.ts handleInitialize(request: InitializeRequest)

InitializeResponse AcpGatewayAgent::handle_initialize(const InitializeRequest& req) {
    InitializeResponse resp;
    resp.protocol_version = ACP_PROTOCOL_VERSION;
    resp.agent.name = impl_->agent_info.name;
    resp.agent.title = impl_->agent_info.title;
    resp.agent.version = impl_->agent_info.version;
    resp.commands = get_available_commands();
    return resp;
}

// ── handleAuthenticate ───────────────────────────────────────────────────────
// 🎯 对齐 translator.ts handleAuthenticate(request: AuthenticateRequest)

AuthenticateResponse AcpGatewayAgent::handle_authenticate(const AuthenticateRequest& req) {
    AuthenticateResponse resp;
    resp.authenticated = true;  // Gateway handles real auth
    return resp;
}

// ── handleNewSession ─────────────────────────────────────────────────────────
// 🎯 对齐 translator.ts handleNewSession(request: NewSessionRequest)

NewSessionResponse AcpGatewayAgent::handle_new_session(const NewSessionRequest& req) {
    // Rate limit check
    if (!impl_->session_create_rate_limiter->try_acquire()) {
        throw AcpRuntimeError("ACP_RATE_LIMITED", "Session creation rate limit exceeded");
    }

    // Parse session metadata from request
    AcpSessionMeta meta = parse_session_meta(req.meta);

    // Resolve session key
    std::string session_key = meta.session_key.value_or(
        impl_->opts.default_session_key.value_or("default"));

    // Check reset
    bool should_reset = meta.reset_session.value_or(impl_->opts.reset_session);
    if (should_reset) {
        // 🎯 对齐: resetSessionIfNeeded
        impl_->gw->request("sessions.reset", {{"key", session_key}});
    }

    // Create or get session from store
    std::string cwd = req.cwd.empty() ? "." : req.cwd;
    auto session = impl_->session_store->create_session(session_key, cwd);

    // Build session presentation
    SessionPresentation pres = build_session_presentation(session_key);

    NewSessionResponse resp;
    resp.session_id = session.session_id;
    resp.config_options = pres.config_options;
    resp.modes = pres.modes;
    return resp;
}

// ── handleListSessions ───────────────────────────────────────────────────────
// 🎯 对齐 translator.ts handleListSessions(request: ListSessionsRequest)

ListSessionsResponse AcpGatewayAgent::handle_list_sessions(const ListSessionsRequest& req) {
    auto result = impl_->gw->request_typed<SessionsListResult>("sessions.list", {
        {"limit", req.limit > 0 ? req.limit : 50}
    });

    ListSessionsResponse resp;
    for (auto& row : result.sessions) {
        AcpSessionInfo info;
        info.session_id = row.session_id;
        info.title = row.derived_title.empty() ? row.label : row.derived_title;
        info.updated_at = row.updated_at;
        resp.sessions.push_back(std::move(info));
    }
    return resp;
}

// ── handleLoadSession ────────────────────────────────────────────────────────
// 🎯 对齐 translator.ts handleLoadSession(request: LoadSessionRequest)

LoadSessionResponse AcpGatewayAgent::handle_load_session(const LoadSessionRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session) {
        throw AcpRuntimeError("ACP_SESSION_NOT_FOUND", "Session not found: " + req.session_id);
    }

    // Build presentation
    SessionPresentation pres = build_session_presentation(session->session_key);

    LoadSessionResponse resp;
    resp.session_id = session->session_id;
    resp.config_options = pres.config_options;
    resp.modes = pres.modes;

    // 🎯 对齐: transcript replay (extractReplayChunks)
    if (req.replay) {
        auto transcript = impl_->gw->request_typed<TranscriptResult>(
            "sessions.transcript",
            {{"key", session->session_key}, {"limit", ACP_LOAD_SESSION_REPLAY_LIMIT}});
        for (auto& msg : transcript.messages) {
            auto chunks = extract_replay_chunks(msg);
            for (auto& chunk : chunks) {
                resp.replay_chunks.push_back(std::move(chunk));
            }
        }
    }

    return resp;
}

// ── handlePrompt ─────────────────────────────────────────────────────────────
// 🎯 对齐 translator.ts handlePrompt(request: PromptRequest)
// 这是核心入口：接收 ACP prompt → 转发到 Gateway chat → 流式回调

PromptResponse AcpGatewayAgent::handle_prompt(const PromptRequest& req,
                                                PromptStreamCallback stream_cb) {
    // Validate prompt size (DoS guard, CWE-400)
    size_t prompt_bytes = req.text.size();
    for (auto& att : req.attachments) {
        prompt_bytes += att.content.size();
    }
    if (prompt_bytes > MAX_PROMPT_BYTES) {
        throw AcpRuntimeError("ACP_PROMPT_TOO_LARGE",
            "Prompt exceeds maximum size of 2MB");
    }

    auto session = impl_->session_store->get_session(req.session_id);
    if (!session) {
        throw AcpRuntimeError("ACP_SESSION_NOT_FOUND",
            "Session not found: " + req.session_id);
    }

    // 🎯 对齐: build gateway chat request
    nlohmann::json chat_params;
    chat_params["key"] = session->session_key;
    chat_params["text"] = req.text;
    if (!req.attachments.empty()) {
        // 🎯 对齐: extractAttachmentsFromPrompt
        auto att_json = nlohmann::json::array();
        for (auto& att : req.attachments) {
            att_json.push_back({
                {"type", att.type},
                {"mimeType", att.mime_type},
                {"content", att.content}
            });
        }
        chat_params["attachments"] = att_json;
    }

    // 🎯 对齐: provenance mode
    if (impl_->opts.provenance_mode == "meta" ||
        impl_->opts.provenance_mode == "meta+receipt") {
        chat_params["provenance"] = impl_->opts.provenance_mode;
    }

    // Register pending prompt
    std::string run_id;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        PendingPrompt pending;
        pending.session_id = req.session_id;
        pending.session_key = session->session_key;
        pending.idempotency_key = req.idempotency_key;
        impl_->pending_prompts[req.session_id] = std::move(pending);
    }

    // 🎯 对齐: send to gateway and process events
    auto result = impl_->gw->request_typed<ChatResult>("chat", chat_params);
    run_id = result.run_id;

    // Track active run
    impl_->session_store->set_active_run(req.session_id, run_id);

    // Build response
    PromptResponse resp;
    resp.stop_reason = map_stop_reason(result.stop_reason);
    resp.text = result.text;

    // Clean up
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->pending_prompts.erase(req.session_id);
    }
    impl_->session_store->clear_active_run(req.session_id);

    return resp;
}

// ── handleGatewayEvent ───────────────────────────────────────────────────────
// 🎯 对齐 translator.ts handleGatewayEvent(evt: EventFrame)

void AcpGatewayAgent::handle_gateway_event(const gateway::EventFrame& evt) {
    std::lock_guard<std::mutex> lock(impl_->mu);

    // Find matching pending prompt by session key
    for (auto& [sid, pending] : impl_->pending_prompts) {
        if (pending.session_key != evt.session_key) continue;

        // 🎯 对齐: process different event types
        if (evt.type == "agent_text") {
            // Accumulate text delta
            pending.sent_text += evt.text;
            pending.sent_text_length += evt.text.size();
        }
        else if (evt.type == "agent_thought") {
            // Accumulate thought delta
            pending.sent_thought += evt.text;
            pending.sent_thought_length += evt.text.size();
        }
        else if (evt.type == "tool_call_start") {
            // 🎯 对齐: track tool call
            PendingToolCall tc;
            tc.kind = infer_tool_kind(evt.tool_name);
            tc.title = format_tool_title(evt.tool_name, evt.tool_input);
            tc.raw_input = evt.tool_input;
            tc.locations = extract_tool_call_locations(evt.tool_input);
            pending.tool_calls[evt.tool_call_id] = std::move(tc);
        }
        else if (evt.type == "tool_call_end") {
            // Tool call completed — extract locations from result
            if (auto it = pending.tool_calls.find(evt.tool_call_id);
                it != pending.tool_calls.end()) {
                auto result_locations = extract_tool_call_locations_from_result(evt.tool_result);
                for (auto& loc : result_locations) {
                    it->second.locations.push_back(std::move(loc));
                }
            }
        }
        else if (evt.type == "run_accepted") {
            pending.send_accepted = true;
        }
        else if (evt.type == "run_finished") {
            // Run complete — resolve the pending prompt
        }
        break;
    }
}

// ── handleGatewayReconnect ───────────────────────────────────────────────────
// 🎯 对齐 translator.ts handleGatewayReconnect()

void AcpGatewayAgent::handle_gateway_reconnect() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->gateway_connected = true;
    impl_->disconnect_generation++;
}

// ── handleGatewayDisconnect ──────────────────────────────────────────────────
// 🎯 对齐 translator.ts handleGatewayDisconnect(reason: string)

void AcpGatewayAgent::handle_gateway_disconnect(const std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->gateway_connected = false;

    // Mark all pending prompts with disconnect context
    int gen = ++impl_->disconnect_generation;
    for (auto& [sid, pending] : impl_->pending_prompts) {
        if (!pending.disconnect_context) {
            pending.disconnect_context = DisconnectContext{gen, reason};
        }
    }
}

// ── handleSetSessionMode ─────────────────────────────────────────────────────
// 🎯 对齐 translator.ts handleSetSessionMode(request: SetSessionModeRequest)

SetSessionModeResponse AcpGatewayAgent::handle_set_session_mode(
    const SetSessionModeRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session) {
        throw AcpRuntimeError("ACP_SESSION_NOT_FOUND", "Session not found");
    }

    // Send thought level change to gateway
    impl_->gw->request("sessions.patch", {
        {"key", session->session_key},
        {"patch", {{"thinkingLevel", req.mode_id}}}
    });

    // Rebuild presentation
    SessionPresentation pres = build_session_presentation(session->session_key);

    SetSessionModeResponse resp;
    resp.modes = pres.modes;
    resp.config_options = pres.config_options;
    return resp;
}

// ── handleSetSessionConfigOption ─────────────────────────────────────────────
// 🎯 对齐 translator.ts handleSetSessionConfigOption(request)

SetSessionConfigOptionResponse AcpGatewayAgent::handle_set_session_config_option(
    const SetSessionConfigOptionRequest& req) {
    auto session = impl_->session_store->get_session(req.session_id);
    if (!session) {
        throw AcpRuntimeError("ACP_SESSION_NOT_FOUND", "Session not found");
    }

    // 🎯 对齐: map config option ID to gateway patch field
    nlohmann::json patch;
    if (req.option_id == ACP_THOUGHT_LEVEL_CONFIG_ID) {
        patch["thinkingLevel"] = req.value;
    } else if (req.option_id == ACP_FAST_MODE_CONFIG_ID) {
        patch["fastMode"] = (req.value == "on");
    } else if (req.option_id == ACP_VERBOSE_LEVEL_CONFIG_ID) {
        patch["verboseLevel"] = req.value;
    } else if (req.option_id == ACP_REASONING_LEVEL_CONFIG_ID) {
        patch["reasoningLevel"] = req.value;
    } else if (req.option_id == ACP_RESPONSE_USAGE_CONFIG_ID) {
        patch["responseUsage"] = req.value;
    } else if (req.option_id == ACP_ELEVATED_LEVEL_CONFIG_ID) {
        patch["elevatedLevel"] = req.value;
    } else {
        throw AcpRuntimeError("ACP_INVALID_CONFIG", "Unknown config option: " + req.option_id);
    }

    impl_->gw->request("sessions.patch", {
        {"key", session->session_key},
        {"patch", patch}
    });

    SessionPresentation pres = build_session_presentation(session->session_key);

    SetSessionConfigOptionResponse resp;
    resp.config_options = pres.config_options;
    resp.modes = pres.modes;
    return resp;
}

// ── buildSessionPresentation ─────────────────────────────────────────────────
// 🎯 对齐 translator.ts buildSessionPresentation(params)

SessionPresentation AcpGatewayAgent::build_session_presentation(
    const std::string& session_key) {
    SessionPresentation pres;

    // Query gateway for current session state
    auto row = impl_->gw->request_typed<GatewaySessionRow>(
        "sessions.get", {{"key", session_key}});

    // 🎯 对齐: thinking levels
    std::string current_mode = row.thinking_level.empty() ? "adaptive" : row.thinking_level;
    std::vector<std::string> available_levels = {"adaptive", "off", "minimal", "low",
                                                   "medium", "high", "xhigh"};

    // Modes
    pres.modes.current_mode_id = current_mode;
    for (auto& level : available_levels) {
        SessionMode mode;
        mode.id = level;
        mode.name = format_thinking_level_name(level);
        mode.description = build_thinking_mode_description(level);
        pres.modes.available_modes.push_back(std::move(mode));
    }

    // 🎯 对齐: config options (6 种配置项)
    pres.config_options.push_back(build_select_config_option(
        ACP_THOUGHT_LEVEL_CONFIG_ID, "Thought level", "thought_level",
        "Controls how much deliberate reasoning is requested.",
        current_mode, available_levels));

    pres.config_options.push_back(build_select_config_option(
        ACP_FAST_MODE_CONFIG_ID, "Fast mode", "",
        "Controls whether sessions use the fast-mode profile.",
        row.fast_mode ? "on" : "off", {"off", "on"}));

    pres.config_options.push_back(build_select_config_option(
        ACP_VERBOSE_LEVEL_CONFIG_ID, "Tool verbosity", "",
        "Controls how much tool progress detail is kept enabled.",
        row.verbose_level.empty() ? "off" : row.verbose_level,
        {"off", "on", "full"}));

    pres.config_options.push_back(build_select_config_option(
        ACP_REASONING_LEVEL_CONFIG_ID, "Reasoning stream", "",
        "Controls whether reasoning-capable models emit reasoning text.",
        row.reasoning_level.empty() ? "off" : row.reasoning_level,
        {"off", "on", "stream"}));

    pres.config_options.push_back(build_select_config_option(
        ACP_RESPONSE_USAGE_CONFIG_ID, "Usage detail", "",
        "Controls how much usage information is attached to responses.",
        row.response_usage.empty() ? "off" : row.response_usage,
        {"off", "tokens", "full"}));

    pres.config_options.push_back(build_select_config_option(
        ACP_ELEVATED_LEVEL_CONFIG_ID, "Elevated actions", "",
        "Controls how aggressively elevated execution is allowed.",
        row.elevated_level.empty() ? "off" : row.elevated_level,
        {"off", "on", "ask", "full"}));

    return pres;
}

// ── buildSelectConfigOption ──────────────────────────────────────────────────
// 🎯 对齐 translator.ts buildSelectConfigOption(params)

SessionConfigOption AcpGatewayAgent::build_select_config_option(
    const std::string& id, const std::string& name, const std::string& category,
    const std::string& description, const std::string& current_value,
    const std::vector<std::string>& values) {
    SessionConfigOption opt;
    opt.type = "select";
    opt.id = id;
    opt.name = name;
    opt.category = category;
    opt.description = description;
    opt.current_value = current_value;
    for (auto& v : values) {
        ConfigOptionValue ov;
        ov.value = v;
        ov.name = format_config_value_name(v);
        opt.options.push_back(std::move(ov));
    }
    return opt;
}

// ── formatThinkingLevelName ──────────────────────────────────────────────────
// 🎯 对齐 translator.ts formatThinkingLevelName(level)

std::string AcpGatewayAgent::format_thinking_level_name(const std::string& level) {
    if (level == "xhigh") return "Extra High";
    if (level == "adaptive") return "Adaptive";
    if (level.empty()) return "Unknown";
    std::string result = level;
    result[0] = toupper(result[0]);
    return result;
}

// ── buildThinkingModeDescription ─────────────────────────────────────────────
// 🎯 对齐 translator.ts buildThinkingModeDescription(level)

std::string AcpGatewayAgent::build_thinking_mode_description(const std::string& level) {
    if (level == "adaptive") {
        return "Use the Gateway session default thought level.";
    }
    return "";
}

// ── formatConfigValueName ────────────────────────────────────────────────────
// 🎯 对齐 translator.ts formatConfigValueName(value)

std::string AcpGatewayAgent::format_config_value_name(const std::string& value) {
    if (value == "xhigh") return "Extra High";
    if (value.empty()) return "Unknown";
    std::string result = value;
    result[0] = toupper(result[0]);
    return result;
}

// ── mapStopReason ────────────────────────────────────────────────────────────
// 🎯 对齐 translator.ts stop reason mapping

std::string AcpGatewayAgent::map_stop_reason(const std::string& gateway_reason) {
    if (gateway_reason == "end_turn" || gateway_reason == "stop") return "endTurn";
    if (gateway_reason == "tool_use") return "toolUse";
    if (gateway_reason == "max_tokens") return "maxOutputTokens";
    if (gateway_reason == "content_filtered") return "contentFiltered";
    return "endTurn";
}

// ── extractReplayChunks ──────────────────────────────────────────────────────
// 🎯 对齐 translator.ts extractReplayChunks(message)

std::vector<ReplayChunk> AcpGatewayAgent::extract_replay_chunks(
    const TranscriptMessage& msg) {
    std::vector<ReplayChunk> chunks;
    if (msg.role != "user" && msg.role != "assistant") return chunks;

    std::string update_type = (msg.role == "user")
        ? "user_message_chunk" : "agent_message_chunk";

    if (!msg.text_content.empty()) {
        chunks.push_back({update_type, msg.text_content});
    }

    // 🎯 对齐: extract thinking blocks as agent_thought_chunk
    for (auto& block : msg.content_blocks) {
        if (block.type == "thinking" && !block.thinking.empty()) {
            chunks.push_back({"agent_thought_chunk", block.thinking});
        }
        else if (block.type == "text" && !block.text.empty()) {
            chunks.push_back({update_type, block.text});
        }
    }

    return chunks;
}

// ── parseSessionMeta ─────────────────────────────────────────────────────────
// 🎯 对齐 session-mapper.ts parseSessionMeta(meta)

AcpSessionMeta AcpGatewayAgent::parse_session_meta(const nlohmann::json& meta) {
    AcpSessionMeta result;
    if (meta.is_null() || !meta.is_object()) return result;

    // Read sessionKey from multiple possible field names
    for (auto& key : {"sessionKey", "session", "key"}) {
        if (meta.contains(key) && meta[key].is_string()) {
            result.session_key = meta[key].get<std::string>();
            break;
        }
    }

    // Read sessionLabel
    for (auto& key : {"sessionLabel", "label"}) {
        if (meta.contains(key) && meta[key].is_string()) {
            result.session_label = meta[key].get<std::string>();
            break;
        }
    }

    // Read resetSession
    for (auto& key : {"resetSession", "reset"}) {
        if (meta.contains(key) && meta[key].is_boolean()) {
            result.reset_session = meta[key].get<bool>();
            break;
        }
    }

    // Read requireExisting
    for (auto& key : {"requireExistingSession", "requireExisting"}) {
        if (meta.contains(key) && meta[key].is_boolean()) {
            result.require_existing = meta[key].get<bool>();
            break;
        }
    }

    return result;
}

} // namespace acp
} // namespace hunterclaw
