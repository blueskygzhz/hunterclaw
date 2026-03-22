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
  BEFORE_PROMPT_BUILD,
  BEFORE_AGENT_START,
  LLM_INPUT,
  LLM_OUTPUT,
  AGENT_END,
  BEFORE_COMPACTION,
  AFTER_COMPACTION,
  BEFORE_RESET,
  MESSAGE_RECEIVED,
  MESSAGE_SENDING,
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
  SUBAGENT_ENDED,
  GATEWAY_START,
  GATEWAY_STOP
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

struct PluginHookBeforeToolCallResult {
  std::optional<std::string> params_json;  // Modified params as JSON
  bool block = false;
  std::optional<std::string> block_reason;
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
