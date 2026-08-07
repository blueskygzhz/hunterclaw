#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace hunterclaw {
namespace plugins {

// Forward declarations
class PluginLogger;
class PluginRuntime;
class PluginRegistry;

// ============================================================================
// Plugin Types & Enums
// ============================================================================

enum class PluginKind {
  MEMORY,
  CONTEXT_ENGINE,
  GENERAL
};

enum class PluginOrigin {
  BUNDLED,
  GLOBAL,
  WORKSPACE,
  CONFIG
};

enum class PluginStatus {
  LOADED,
  DISABLED,
  ERROR
};

enum class PluginHookName {
  BEFORE_MODEL_RESOLVE,
  AGENT_TURN_PREPARE,
  BEFORE_PROMPT_BUILD,
  BEFORE_AGENT_START,
  BEFORE_AGENT_REPLY,
  MODEL_CALL_STARTED,
  MODEL_CALL_ENDED,
  LLM_INPUT,
  LLM_OUTPUT,
  BEFORE_AGENT_FINALIZE,
  AGENT_END,
  BEFORE_COMPACTION,
  AFTER_COMPACTION,
  BEFORE_RESET,
  INBOUND_CLAIM,
  CHANNEL_PAIRING_REQUESTED,
  MESSAGE_RECEIVED,
  MESSAGE_SENDING,
  REPLY_PAYLOAD_SENDING,
  MESSAGE_SENT,
  BEFORE_TOOL_CALL,
  AFTER_TOOL_CALL,
  TOOL_RESULT_PERSIST,
  BEFORE_MESSAGE_WRITE,
  SESSION_START,
  SESSION_END,
  SUBAGENT_SPAWNING,
  SUBAGENT_DELIVERY_TARGET,
  SUBAGENT_SPAWNED,
  SUBAGENT_PROGRESS,
  SUBAGENT_ENDED,
  DEACTIVATE,
  GATEWAY_START,
  GATEWAY_STOP,
  HEARTBEAT_PROMPT_CONTRIBUTION,
  CRON_RECONCILED,
  CRON_CHANGED,
  SKILL_PROPOSAL_EVALUATE,
  SKILL_PROPOSAL_CHANGED,
  SKILL_CHANGED,
  BEFORE_DISPATCH,
  REPLY_DISPATCH,
  BEFORE_INSTALL,
  BEFORE_AGENT_RUN,
  RESOLVE_EXEC_ENV
};

enum class HttpRouteAuth {
  GATEWAY,
  PLUGIN
};

enum class HttpRouteMatch {
  EXACT,
  PREFIX
};

// ============================================================================
// Plugin Logger
// ============================================================================

class PluginLogger {
 public:
  virtual ~PluginLogger() = default;

  virtual void debug(const std::string& message) = 0;
  virtual void info(const std::string& message) = 0;
  virtual void warn(const std::string& message) = 0;
  virtual void error(const std::string& message) = 0;
};

// Default logger implementation
class DefaultPluginLogger : public PluginLogger {
 public:
  void debug(const std::string& message) override;
  void info(const std::string& message) override;
  void warn(const std::string& message) override;
  void error(const std::string& message) override;
};

// ============================================================================
// Plugin Config & UI Hints
// ============================================================================

struct PluginConfigUiHint {
  std::optional<std::string> label;
  std::optional<std::string> help;
  std::vector<std::string> tags;
  bool advanced = false;
  bool sensitive = false;
  std::optional<std::string> placeholder;
};

struct PluginConfigValidation {
  bool ok;
  std::vector<std::string> errors;
  std::optional<std::string> value;  // JSON string
};

class PluginConfigSchema {
 public:
  virtual ~PluginConfigSchema() = default;

  virtual PluginConfigValidation validate(const std::string& value_json) const = 0;
  virtual std::unordered_map<std::string, PluginConfigUiHint> get_ui_hints() const = 0;
  virtual std::string get_json_schema() const = 0;  // Returns JSON string
};

// ============================================================================
// Plugin Tool Context & Factory
// ============================================================================

struct PluginToolContext {
  std::string config_json;  // OpenClawConfig as JSON
  std::optional<std::string> workspace_dir;
  std::optional<std::string> agent_dir;
  std::optional<std::string> agent_id;
  std::optional<std::string> session_key;
  std::optional<std::string> session_id;  // Ephemeral session UUID
  std::optional<std::string> message_channel;
  std::optional<std::string> agent_account_id;
  std::optional<std::string> requester_sender_id;
  bool sender_is_owner = false;
  bool sandboxed = false;
};

// Tool factory function type
using PluginToolFactory = std::function<std::vector<std::string>(const PluginToolContext&)>;

struct PluginToolOptions {
  std::optional<std::string> name;
  std::vector<std::string> names;
  bool optional = false;
};

// ============================================================================
// Plugin Hook — Agent Context (shared across agent hooks)
// ============================================================================

struct PluginHookAgentContext {
  std::optional<std::string> agent_id;
  std::optional<std::string> session_key;
  std::optional<std::string> session_id;
  std::optional<std::string> workspace_dir;
  std::optional<std::string> message_provider;
  /** What initiated this agent run: "user", "heartbeat", "cron", or "memory". */
  std::optional<std::string> trigger;
  /** Channel identifier (e.g. "telegram", "discord", "whatsapp"). */
  std::optional<std::string> channel_id;
};

// ============================================================================
// Plugin Hook — before_model_resolve
// ============================================================================

struct PluginHookBeforeModelResolveEvent {
  /** User prompt for this run. No session messages are available yet in this phase. */
  std::string prompt;
  /**
   * Attachment metadata for this run (JSON array string of {mime,kind,size,...}).
   * Aligned with OpenClaw PluginHookBeforeModelResolveEvent.attachments — enables
   * file-aware model routing (e.g. route image attachments to a vision model).
   */
  std::optional<std::string> attachments_json;
};

struct PluginHookBeforeModelResolveResult {
  /** Override the model for this agent run. E.g. "llama3.3:8b" */
  std::optional<std::string> model_override;
  /** Override the provider for this agent run. E.g. "ollama" */
  std::optional<std::string> provider_override;
};

// ============================================================================
// Plugin Hook — before_prompt_build
// ============================================================================

struct PluginHookBeforePromptBuildEvent {
  std::string prompt;
  /** Session messages prepared for this run (JSON array string). */
  std::string messages_json;
};

struct PluginHookBeforePromptBuildResult {
  std::optional<std::string> system_prompt;
  std::optional<std::string> prepend_context;
  /**
   * Prepended to the agent system prompt so providers can cache it.
   * Use for static plugin guidance instead of prepend_context.
   */
  std::optional<std::string> prepend_system_context;
  /**
   * Appended to the agent system prompt so providers can cache it.
   * Use for static plugin guidance instead of prepend_context.
   */
  std::optional<std::string> append_system_context;
  /**
   * Appended to the user-facing prompt context (tail). Aligned with OpenClaw
   * PluginHookBeforePromptBuildResult.appendContext.
   */
  std::optional<std::string> append_context;
  /**
   * Narrow the tool set available for THIS run. Aligned with OpenClaw
   * PluginHookBeforePromptBuildResult.toolsAllow. Semantics:
   *   - std::nullopt        : no constraint from this hook
   *   - present (may empty)  : allowed tool names; multiple hooks intersect.
   * An empty vector disables all optional tools for the run.
   */
  std::optional<std::vector<std::string>> tools_allow;
};

// ============================================================================
// Plugin Hook — agent_turn_prepare (modifying)
//   对齐 OpenClaw PluginAgentTurnPrepareEvent/Result (host-hook-turn-types.ts)。
//   多插件贡献 prepend/append context，按顺序拼接。
// ============================================================================

struct PluginHookAgentTurnPrepareEvent {
  std::string prompt;
  std::optional<std::string> messages_json;
  std::optional<std::string> queued_injections_json;
};

struct PluginHookAgentTurnPrepareResult {
  std::optional<std::string> prepend_context;
  std::optional<std::string> append_context;
};

// ============================================================================
// Plugin Hook — before_agent_finalize (modifying)
//   对齐 OpenClaw PluginHookBeforeAgentFinalizeEvent/Result (hook-types.ts:456)。
//   允许插件在自然 final reply 前请求再跑一轮模型（revise/retry）。
// ============================================================================

struct PluginHookBeforeAgentFinalizeEvent {
  std::optional<std::string> run_id;
  std::string session_id;
  std::optional<std::string> session_key;
  std::optional<std::string> turn_id;
  std::optional<std::string> provider;
  std::optional<std::string> model;
  std::optional<std::string> cwd;
  std::optional<std::string> transcript_path;
  bool stop_hook_active = false;
  std::optional<std::string> last_assistant_message;
  std::optional<std::string> messages_json;
};

struct PluginHookBeforeAgentFinalizeRetry {
  std::string instruction;
  std::optional<std::string> idempotency_key;
};

struct PluginHookBeforeAgentFinalizeResult {
  // "continue" | "revise" | "finalize"
  std::optional<std::string> action;
  std::optional<std::string> reason;
  std::optional<PluginHookBeforeAgentFinalizeRetry> retry;
};

// ============================================================================
// Plugin Hook — before_agent_run (gate)
//   对齐 OpenClaw PluginHookBeforeAgentRunEvent + InputGateDecision
//   (hook-types.ts:1206 / hook-decision-types.ts)。会话解析+工作区准备后、
//   模型推理前触发；返回最严格的 pass/block；void 视为 pass；fail-closed。
// ============================================================================

struct PluginHookBeforeAgentRunEvent {
  std::string prompt;
  std::optional<std::string> messages_json;
  std::optional<std::string> system_prompt;
  std::optional<std::string> account_id;
  std::optional<std::string> channel_id;
  std::optional<std::string> sender_id;
  std::optional<bool> sender_is_owner;
};

struct PluginHookBeforeAgentRunResult {
  // 对齐 InputGateDecision：默认 pass；block=true 表示拦截。
  bool block = false;
  std::optional<std::string> reason;     // 内部原因，勿外泄
  std::optional<std::string> message;    // 面向用户的可展示信息
  std::optional<std::string> category;   // 分析用类别
};

// ============================================================================
// Plugin Hook — before_agent_start (legacy: combines both phases)
// ============================================================================

struct PluginHookBeforeAgentStartEvent {
  std::string prompt;
  /** Optional because legacy hook can run in pre-session phase. */
  std::optional<std::string> messages_json;
};

struct PluginHookBeforeAgentStartResult {
  // From PluginHookBeforePromptBuildResult
  std::optional<std::string> system_prompt;
  std::optional<std::string> prepend_context;
  std::optional<std::string> prepend_system_context;
  std::optional<std::string> append_system_context;
  // From PluginHookBeforeModelResolveResult
  std::optional<std::string> model_override;
  std::optional<std::string> provider_override;
};

// ============================================================================
// Plugin Hook — before_agent_reply (claiming hook: short-circuit the reply)
//
// Aligned with OpenClaw:
//   PluginHookBeforeAgentReplyEvent  { cleanedBody }
//   PluginHookBeforeAgentReplyResult { handled, reply?, reason? }
//   runBeforeAgentReply = runClaimingHook (first {handled:true} wins)
//
// Runs BEFORE the LLM is invoked. If a handler returns handled==true, the
// entire agent turn is short-circuited and `reply_json` (a serialized
// ReplyPayload) is returned directly to the user; the LLM is never called.
// ============================================================================

struct PluginHookBeforeAgentReplyEvent {
  /** Cleaned inbound user text for this run (commands/markers stripped). */
  std::string cleaned_body;
};

struct PluginHookBeforeAgentReplyResult {
  /** When true, the turn is claimed and the LLM call is skipped. */
  bool handled = false;
  /** Serialized ReplyPayload (JSON) to deliver directly. Optional. */
  std::optional<std::string> reply_json;
  /** Optional human-readable reason for observability. */
  std::optional<std::string> reason;
};

// ============================================================================
// Plugin Hook — model_call_started / model_call_ended
//   对齐 OpenClaw PluginHookModelCallStartedEvent / PluginHookModelCallEndedEvent
//   (hook-types.ts:373-401)。观测型 void hook，包裹单次模型调用。
// ============================================================================

struct PluginHookModelCallStartedEvent {
  std::string run_id;
  std::string call_id;
  std::optional<std::string> session_key;
  std::optional<std::string> session_id;
  std::string provider;
  std::string model;
  std::optional<std::string> api;
  std::optional<std::string> transport;
  std::optional<int> context_token_budget;
  std::optional<std::string> context_window_source;
  std::optional<int> context_window_reference_tokens;
};

struct PluginHookModelCallEndedEvent {
  std::string run_id;
  std::string call_id;
  std::optional<std::string> session_key;
  std::optional<std::string> session_id;
  std::string provider;
  std::string model;
  std::optional<std::string> api;
  std::optional<std::string> transport;
  std::optional<int> context_token_budget;
  std::optional<std::string> context_window_source;
  std::optional<int> context_window_reference_tokens;
  int duration_ms = 0;
  std::string outcome;  // "completed" | "error"
  std::optional<std::string> error_category;
  std::optional<std::string> failure_kind;  // aborted|connection_closed|connection_reset|terminated|timeout
  std::optional<int> request_payload_bytes;
  std::optional<int> response_stream_bytes;
  std::optional<int> time_to_first_byte_ms;
  std::optional<std::string> upstream_request_id_hash;
};

// ============================================================================
// Plugin Hook — llm_input
// ============================================================================

struct PluginHookLlmInputEvent {
  std::string run_id;
  std::string session_id;
  std::string provider;
  std::string model;
  std::optional<std::string> system_prompt;
  std::string prompt;
  std::string history_messages_json;  // JSON array
  int images_count = 0;
};

// ============================================================================
// Plugin Hook — llm_output
// ============================================================================

struct PluginHookLlmOutputUsage {
  std::optional<int> input;
  std::optional<int> output;
  std::optional<int> cache_read;
  std::optional<int> cache_write;
  std::optional<int> total;
};

struct PluginHookLlmOutputEvent {
  std::string run_id;
  std::string session_id;
  std::string provider;
  std::string model;
  std::vector<std::string> assistant_texts;
  std::optional<std::string> last_assistant_json;
  std::optional<PluginHookLlmOutputUsage> usage;
};

// ============================================================================
// Plugin Hook — agent_end
// ============================================================================

struct PluginHookAgentEndEvent {
  std::string messages_json;  // JSON array
  bool success = false;
  std::optional<std::string> error;
  std::optional<int> duration_ms;
};

// ============================================================================
// Plugin Hook — before_compaction / after_compaction
// ============================================================================

struct PluginHookBeforeCompactionEvent {
  /** Total messages in the session before any truncation or compaction */
  int message_count = 0;
  /** Messages being fed to the compaction LLM (after history-limit truncation) */
  std::optional<int> compacting_count;
  std::optional<int> token_count;
  std::optional<std::string> messages_json;
  /** Path to the session JSONL transcript. */
  std::optional<std::string> session_file;
};

struct PluginHookAfterCompactionEvent {
  int message_count = 0;
  std::optional<int> token_count;
  int compacted_count = 0;
  /** Path to the session JSONL transcript. */
  std::optional<std::string> session_file;
};

// ============================================================================
// Plugin Hook — before_reset
// Aligned with OpenClaw session-reset-service.ts
// Fires before a session is reset; void hook (cannot cancel in OpenClaw).
// ============================================================================

struct PluginHookBeforeResetEvent {
  /** The stable session key (e.g. "main", "work"). */
  std::optional<std::string> session_key;
  /** The current (old) ephemeral session UUID, before reset. */
  std::optional<std::string> old_session_id;
  /** Reset reason: "reset" | "new". */
  std::optional<std::string> reason;
  /** Path to the session JSONL transcript (if available). */
  std::optional<std::string> session_file;
  /** Serialized current messages (if available). */
  std::optional<std::string> messages_json;
};

// ============================================================================
// Plugin Hook — message_received / message_sending / message_sent
// ============================================================================

struct PluginHookMessageContext {
  std::string channel_id;
  std::optional<std::string> account_id;
  std::optional<std::string> conversation_id;
};

struct PluginHookMessageReceivedEvent {
  std::string from;
  std::string content;
  std::optional<int64_t> timestamp;
  std::optional<std::string> metadata_json;
};

// ============================================================================
// Plugin Hook — inbound_claim (claiming)
//   对齐 OpenClaw PluginHookInboundClaimEvent/Result + Context
//   (hook-message.types.ts:92 / hook-types.ts:508)。
//   插件可在 commands/agent dispatch 之前认领入站事件（first-claim-wins）。
// ============================================================================

struct PluginHookInboundClaimEvent {
  std::string content;
  std::optional<std::string> body;
  std::optional<std::string> body_for_agent;
  std::optional<std::string> transcript;
  std::optional<int64_t> timestamp;
  std::string channel;
  std::optional<std::string> account_id;
  std::optional<std::string> conversation_id;
  std::optional<std::string> parent_conversation_id;
  std::optional<std::string> sender_id;
  std::optional<std::string> message_id;
};

struct PluginHookInboundClaimContext {
  std::string channel_id;
  std::optional<std::string> account_id;
  std::optional<std::string> conversation_id;
  std::optional<std::string> agent_id;
  std::optional<std::string> sender_id;
  std::optional<std::string> message_id;
};

struct PluginHookInboundClaimResult {
  bool handled = false;
  std::optional<std::string> reply_json;  // 序列化 ReplyPayload（可选直接投递）
};

// ============================================================================
// Plugin Hook — channel_pairing_requested (void 观测)
//   对齐 OpenClaw PluginHookChannelPairingRequestedEvent (hook-types.ts:206)。
//   创建 pending pairing 请求时通知 operator。metadata 视为不可信。
// ============================================================================

struct PluginHookChannelPairingRequestedEvent {
  std::string channel;
  std::optional<std::string> account_id;
  std::string sender_id;
  std::string code;
  std::optional<std::string> metadata_json;
};

// ============================================================================
// Plugin Hook — reply_payload_sending (modifying)
//   对齐 OpenClaw PluginHookReplyPayloadSendingEvent/Result (hook-types.ts:659)。
//   投递前顺序修改/取消规范化的 reply payload（每个 handler 拿到最新 payload）。
// ============================================================================

struct PluginHookReplyPayloadSendingEvent {
  std::string payload_json;              // 序列化 ReplyPayload
  std::string kind;                      // ReplyDispatchKind
  std::optional<std::string> channel;
  std::optional<std::string> session_key;
  std::optional<std::string> run_id;
  std::optional<std::string> usage_state_json;
};

struct PluginHookReplyPayloadSendingResult {
  std::optional<std::string> payload_json;  // 修改后的 payload
  bool cancel = false;
  std::optional<std::string> reason;
};

struct PluginHookMessageSendingEvent {
  std::string to;
  std::string content;
  std::optional<std::string> metadata_json;
};

struct PluginHookMessageSendingResult {
  std::optional<std::string> content;
  bool cancel = false;
};

struct PluginHookMessageSentEvent {
  std::string to;
  std::string content;
  bool success = false;
  std::optional<std::string> error;
};

// ============================================================================
// Plugin Hook — before_dispatch (claiming)
//   对齐 OpenClaw PluginHookBeforeDispatchEvent/Context/Result (hook-types.ts:513)。
//   model dispatch 之前，插件可检查/接管消息。first-claim-wins（{handled, text?}）。
// ============================================================================

struct PluginHookBeforeDispatchEvent {
  std::optional<std::string> message_id;
  std::string content;
  std::optional<std::string> body;
  std::optional<std::string> channel;
  std::optional<std::string> session_key;
  std::optional<std::string> sender_id;
  std::optional<bool> is_group;
  std::optional<int64_t> timestamp;
};

struct PluginHookBeforeDispatchContext {
  std::optional<std::string> message_id;
  std::optional<std::string> channel_id;
  std::optional<std::string> account_id;
  std::optional<std::string> conversation_id;
  std::optional<std::string> session_key;
  std::optional<std::string> sender_id;
};

struct PluginHookBeforeDispatchResult {
  bool handled = false;
  std::optional<std::string> text;
};

// ============================================================================
// Plugin Hook — reply_dispatch (claiming)
//   对齐 OpenClaw PluginHookReplyDispatchEvent/Context/Result (hook-types.ts:548)。
//   默认 model path 之前，插件可接管整个 reply dispatch。first-claim-wins。
//   event 字段众多，此处传核心可用字段。
// ============================================================================

struct PluginHookReplyDispatchEvent {
  std::optional<std::string> run_id;
  std::optional<std::string> session_key;
  std::optional<std::vector<std::string>> tools_allow;
  bool inbound_audio = false;
  std::optional<std::string> tts_channel;
  bool should_route_to_originating = false;
  std::optional<std::string> originating_channel;
  bool should_send_tool_summaries = false;
  std::string send_policy;  // "allow" | "deny"
  std::optional<bool> is_tail_dispatch;
};

struct PluginHookReplyDispatchContext {
  std::optional<std::string> channel_id;
  std::optional<std::string> account_id;
  std::optional<std::string> conversation_id;
  std::optional<std::string> session_key;
};

struct PluginHookReplyDispatchResult {
  bool handled = false;
  bool queued_final = false;
  std::optional<std::string> counts_json;  // Record<ReplyDispatchKind, number>
};

// ============================================================================
// Plugin Hook — before_tool_call / after_tool_call
// ============================================================================

struct PluginHookToolContext {
  std::optional<std::string> agent_id;
  std::optional<std::string> session_key;
  /** Ephemeral session UUID — regenerated on /new and /reset. */
  std::optional<std::string> session_id;
  /** Stable run identifier for this agent invocation. */
  std::optional<std::string> run_id;
  std::string tool_name;
  /** Provider-specific tool call ID when available. */
  std::optional<std::string> tool_call_id;
};

struct PluginHookBeforeToolCallEvent {
  std::string tool_name;
  std::string params_json;  // Record<string, unknown> as JSON
  /** Stable run identifier for this agent invocation. */
  std::optional<std::string> run_id;
  /** Provider-specific tool call ID when available. */
  std::optional<std::string> tool_call_id;
};

// Plugin approval request attached to a before_tool_call result.
// Aligned with OpenClaw PluginHookBeforeToolCallResult["requireApproval"].
struct PluginHookApprovalRequest {
  std::optional<std::string> plugin_id;
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<std::string> severity;                 // "info" | "warning" | "critical"
  std::vector<std::string> allowed_decisions;          // subset of allow_once/allow_always/deny
  std::optional<int64_t> timeout_ms;
  // OpenClaw: when set, a timeout is treated as a veto with this reason instead
  // of a failure disposition.
  std::optional<std::string> timeout_reason;
};

struct PluginHookBeforeToolCallResult {
  std::optional<std::string> params_json;  // Modified params as JSON
  bool block = false;
  std::optional<std::string> block_reason;
  // Aligned with OpenClaw: plugin may request approval instead of blocking.
  std::optional<PluginHookApprovalRequest> require_approval;
};

struct PluginHookAfterToolCallEvent {
  std::string tool_name;
  std::string params_json;
  /** Stable run identifier for this agent invocation. */
  std::optional<std::string> run_id;
  /** Provider-specific tool call ID when available. */
  std::optional<std::string> tool_call_id;
  std::optional<std::string> result_json;
  std::optional<std::string> error;
  std::optional<int> duration_ms;
};

// ============================================================================
// Plugin Hook — tool_result_persist
// ============================================================================

struct PluginHookToolResultPersistContext {
  std::optional<std::string> agent_id;
  std::optional<std::string> session_key;
  std::optional<std::string> tool_name;
  std::optional<std::string> tool_call_id;
};

struct PluginHookToolResultPersistEvent {
  std::optional<std::string> tool_name;
  std::optional<std::string> tool_call_id;
  /**
   * The toolResult message about to be written to the session transcript (JSON).
   * Handlers may return a modified message.
   */
  std::string message_json;
  /** True when the tool result was synthesized by a guard/repair step. */
  bool is_synthetic = false;
};

struct PluginHookToolResultPersistResult {
  std::optional<std::string> message_json;
};

// ============================================================================
// Plugin Hook — before_message_write
// ============================================================================

struct PluginHookBeforeMessageWriteEvent {
  std::string message_json;
  std::optional<std::string> session_key;
  std::optional<std::string> agent_id;
};

struct PluginHookBeforeMessageWriteContext {
  std::optional<std::string> agent_id;
  std::optional<std::string> session_key;
};

struct PluginHookBeforeMessageWriteResult {
  bool block = false;  // If true, message is NOT written to JSONL
  std::optional<std::string> message_json;  // Optional: modified message to write instead
};

// ============================================================================
// Plugin Hook — session_start / session_end
// ============================================================================

struct PluginHookSessionContext {
  std::optional<std::string> agent_id;
  std::string session_id;
  std::optional<std::string> session_key;
};

struct PluginHookSessionStartEvent {
  std::string session_id;
  std::optional<std::string> session_key;
  std::optional<std::string> resumed_from;
};

struct PluginHookSessionEndEvent {
  std::string session_id;
  std::optional<std::string> session_key;
  int message_count = 0;
  std::optional<int> duration_ms;
};

// ============================================================================
// Plugin Hook — subagent_spawning / subagent_delivery_target / subagent_spawned / subagent_ended
// ============================================================================

struct PluginHookSubagentContext {
  std::optional<std::string> run_id;
  std::optional<std::string> child_session_key;
  std::optional<std::string> requester_session_key;
};

struct PluginHookSubagentRequester {
  std::optional<std::string> channel;
  std::optional<std::string> account_id;
  std::optional<std::string> to;
  std::optional<std::string> thread_id;
};

struct PluginHookSubagentSpawnBase {
  std::string child_session_key;
  std::string agent_id;
  std::optional<std::string> label;
  std::string mode;  // "run" | "session"
  std::optional<PluginHookSubagentRequester> requester;
  bool thread_requested = false;
};

struct PluginHookSubagentSpawningEvent : PluginHookSubagentSpawnBase {};

struct PluginHookSubagentSpawningResult {
  bool ok = true;
  bool thread_binding_ready = false;
  std::optional<std::string> error;
};

struct PluginHookSubagentDeliveryTargetEvent {
  std::string child_session_key;
  std::string requester_session_key;
  std::optional<PluginHookSubagentRequester> requester_origin;
  std::optional<std::string> child_run_id;
  std::optional<std::string> spawn_mode;  // "run" | "session"
  bool expects_completion_message = false;
};

struct PluginHookSubagentDeliveryTargetResult {
  std::optional<PluginHookSubagentRequester> origin;
};

struct PluginHookSubagentSpawnedEvent : PluginHookSubagentSpawnBase {
  std::string run_id;
};

struct PluginHookSubagentEndedEvent {
  std::string target_session_key;
  std::string target_kind;  // "subagent" | "acp"
  std::string reason;
  bool send_farewell = false;
  std::optional<std::string> account_id;
  std::optional<std::string> run_id;
  std::optional<int64_t> ended_at;
  std::optional<std::string> outcome;  // "ok" | "error" | "timeout" | "killed" | "reset" | "deleted"
  std::optional<std::string> error;
};

// ============================================================================
// Plugin Hook — gateway_start / gateway_stop
// ============================================================================

struct PluginHookGatewayContext {
  std::optional<int> port;
};

struct PluginHookGatewayStartEvent {
  int port = 0;
};

struct PluginHookGatewayStopEvent {
  std::optional<std::string> reason;
};

// ============================================================================
// Plugin Hook Handler & Options
// ============================================================================

struct PluginHookEvent {
  std::string event_type;
  std::string data_json;  // Event data as JSON
};

// Generic hook handler (used internally for registration)
using PluginHookHandler = std::function<void(const PluginHookEvent&, const std::string& ctx_json)>;

struct PluginHookOptions {
  std::optional<std::string> entry;
  std::optional<std::string> name;
  std::optional<std::string> description;
  bool register_hook = true;
  int priority = 0;
};

// ============================================================================
// Plugin HTTP Route
// ============================================================================

using PluginHttpRouteHandler = std::function<bool(const std::string& request_json, std::string& response_json)>;

struct PluginHttpRouteParams {
  std::string path;
  PluginHttpRouteHandler handler;
  HttpRouteAuth auth = HttpRouteAuth::GATEWAY;
  HttpRouteMatch match = HttpRouteMatch::EXACT;
  bool replace_existing = false;
};

// ============================================================================
// Plugin Command
// ============================================================================

struct PluginCommandContext {
  /** The sender's identifier (e.g., Telegram user ID) */
  std::optional<std::string> sender_id;
  /** The channel/surface (e.g., "telegram", "discord") */
  std::string channel;
  /** Provider channel id (e.g., "telegram") */
  std::optional<std::string> channel_id;
  /** Whether the sender is on the allowlist */
  bool is_authorized_sender = false;
  /** Raw command arguments after the command name */
  std::optional<std::string> args;
  /** The full normalized command body */
  std::string command_body;
  /** Current config as JSON */
  std::string config_json;
  /** Raw "From" value (channel-scoped id) */
  std::optional<std::string> from;
  /** Raw "To" value (channel-scoped id) */
  std::optional<std::string> to;
  /** Account id for multi-account channels */
  std::optional<std::string> account_id;
  /** Thread/topic id if available */
  std::optional<int> message_thread_id;
};

struct PluginCommandResult {
  std::string payload_json;  // ReplyPayload as JSON
};

using PluginCommandHandler = std::function<PluginCommandResult(const PluginCommandContext&)>;

struct PluginCommandDefinition {
  std::string name;  // Without leading slash
  std::unordered_map<std::string, std::string> native_names;  // Provider-specific aliases
  std::string description;
  bool accepts_args = false;
  bool require_auth = true;
  PluginCommandHandler handler;
};

// ============================================================================
// Plugin Service
// ============================================================================

struct PluginServiceContext {
  std::string config_json;
  std::optional<std::string> workspace_dir;
  std::string state_dir;
  std::shared_ptr<PluginLogger> logger;
};

class PluginService {
 public:
  virtual ~PluginService() = default;

  std::string id;
  virtual void start(const PluginServiceContext& ctx) = 0;
  virtual void stop(const PluginServiceContext& ctx) = 0;
};

// ============================================================================
// Plugin Diagnostic
// ============================================================================

struct PluginDiagnostic {
  enum class Level { WARN, ERROR };

  Level level;
  std::string message;
  std::optional<std::string> plugin_id;
  std::optional<std::string> source;
};

// ============================================================================
// Plugin API
// ============================================================================

class PluginApi {
 public:
  virtual ~PluginApi() = default;

  std::string id;
  std::string name;
  std::optional<std::string> version;
  std::optional<std::string> description;
  std::string source;
  std::string config_json;  // OpenClawConfig as JSON
  std::optional<std::string> plugin_config_json;  // Plugin-specific config
  std::shared_ptr<PluginRuntime> runtime;
  std::shared_ptr<PluginLogger> logger;

  virtual void register_tool(PluginToolFactory factory, const PluginToolOptions& opts = {}) = 0;
  virtual void register_hook(const std::vector<PluginHookName>& events, PluginHookHandler handler, const PluginHookOptions& opts = {}) = 0;
  virtual void register_http_route(const PluginHttpRouteParams& params) = 0;
  virtual void register_gateway_method(const std::string& method, std::function<void()> handler) = 0;
  virtual void register_service(std::shared_ptr<PluginService> service) = 0;
  virtual void register_command(const PluginCommandDefinition& command) = 0;
  virtual void register_context_engine(const std::string& id, std::function<void()> factory) = 0;
  virtual void register_channel(const std::string& channel_id) = 0;  // 对齐 OpenClaw: api.registerChannel
  virtual std::string resolve_path(const std::string& input) const = 0;

  /** Register a lifecycle hook handler (typed, mirrors OpenClaw api.on()) */
  virtual void on(PluginHookName hook_name, PluginHookHandler handler, int priority = 0) = 0;
};

// ============================================================================
// Plugin Definition
// ============================================================================

using PluginRegisterFunc = std::function<void(PluginApi&)>;
using PluginActivateFunc = std::function<void(PluginApi&)>;

struct PluginDefinition {
  std::optional<std::string> id;
  std::optional<std::string> name;
  std::optional<std::string> description;
  std::optional<std::string> version;
  std::optional<PluginKind> kind;
  std::shared_ptr<PluginConfigSchema> config_schema;
  std::optional<PluginRegisterFunc> register_func;
  std::optional<PluginActivateFunc> activate_func;
};

// ============================================================================
// Plugin Registrations
// ============================================================================

struct PluginToolRegistration {
  std::string plugin_id;
  PluginToolFactory factory;
  std::vector<std::string> names;
  bool optional;
  std::string source;
};

struct PluginHookRegistration {
  std::string plugin_id;
  std::vector<PluginHookName> events;
  PluginHookHandler handler;
  std::string source;
  int priority = 0;
};

struct PluginHttpRouteRegistration {
  std::string plugin_id;
  std::string path;
  PluginHttpRouteHandler handler;
  HttpRouteAuth auth;
  HttpRouteMatch match;
  std::string source;
};

struct PluginServiceRegistration {
  std::string plugin_id;
  std::shared_ptr<PluginService> service;
  std::string source;
};

struct PluginCommandRegistration {
  std::string plugin_id;
  PluginCommandDefinition command;
  std::string source;
};

// ============================================================================
// Plugin Channel Registration (对齐 OpenClaw PluginChannelRegistration)
// ============================================================================

// Forward declaration — full ChannelPlugin type defined in channels/plugin.h
// We store an opaque JSON descriptor here to avoid circular dependency.
// The actual ChannelPlugin is registered via ChannelPluginRegistry at load time.
struct PluginChannelRegistration {
  std::string plugin_id;
  std::string channel_id;
  std::string source;
  // Note: The actual ChannelPlugin descriptor is stored in ChannelPluginRegistry.
  // This record tracks which plugin registered which channel for bookkeeping.
};

// ============================================================================
// Plugin Record
// ============================================================================

struct PluginRecord {
  std::string id;
  std::string name;
  std::optional<std::string> version;
  std::optional<std::string> description;
  std::optional<PluginKind> kind;
  std::string source;
  PluginOrigin origin;
  std::optional<std::string> workspace_dir;
  bool enabled;
  PluginStatus status;
  std::optional<std::string> error;

  // Registration counts
  std::vector<std::string> tool_names;
  std::vector<PluginHookName> hook_names;
  std::vector<std::string> channel_ids;
  std::vector<std::string> provider_ids;
  std::vector<std::string> gateway_methods;
  std::vector<std::string> cli_commands;
  std::vector<std::string> services;
  std::vector<std::string> commands;
  int http_routes = 0;
  int hook_count = 0;

  bool config_schema = false;
  std::unordered_map<std::string, PluginConfigUiHint> config_ui_hints;
  std::optional<std::string> config_json_schema;
};

// ============================================================================
// Plugin Registry
// ============================================================================

class PluginRegistry {
 public:
  PluginRegistry();
  ~PluginRegistry();

  // Registration methods
  void register_plugin(const PluginRecord& record);
  void register_tool(const PluginToolRegistration& reg);
  void register_hook(const PluginHookRegistration& reg);
  void register_http_route(const PluginHttpRouteRegistration& reg);
  void register_service(const PluginServiceRegistration& reg);
  void register_command(const PluginCommandRegistration& reg);
  void register_channel(const PluginChannelRegistration& reg);  // 对齐 OpenClaw: registry.channels
  void add_diagnostic(const PluginDiagnostic& diag);

  // Query methods
  std::vector<PluginRecord> list_plugins() const;
  std::optional<PluginRecord> get_plugin(const std::string& plugin_id) const;
  std::vector<PluginToolRegistration> list_tools() const;
  std::vector<PluginHookRegistration> list_hooks() const;
  std::vector<PluginHookRegistration> list_hooks_for_event(PluginHookName event) const;
  std::vector<PluginHttpRouteRegistration> list_http_routes() const;
  std::vector<PluginServiceRegistration> list_services() const;
  std::vector<PluginCommandRegistration> list_commands() const;
  std::vector<PluginChannelRegistration> list_channels() const;  // 对齐 OpenClaw
  std::vector<PluginDiagnostic> get_diagnostics() const;

  // Clear
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Plugin Loader
// ============================================================================

struct PluginLoadOptions {
  std::optional<std::string> config_json;  // OpenClawConfig as JSON
  std::optional<std::string> workspace_dir;
  std::shared_ptr<PluginLogger> logger;
  bool cache = true;
  enum class Mode { FULL, VALIDATE } mode = Mode::FULL;
};

class PluginLoader {
 public:
  PluginLoader();
  ~PluginLoader();

  // Load plugins
  std::shared_ptr<PluginRegistry> load_plugins(const PluginLoadOptions& options = {});

  // Discovery
  std::vector<std::string> discover_bundled_plugins() const;
  std::vector<std::string> discover_workspace_plugins(const std::string& workspace_dir) const;
  std::vector<std::string> discover_global_plugins() const;

  // Cache management
  void clear_cache();
  void invalidate_cache(const std::string& cache_key);

  // Plugin loading
  bool load_plugin_from_path(const std::string& path, PluginRegistry& registry);
  bool load_plugin_definition(const PluginDefinition& def, PluginRegistry& registry, const std::string& source);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Global Plugin Registry Management
// ============================================================================

// Set active plugin registry (thread-safe)
void set_active_plugin_registry(std::shared_ptr<PluginRegistry> registry, const std::string& cache_key = "");

// Get active plugin registry (thread-safe)
std::shared_ptr<PluginRegistry> get_active_plugin_registry();

// Require active plugin registry (throws if not set)
std::shared_ptr<PluginRegistry> require_active_plugin_registry();

// Clear active plugin registry
void clear_active_plugin_registry();

// ============================================================================
// Utility Functions
// ============================================================================

// Convert enum to string
std::string plugin_kind_to_string(PluginKind kind);
std::string plugin_origin_to_string(PluginOrigin origin);
std::string plugin_status_to_string(PluginStatus status);
std::string plugin_hook_name_to_string(PluginHookName hook);
std::string http_route_auth_to_string(HttpRouteAuth auth);
std::string http_route_match_to_string(HttpRouteMatch match);

// Convert string to enum
std::optional<PluginKind> string_to_plugin_kind(const std::string& str);
std::optional<PluginOrigin> string_to_plugin_origin(const std::string& str);
std::optional<PluginStatus> string_to_plugin_status(const std::string& str);
std::optional<PluginHookName> string_to_plugin_hook_name(const std::string& str);
std::optional<HttpRouteAuth> string_to_http_route_auth(const std::string& str);
std::optional<HttpRouteMatch> string_to_http_route_match(const std::string& str);

}  // namespace plugins
}  // namespace hunterclaw
