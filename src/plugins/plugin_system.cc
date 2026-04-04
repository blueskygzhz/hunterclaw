#include "hunterclaw/plugins/plugin_system.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace hunterclaw {
namespace plugins {

// ============================================================================
// DefaultPluginLogger Implementation
// ============================================================================

void DefaultPluginLogger::debug(const std::string& message) {
  std::cout << "[PLUGIN DEBUG] " << message << std::endl;
}

void DefaultPluginLogger::info(const std::string& message) {
  std::cout << "[PLUGIN INFO] " << message << std::endl;
}

void DefaultPluginLogger::warn(const std::string& message) {
  std::cerr << "[PLUGIN WARN] " << message << std::endl;
}

void DefaultPluginLogger::error(const std::string& message) {
  std::cerr << "[PLUGIN ERROR] " << message << std::endl;
}

// ============================================================================
// PluginRegistry Implementation
// ============================================================================

struct PluginRegistry::Impl {
  std::vector<PluginRecord> plugins;
  std::vector<PluginToolRegistration> tools;
  std::vector<PluginHookRegistration> hooks;
  std::vector<PluginHttpRouteRegistration> http_routes;
  std::vector<PluginServiceRegistration> services;
  std::vector<PluginCommandRegistration> commands;
  std::vector<PluginChannelRegistration> channels;  // 对齐 OpenClaw: registry.channels
  std::vector<PluginDiagnostic> diagnostics;
  mutable std::mutex mutex;
};

PluginRegistry::PluginRegistry() : impl_(std::make_unique<Impl>()) {}
PluginRegistry::~PluginRegistry() = default;

void PluginRegistry::register_plugin(const PluginRecord& record) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  // Check if plugin already exists
  auto it = std::find_if(impl_->plugins.begin(), impl_->plugins.end(),
                         [&](const PluginRecord& p) { return p.id == record.id; });
  
  if (it != impl_->plugins.end()) {
    // Update existing plugin
    *it = record;
  } else {
    // Add new plugin
    impl_->plugins.push_back(record);
  }
}

void PluginRegistry::register_tool(const PluginToolRegistration& reg) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->tools.push_back(reg);
}

void PluginRegistry::register_hook(const PluginHookRegistration& reg) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  // Insert sorted by priority (higher priority first)
  auto insert_pos = std::find_if(impl_->hooks.begin(), impl_->hooks.end(),
                                  [&](const PluginHookRegistration& h) {
                                    return h.priority < reg.priority;
                                  });
  impl_->hooks.insert(insert_pos, reg);
}

void PluginRegistry::register_http_route(const PluginHttpRouteRegistration& reg) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->http_routes.push_back(reg);
}

void PluginRegistry::register_service(const PluginServiceRegistration& reg) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->services.push_back(reg);
}

void PluginRegistry::register_command(const PluginCommandRegistration& reg) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->commands.push_back(reg);
}

// 对齐 OpenClaw: registerChannel — 记录 channel 注册到 registry.channels
void PluginRegistry::register_channel(const PluginChannelRegistration& reg) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  // 检查重复注册 (对齐 OpenClaw: reject duplicate channel ids)
  auto it = std::find_if(impl_->channels.begin(), impl_->channels.end(),
                         [&](const PluginChannelRegistration& c) { return c.channel_id == reg.channel_id; });
  if (it != impl_->channels.end()) {
    PluginDiagnostic diag;
    diag.level = PluginDiagnostic::Level::ERROR;
    diag.plugin_id = reg.plugin_id;
    diag.source = reg.source;
    diag.message = "channel already registered: " + reg.channel_id + " (" + it->plugin_id + ")";
    impl_->diagnostics.push_back(diag);
    return;
  }
  impl_->channels.push_back(reg);
}

void PluginRegistry::add_diagnostic(const PluginDiagnostic& diag) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->diagnostics.push_back(diag);
}

std::vector<PluginRecord> PluginRegistry::list_plugins() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->plugins;
}

std::optional<PluginRecord> PluginRegistry::get_plugin(const std::string& plugin_id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  auto it = std::find_if(impl_->plugins.begin(), impl_->plugins.end(),
                         [&](const PluginRecord& p) { return p.id == plugin_id; });
  
  if (it != impl_->plugins.end()) {
    return *it;
  }
  return std::nullopt;
}

std::vector<PluginToolRegistration> PluginRegistry::list_tools() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->tools;
}

std::vector<PluginHookRegistration> PluginRegistry::list_hooks() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->hooks;
}

std::vector<PluginHookRegistration> PluginRegistry::list_hooks_for_event(PluginHookName event) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  std::vector<PluginHookRegistration> result;
  for (const auto& hook : impl_->hooks) {
    if (std::find(hook.events.begin(), hook.events.end(), event) != hook.events.end()) {
      result.push_back(hook);
    }
  }
  return result;
}

std::vector<PluginHttpRouteRegistration> PluginRegistry::list_http_routes() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->http_routes;
}

std::vector<PluginServiceRegistration> PluginRegistry::list_services() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->services;
}

std::vector<PluginCommandRegistration> PluginRegistry::list_commands() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->commands;
}

std::vector<PluginChannelRegistration> PluginRegistry::list_channels() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->channels;
}

std::vector<PluginDiagnostic> PluginRegistry::get_diagnostics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->diagnostics;
}

void PluginRegistry::clear() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->plugins.clear();
  impl_->tools.clear();
  impl_->hooks.clear();
  impl_->http_routes.clear();
  impl_->services.clear();
  impl_->commands.clear();
  impl_->channels.clear();
  impl_->diagnostics.clear();
}

// ============================================================================
// PluginLoader Implementation
// ============================================================================

struct PluginLoader::Impl {
  std::unordered_map<std::string, std::shared_ptr<PluginRegistry>> cache;
  std::mutex cache_mutex;
};

PluginLoader::PluginLoader() : impl_(std::make_unique<Impl>()) {}
PluginLoader::~PluginLoader() = default;

std::shared_ptr<PluginRegistry> PluginLoader::load_plugins(const PluginLoadOptions& options) {
  std::string cache_key = options.workspace_dir.value_or("") + ":" + 
                          (options.cache ? "cached" : "no-cache");
  
  // Check cache
  if (options.cache) {
    std::lock_guard<std::mutex> lock(impl_->cache_mutex);
    auto it = impl_->cache.find(cache_key);
    if (it != impl_->cache.end()) {
      if (options.logger) {
        options.logger->debug("Loading plugins from cache: " + cache_key);
      }
      return it->second;
    }
  }
  
  // Create new registry
  auto registry = std::make_shared<PluginRegistry>();
  
  if (options.logger) {
    options.logger->info("Loading OpenClaw plugins...");
  }
  
  // Discover plugins
  std::vector<std::string> plugin_paths;
  
  // 1. Bundled plugins
  auto bundled = discover_bundled_plugins();
  plugin_paths.insert(plugin_paths.end(), bundled.begin(), bundled.end());
  
  if (options.logger) {
    options.logger->debug("Found " + std::to_string(bundled.size()) + " bundled plugins");
  }
  
  // 2. Workspace plugins
  if (options.workspace_dir) {
    auto workspace = discover_workspace_plugins(*options.workspace_dir);
    plugin_paths.insert(plugin_paths.end(), workspace.begin(), workspace.end());
    
    if (options.logger) {
      options.logger->debug("Found " + std::to_string(workspace.size()) + " workspace plugins");
    }
  }
  
  // 3. Global plugins
  auto global = discover_global_plugins();
  plugin_paths.insert(plugin_paths.end(), global.begin(), global.end());
  
  if (options.logger) {
    options.logger->debug("Found " + std::to_string(global.size()) + " global plugins");
  }
  
  // Load plugins
  int loaded_count = 0;
  int error_count = 0;
  
  for (const auto& path : plugin_paths) {
    if (load_plugin_from_path(path, *registry)) {
      loaded_count++;
    } else {
      error_count++;
      
      PluginDiagnostic diag;
      diag.level = PluginDiagnostic::Level::ERROR;
      diag.message = "Failed to load plugin from path: " + path;
      registry->add_diagnostic(diag);
    }
  }
  
  if (options.logger) {
    options.logger->info("Loaded " + std::to_string(loaded_count) + " plugins, " + 
                         std::to_string(error_count) + " errors");
  }
  
  // Cache if requested
  if (options.cache) {
    std::lock_guard<std::mutex> lock(impl_->cache_mutex);
    impl_->cache[cache_key] = registry;
  }
  
  // Set as active registry
  set_active_plugin_registry(registry, cache_key);
  
  return registry;
}

std::vector<std::string> PluginLoader::discover_bundled_plugins() const {
  std::vector<std::string> plugins;
  
  // Look for bundled plugins in common locations
  std::vector<std::string> search_paths = {
    "/usr/lib/hunterclaw/plugins",
    "/usr/local/lib/hunterclaw/plugins",
    "./plugins",
    "../plugins"
  };
  
  for (const auto& base_path : search_paths) {
    if (std::filesystem::exists(base_path)) {
      for (const auto& entry : std::filesystem::directory_iterator(base_path)) {
        if (entry.is_directory()) {
          plugins.push_back(entry.path().string());
        }
      }
    }
  }
  
  return plugins;
}

std::vector<std::string> PluginLoader::discover_workspace_plugins(const std::string& workspace_dir) const {
  std::vector<std::string> plugins;
  
  std::string plugins_dir = workspace_dir + "/.hunterclaw/plugins";
  if (std::filesystem::exists(plugins_dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(plugins_dir)) {
      if (entry.is_directory()) {
        plugins.push_back(entry.path().string());
      }
    }
  }
  
  return plugins;
}

std::vector<std::string> PluginLoader::discover_global_plugins() const {
  std::vector<std::string> plugins;
  
  // Look in home directory
  const char* home = std::getenv("HOME");
  if (home) {
    std::string global_plugins_dir = std::string(home) + "/.hunterclaw/plugins";
    if (std::filesystem::exists(global_plugins_dir)) {
      for (const auto& entry : std::filesystem::directory_iterator(global_plugins_dir)) {
        if (entry.is_directory()) {
          plugins.push_back(entry.path().string());
        }
      }
    }
  }
  
  return plugins;
}

void PluginLoader::clear_cache() {
  std::lock_guard<std::mutex> lock(impl_->cache_mutex);
  impl_->cache.clear();
}

void PluginLoader::invalidate_cache(const std::string& cache_key) {
  std::lock_guard<std::mutex> lock(impl_->cache_mutex);
  impl_->cache.erase(cache_key);
}

bool PluginLoader::load_plugin_from_path(const std::string& path, PluginRegistry& registry) {
  // Check if plugin manifest exists
  std::string manifest_path = path + "/plugin.json";
  if (!std::filesystem::exists(manifest_path)) {
    return false;
  }
  
  // For now, just create a basic plugin record
  // In a real implementation, you would parse the plugin.json and load the plugin
  PluginRecord record;
  record.id = std::filesystem::path(path).filename().string();
  record.name = record.id;
  record.source = path;
  record.origin = PluginOrigin::WORKSPACE;
  record.enabled = true;
  record.status = PluginStatus::LOADED;
  
  registry.register_plugin(record);
  
  return true;
}

bool PluginLoader::load_plugin_definition(const PluginDefinition& def, PluginRegistry& registry, const std::string& source) {
  // Create plugin record
  PluginRecord record;
  record.id = def.id.value_or("unknown");
  record.name = def.name.value_or(record.id);
  record.version = def.version;
  record.description = def.description;
  record.kind = def.kind;
  record.source = source;
  record.origin = PluginOrigin::BUNDLED;
  record.enabled = true;
  record.status = PluginStatus::LOADED;
  
  if (def.config_schema) {
    record.config_schema = true;
    record.config_ui_hints = def.config_schema->get_ui_hints();
    record.config_json_schema = def.config_schema->get_json_schema();
  }
  
  registry.register_plugin(record);
  
  // ✅ 对齐 OpenClaw: 调用 register_func 和 activate_func
  // OpenClaw: plugin.register(api) — api 提供 registerChannel/registerTool 等方法
  // HunterClaw: create a lightweight PluginApi that delegates to registry
  if (def.register_func || def.activate_func) {
    // Create a simple PluginApi implementation that delegates to the registry
    class BundledPluginApi : public PluginApi {
    public:
      PluginRegistry& registry_;
      std::string plugin_id_;
      std::string source_;
      
      BundledPluginApi(PluginRegistry& reg, const std::string& id, const std::string& src)
          : registry_(reg), plugin_id_(id), source_(src) {
        this->id = id;
        this->name = id;
        this->source = src;
      }
      
      void register_tool(PluginToolFactory, const PluginToolOptions&) override {}
      void register_hook(const std::vector<PluginHookName>&, PluginHookHandler, const PluginHookOptions&) override {}
      void register_http_route(const PluginHttpRouteParams&) override {}
      void register_gateway_method(const std::string&, std::function<void()>) override {}
      void register_service(std::shared_ptr<PluginService>) override {}
      void register_command(const PluginCommandDefinition&) override {}
      void register_context_engine(const std::string&, std::function<void()>) override {}
      std::string resolve_path(const std::string& input) const override { return input; }
      void on(PluginHookName, PluginHookHandler, int) override {}
      
      // 对齐 OpenClaw: api.registerChannel({ plugin }) → registry.channels.push(...)
      void register_channel(const std::string& channel_id) override {
        PluginChannelRegistration reg;
        reg.plugin_id = plugin_id_;
        reg.channel_id = channel_id;
        reg.source = source_;
        registry_.register_channel(reg);
        
        // Also update the PluginRecord.channel_ids for bookkeeping
        auto plugin_opt = registry_.get_plugin(plugin_id_);
        if (plugin_opt.has_value()) {
          auto updated = *plugin_opt;
          updated.channel_ids.push_back(channel_id);
          registry_.register_plugin(updated);  // re-register with updated channel_ids
        }
      }
    };
    
    BundledPluginApi api(registry, record.id, source);
    
    if (def.register_func) {
      try {
        (*def.register_func)(api);
      } catch (const std::exception& e) {
        PluginDiagnostic diag;
        diag.level = PluginDiagnostic::Level::ERROR;
        diag.plugin_id = record.id;
        diag.message = std::string("register_func failed: ") + e.what();
        registry.add_diagnostic(diag);
      }
    }
    if (def.activate_func) {
      try {
        (*def.activate_func)(api);
      } catch (const std::exception& e) {
        PluginDiagnostic diag;
        diag.level = PluginDiagnostic::Level::ERROR;
        diag.plugin_id = record.id;
        diag.message = std::string("activate_func failed: ") + e.what();
        registry.add_diagnostic(diag);
      }
    }
  }
  
  return true;
}

// ============================================================================
// Global Plugin Registry Management
// ============================================================================

namespace {
struct GlobalRegistryState {
  std::shared_ptr<PluginRegistry> registry;
  std::string cache_key;
  std::mutex mutex;
};

GlobalRegistryState& get_global_registry_state() {
  static GlobalRegistryState state;
  return state;
}
}  // namespace

void set_active_plugin_registry(std::shared_ptr<PluginRegistry> registry, const std::string& cache_key) {
  auto& state = get_global_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.registry = registry;
  state.cache_key = cache_key;
}

std::shared_ptr<PluginRegistry> get_active_plugin_registry() {
  auto& state = get_global_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.registry;
}

std::shared_ptr<PluginRegistry> require_active_plugin_registry() {
  auto registry = get_active_plugin_registry();
  if (!registry) {
    throw std::runtime_error("No active plugin registry set");
  }
  return registry;
}

void clear_active_plugin_registry() {
  auto& state = get_global_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.registry.reset();
  state.cache_key.clear();
}

// ============================================================================
// Utility Functions - Enum to String
// ============================================================================

std::string plugin_kind_to_string(PluginKind kind) {
  switch (kind) {
    case PluginKind::MEMORY: return "memory";
    case PluginKind::CONTEXT_ENGINE: return "context-engine";
    case PluginKind::GENERAL: return "general";
  }
  return "unknown";
}

std::string plugin_origin_to_string(PluginOrigin origin) {
  switch (origin) {
    case PluginOrigin::BUNDLED: return "bundled";
    case PluginOrigin::GLOBAL: return "global";
    case PluginOrigin::WORKSPACE: return "workspace";
    case PluginOrigin::CONFIG: return "config";
  }
  return "unknown";
}

std::string plugin_status_to_string(PluginStatus status) {
  switch (status) {
    case PluginStatus::LOADED: return "loaded";
    case PluginStatus::DISABLED: return "disabled";
    case PluginStatus::ERROR: return "error";
  }
  return "unknown";
}

std::string plugin_hook_name_to_string(PluginHookName hook) {
  switch (hook) {
    case PluginHookName::BEFORE_MODEL_RESOLVE: return "before_model_resolve";
    case PluginHookName::BEFORE_PROMPT_BUILD: return "before_prompt_build";
    case PluginHookName::BEFORE_AGENT_START: return "before_agent_start";
    case PluginHookName::LLM_INPUT: return "llm_input";
    case PluginHookName::LLM_OUTPUT: return "llm_output";
    case PluginHookName::AGENT_END: return "agent_end";
    case PluginHookName::BEFORE_COMPACTION: return "before_compaction";
    case PluginHookName::AFTER_COMPACTION: return "after_compaction";
    case PluginHookName::BEFORE_RESET: return "before_reset";
    case PluginHookName::MESSAGE_RECEIVED: return "message_received";
    case PluginHookName::MESSAGE_SENDING: return "message_sending";
    case PluginHookName::MESSAGE_SENT: return "message_sent";
    case PluginHookName::BEFORE_TOOL_CALL: return "before_tool_call";
    case PluginHookName::AFTER_TOOL_CALL: return "after_tool_call";
    case PluginHookName::TOOL_RESULT_PERSIST: return "tool_result_persist";
    case PluginHookName::BEFORE_MESSAGE_WRITE: return "before_message_write";
    case PluginHookName::SESSION_START: return "session_start";
    case PluginHookName::SESSION_END: return "session_end";
    case PluginHookName::SUBAGENT_SPAWNING: return "subagent_spawning";
    case PluginHookName::SUBAGENT_DELIVERY_TARGET: return "subagent_delivery_target";
    case PluginHookName::SUBAGENT_SPAWNED: return "subagent_spawned";
    case PluginHookName::SUBAGENT_ENDED: return "subagent_ended";
    case PluginHookName::GATEWAY_START: return "gateway_start";
    case PluginHookName::GATEWAY_STOP: return "gateway_stop";
  }
  return "unknown";
}

std::string http_route_auth_to_string(HttpRouteAuth auth) {
  switch (auth) {
    case HttpRouteAuth::GATEWAY: return "gateway";
    case HttpRouteAuth::PLUGIN: return "plugin";
  }
  return "unknown";
}

std::string http_route_match_to_string(HttpRouteMatch match) {
  switch (match) {
    case HttpRouteMatch::EXACT: return "exact";
    case HttpRouteMatch::PREFIX: return "prefix";
  }
  return "unknown";
}

// ============================================================================
// Utility Functions - String to Enum
// ============================================================================

std::optional<PluginKind> string_to_plugin_kind(const std::string& str) {
  if (str == "memory") return PluginKind::MEMORY;
  if (str == "context-engine") return PluginKind::CONTEXT_ENGINE;
  if (str == "general") return PluginKind::GENERAL;
  return std::nullopt;
}

std::optional<PluginOrigin> string_to_plugin_origin(const std::string& str) {
  if (str == "bundled") return PluginOrigin::BUNDLED;
  if (str == "global") return PluginOrigin::GLOBAL;
  if (str == "workspace") return PluginOrigin::WORKSPACE;
  if (str == "config") return PluginOrigin::CONFIG;
  return std::nullopt;
}

std::optional<PluginStatus> string_to_plugin_status(const std::string& str) {
  if (str == "loaded") return PluginStatus::LOADED;
  if (str == "disabled") return PluginStatus::DISABLED;
  if (str == "error") return PluginStatus::ERROR;
  return std::nullopt;
}

std::optional<PluginHookName> string_to_plugin_hook_name(const std::string& str) {
  if (str == "before_model_resolve") return PluginHookName::BEFORE_MODEL_RESOLVE;
  if (str == "before_prompt_build") return PluginHookName::BEFORE_PROMPT_BUILD;
  if (str == "before_agent_start") return PluginHookName::BEFORE_AGENT_START;
  if (str == "llm_input") return PluginHookName::LLM_INPUT;
  if (str == "llm_output") return PluginHookName::LLM_OUTPUT;
  if (str == "agent_end") return PluginHookName::AGENT_END;
  if (str == "before_compaction") return PluginHookName::BEFORE_COMPACTION;
  if (str == "after_compaction") return PluginHookName::AFTER_COMPACTION;
  if (str == "before_reset") return PluginHookName::BEFORE_RESET;
  if (str == "message_received") return PluginHookName::MESSAGE_RECEIVED;
  if (str == "message_sending") return PluginHookName::MESSAGE_SENDING;
  if (str == "message_sent") return PluginHookName::MESSAGE_SENT;
  if (str == "before_tool_call") return PluginHookName::BEFORE_TOOL_CALL;
  if (str == "after_tool_call") return PluginHookName::AFTER_TOOL_CALL;
  if (str == "tool_result_persist") return PluginHookName::TOOL_RESULT_PERSIST;
  if (str == "before_message_write") return PluginHookName::BEFORE_MESSAGE_WRITE;
  if (str == "session_start") return PluginHookName::SESSION_START;
  if (str == "session_end") return PluginHookName::SESSION_END;
  if (str == "subagent_spawning") return PluginHookName::SUBAGENT_SPAWNING;
  if (str == "subagent_delivery_target") return PluginHookName::SUBAGENT_DELIVERY_TARGET;
  if (str == "subagent_spawned") return PluginHookName::SUBAGENT_SPAWNED;
  if (str == "subagent_ended") return PluginHookName::SUBAGENT_ENDED;
  if (str == "gateway_start") return PluginHookName::GATEWAY_START;
  if (str == "gateway_stop") return PluginHookName::GATEWAY_STOP;
  return std::nullopt;
}

std::optional<HttpRouteAuth> string_to_http_route_auth(const std::string& str) {
  if (str == "gateway") return HttpRouteAuth::GATEWAY;
  if (str == "plugin") return HttpRouteAuth::PLUGIN;
  return std::nullopt;
}

std::optional<HttpRouteMatch> string_to_http_route_match(const std::string& str) {
  if (str == "exact") return HttpRouteMatch::EXACT;
  if (str == "prefix") return HttpRouteMatch::PREFIX;
  return std::nullopt;
}

}  // namespace plugins
}  // namespace hunterclaw
