#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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
// Plugin Hook Handler
// ============================================================================

struct PluginHookEvent {
  std::string event_type;
  std::string data_json;  // Event data as JSON
};

struct PluginHookContext {
  std::string config_json;
  std::optional<std::string> workspace_dir;
  std::optional<std::string> agent_id;
};

using PluginHookHandler = std::function<void(const PluginHookEvent&, const PluginHookContext&)>;

struct PluginHookOptions {
  std::optional<std::string> name;
  std::optional<std::string> description;
  bool register_immediately = true;
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
  std::optional<std::string> sender_id;
  std::string channel;
  std::optional<std::string> channel_id;
  bool is_authorized_sender = false;
  std::optional<std::string> args;
  std::string command_body;
  std::string config_json;
  std::optional<std::string> from;
  std::optional<std::string> to;
  std::optional<std::string> account_id;
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
