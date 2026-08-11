#pragma once

// ============================================================================
// acp_translator.h
// 🎯 对齐 OpenClaw: src/acp/translator.ts (AcpGatewayAgent)
// ============================================================================

#include "hunterclaw/acp/acp_event_mapper.h"
#include "hunterclaw/acp/runtime_types.h"
#include "hunterclaw/acp/runtime_errors.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <functional>
#include <nlohmann/json.hpp>

namespace hunterclaw {

// Forward declarations
namespace gateway {
    class GatewayClient;
    struct EventFrame;
}
namespace core {
    class Config;
}
namespace infra {
    class FixedWindowRateLimiter;
}

namespace acp {

// Forward declaration: stdio JSON-RPC 连接（session/update notification 回推 +
// session/request_permission server→client request）。定义见 acp_stdio_transport.h。
class AcpStdioConnection;

// ── ACP Protocol version ─────────────────────────────────────────────────────
// ✅ 对齐 OC: src/acp/translator.ts 引用 @agentclientprotocol/sdk 的 PROTOCOL_VERSION
// 根据 ACP schema (uint16 integer): "Only bumped for breaking changes.
//   Non-breaking changes should be introduced via capabilities."
// 当前 ACP major version = 1。Cursor CLI 等客户端明确要求 numeric 而非 string。
// 旧实现误写为字符串 "2025-03-26"（那是 MCP 协议版本，非 ACP）→ 导致 initialize 握手失败。
static constexpr int ACP_PROTOCOL_VERSION = 1;
static const std::string HUNTERCLAW_VERSION = "0.1.0";

// ── ACP Types ────────────────────────────────────────────────────────────────

struct AcpAgentInfo {
    std::string name;
    std::string title;
    std::string version;
};

struct AcpServerOptions {
    std::string gateway_url;
    std::string gateway_token;
    std::string gateway_password;
    std::optional<std::string> default_session_key;
    std::optional<std::string> default_session_label;
    bool require_existing_session = false;
    bool reset_session = false;
    bool prefix_cwd = false;
    std::string provenance_mode;  // "off" | "meta" | "meta+receipt"
    int session_create_rate_limit_max_requests = 0;
    int64_t session_create_rate_limit_window_ms = 0;
    bool verbose = false;
};

// ── ACP Session ──────────────────────────────────────────────────────────────

struct AcpSession {
    std::string session_id;
    std::string session_key;
    std::string cwd;
    int64_t created_at = 0;
    int64_t last_touched_at = 0;
    std::string active_run_id;
    bool cancelled = false;
};

// ── Session Store Interface ──────────────────────────────────────────────────

class AcpSessionStore {
public:
    virtual ~AcpSessionStore() = default;
    virtual AcpSession create_session(const std::string& session_key,
                                       const std::string& cwd,
                                       const std::string& explicit_session_id = "") = 0;
    virtual bool has_session(const std::string& session_id) const = 0;
    virtual std::optional<AcpSession> get_session(const std::string& session_id) const = 0;
    virtual std::optional<AcpSession> get_session_by_run_id(const std::string& run_id) const = 0;
    virtual void set_active_run(const std::string& session_id, const std::string& run_id) = 0;
    virtual void clear_active_run(const std::string& session_id) = 0;
    virtual bool cancel_active_run(const std::string& session_id) = 0;
};

// ── ACP Session Meta (from request) ─────────────────────────────────────────

struct AcpSessionMeta_Request {
    std::optional<std::string> session_key;
    std::optional<std::string> session_label;
    std::optional<bool> reset_session;
    std::optional<bool> require_existing;
    std::optional<bool> prefix_cwd;
};

// ── Session Presentation ─────────────────────────────────────────────────────

struct ConfigOptionValue {
    std::string value;
    std::string name;
};

struct SessionConfigOption {
    std::string type;
    std::string id;
    std::string name;
    std::string category;
    std::string description;
    std::string current_value;
    std::vector<ConfigOptionValue> options;
};

struct SessionMode {
    std::string id;
    std::string name;
    std::string description;
};

struct SessionModeState {
    std::string current_mode_id;
    std::vector<SessionMode> available_modes;
};

struct SessionPresentation {
    std::vector<SessionConfigOption> config_options;
    SessionModeState modes;
};

// ── ACP Request/Response Types ───────────────────────────────────────────────

struct InitializeRequest {
    std::string client_name;
    std::string client_version;
};

// ✅ 对齐 OC translator.ts:503-524 `initialize` response
// ACP schema 要求:
//   protocolVersion: integer (uint16)
//   agentCapabilities: { loadSession, promptCapabilities, mcpCapabilities, sessionCapabilities }
//   agentInfo: { name, title, version }
//   authMethods: AuthMethod[]
struct AcpAgentCapabilities {
    bool load_session = true;                // supports loadSession
    // promptCapabilities
    bool prompt_image = true;
    bool prompt_audio = false;
    bool prompt_embedded_context = true;
    // mcpCapabilities
    bool mcp_http = false;
    bool mcp_sse = false;
    // sessionCapabilities: { list, resume, close } (empty object = supported)
    // 🎯 对齐 OC: sessionCapabilities = { list:{}, resume:{}, close:{} }
    bool session_list_supported = true;
  bool session_resume_supported = true;
    bool session_close_supported = true;
};

struct InitializeResponse {
    int protocol_version = ACP_PROTOCOL_VERSION;  // ✅ integer, not string
    AcpAgentInfo agent;                             // agentInfo
    AcpAgentCapabilities agent_capabilities;        // agentCapabilities
    std::vector<nlohmann::json> auth_methods;       // authMethods (empty by default)
    std::vector<nlohmann::json> commands;           // Legacy HC field; OC sends commands separately via sendAvailableCommands
};

struct AuthenticateRequest {
    std::string token;
};

struct AuthenticateResponse {
    bool authenticated = false;
};

struct NewSessionRequest {
    std::string cwd;
    nlohmann::json meta;
};

struct AcpSessionInfo {
    std::string session_id;
    std::string title;
    std::string updated_at;
};

struct NewSessionResponse {
    std::string session_id;
    std::vector<SessionConfigOption> config_options;
    SessionModeState modes;
};

struct ListSessionsRequest {
    int limit = 50;
};

struct ListSessionsResponse {
    std::vector<AcpSessionInfo> sessions;
};

struct LoadSessionRequest {
    std::string session_id;
    bool replay = false;
};

struct ReplayChunk {
    std::string session_update;
    std::string text;
};

struct TranscriptMessage {
    std::string role;
    std::string text_content;
    struct ContentBlock {
        std::string type;
        std::string text;
        std::string thinking;
    };
    std::vector<ContentBlock> content_blocks;
};

struct TranscriptResult {
    std::vector<TranscriptMessage> messages;
};

struct LoadSessionResponse {
    std::string session_id;
    std::vector<SessionConfigOption> config_options;
    SessionModeState modes;
    std::vector<ReplayChunk> replay_chunks;
};

struct PromptAttachment {
    std::string type;
    std::string mime_type;
    std::string content;
};

struct PromptRequest {
    std::string session_id;
    std::string text;
    std::string idempotency_key;
    std::vector<PromptAttachment> attachments;
};

struct PromptResponse {
    std::string stop_reason;
    std::string text;
};

struct SetSessionModeRequest {
    std::string session_id;
    std::string mode_id;
};

struct SetSessionModeResponse {
    SessionModeState modes;
    std::vector<SessionConfigOption> config_options;
};

struct SetSessionConfigOptionRequest {
    std::string session_id;
    std::string option_id;
    std::string value;
};

struct SetSessionConfigOptionResponse {
    std::vector<SessionConfigOption> config_options;
    SessionModeState modes;
};

// 🎯 对齐 OC resumeSession（translator.ts）: 与 loadSession 类似，
//   请求携带 sessionId/cwd/mcpServers/_meta，返回 { configOptions, modes }，
//   replay 通过 session/update 推送。
struct ResumeSessionRequest {
    std::string session_id;
    std::string cwd;
    nlohmann::json meta;
    bool replay = true;
};

struct ResumeSessionResponse {
    std::vector<SessionConfigOption> config_options;
    SessionModeState modes;
};

// 🎯 对齐 OC closeSession（translator.ts）: 请求携带 sessionId，
//   cancel 正在进行的 run 并释放 session，返回 {}。
struct CloseSessionRequest {
    std::string session_id;
};

struct CloseSessionResponse {
    // OC 返回 {}；此处保留空结构以对齐。
};

struct GatewaySessionRow {
    std::string session_id;
    std::string label;
    std::string derived_title;
    std::string updated_at;
    std::string thinking_level;
    bool fast_mode = false;
    std::string model_provider;
    std::string model;
    std::string verbose_level;
    std::string reasoning_level;
    std::string response_usage;
    std::string elevated_level;
    int64_t total_tokens = 0;
    int64_t context_tokens = 0;
};

struct SessionsListResult {
    std::vector<GatewaySessionRow> sessions;
};

struct ChatResult {
    std::string run_id;
    std::string stop_reason;
    std::string text;
};

using PromptStreamCallback = std::function<void(const std::string&, const std::string&)>;

// 🎯 prompt 派发回调（注入式，避免 acp 库直接依赖 gateway，防循环依赖）。
// 由 server 装配层提供：把一次 ACP prompt 派发到 gateway 的 chat 流程，
// 返回 { run_id, stop_reason, text }。派发期间的流式事件仍通过
// handle_gateway_event 回流到 translator 并转成 session/update。
// 入参：session_key, prompt_text, attachments(json array), idempotency_key。
using PromptDispatchFn = std::function<ChatResult(
    const std::string& session_key,
    const std::string& prompt_text,
    const nlohmann::json& attachments,
    const std::string& idempotency_key)>;

// 🎯 cancel 派发回调（注入式）：abort 指定 session_key/run_id 上的运行。
using CancelDispatchFn = std::function<void(
    const std::string& session_key, const std::string& run_id)>;

// 🎯 对齐 OC GatewayExecApprovalEvent（permission-relay.ts:11-17）。
struct GatewayApprovalEvent {
    std::string approval_id;
    std::string command;       // 可空
    std::string host;     // 可空
    std::string title;         // 可空
 std::string tool_call_id;  // 可空
};

// 🎯 approval 详情查询回调（注入式）：对齐 OC gateway.request("exec.approval.get")。
//   入参 approval_id，返回 { commandText?, commandPreview?, host?, allowedDecisions?[] } json。
using ApprovalDetailsFn = std::function<nlohmann::json(const std::string& approval_id)>;

// 🎯 approval 解决回调（注入式）：对齐 OC gateway.request("exec.approval.resolve")。
//   入参 approval_id + decision（"allow-once"|"allow-always"|"deny"），返回 true 表示成功。
using ApprovalResolveFn = std::function<bool(
    const std::string& approval_id, const std::string& decision)>;

// ── AcpGatewayAgent ──────────────────────────────────────────────────────────

class AcpGatewayAgent {
public:
    AcpGatewayAgent(gateway::GatewayClient* gw, const AcpServerOptions& opts,
          AcpSessionStore* store);
    ~AcpGatewayAgent();

    // 🎯 对齐 OC: AgentSideConnection 注入。用于:
    //   - session/update notification 回推（agent_message_chunk / tool_call / ...）
    //   - session/request_permission server→client request（权限链路）
    // 由 server 装配层在构造后注入。
    void set_connection(AcpStdioConnection* conn);

    // 🎯 注入 prompt / cancel 派发回调（由 server 装配层提供，接 gateway）。
    void set_prompt_dispatch(PromptDispatchFn fn);
 void set_cancel_dispatch(CancelDispatchFn fn);

    // 🎯 注入 approval 详情/解决回调（接 gateway 的 exec.approval.get/resolve）。
    // 用于 session/request_permission 反向权限链路。
    void set_approval_details(ApprovalDetailsFn fn);
void set_approval_resolve(ApprovalResolveFn fn);

    InitializeResponse handle_initialize(const InitializeRequest& req);
    AuthenticateResponse handle_authenticate(const AuthenticateRequest& req);
  NewSessionResponse handle_new_session(const NewSessionRequest& req);
    ListSessionsResponse handle_list_sessions(const ListSessionsRequest& req);
    LoadSessionResponse handle_load_session(const LoadSessionRequest& req);
 // 🎯 对齐 OC resumeSession / closeSession。
    ResumeSessionResponse handle_resume_session(const ResumeSessionRequest& req);
    CloseSessionResponse handle_close_session(const CloseSessionRequest& req);
 PromptResponse handle_prompt(const PromptRequest& req, PromptStreamCallback stream_cb = {});
    SetSessionModeResponse handle_set_session_mode(const SetSessionModeRequest& req);
    SetSessionConfigOptionResponse handle_set_session_config_option(
        const SetSessionConfigOptionRequest& req);

    // 🎯 对齐 OC: cancel(params) — session/cancel notification。
    // abort 指定 session 上正在进行的 run。
    void handle_cancel(const std::string& session_id);

  void handle_gateway_event(const nlohmann::json& evt);
    void handle_gateway_reconnect();
void handle_gateway_disconnect(const std::string& reason);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    SessionPresentation build_session_presentation_default();
    SessionConfigOption build_select_config_option(
        const std::string& id, const std::string& name, const std::string& category,
        const std::string& description, const std::string& current_value,
        const std::vector<std::string>& values);
    std::string format_thinking_level_name(const std::string& level);
    std::string build_thinking_mode_description(const std::string& level);
    std::string format_config_value_name(const std::string& value);
    std::string map_stop_reason(const std::string& gateway_reason);
    std::vector<ReplayChunk> extract_replay_chunks(const TranscriptMessage& msg);
    AcpSessionMeta_Request parse_session_meta(const nlohmann::json& meta);

    // 🎯 对齐 OC: sessionUpdates.emit({ sessionId, update })。
    // 通过注入的 connection 向 client 发 session/update notification。
    void emit_session_update(const std::string& session_id, const nlohmann::json& update);

    // 🎯 对齐 OC: sendAvailableCommands — 在 new/load/resume 后发
    // available_commands_update 通知。
    void emit_available_commands(const std::string& session_id);

    // 🎯 对齐 OC: startApprovalRelay + runApprovalRelay — 处理 Gateway exec approval，
    //   通过 session/request_permission 向 client 请求决策并回写 Gateway。
    void start_approval_relay(const std::string& session_key,
 const std::string& run_id,
        const GatewayApprovalEvent& event);
    void run_approval_relay(const std::string& session_id,
   const std::string& approval_id,
   const GatewayApprovalEvent& event);
};

// ── Available Commands (✅ 委托给 acp_commands.h) ─────────────────────────
// commands 定义统一在 acp_commands.h 中维护，此处为向前兼容保留声明。
// 实现体在 acp_commands.cc 中，通过 get_available_commands_vector() 提供。
// 调用方请使用 acp_commands.h 中的 get_available_commands_vector()。

} // namespace acp
} // namespace hunterclaw
