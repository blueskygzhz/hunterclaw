#include "hunterclaw/config/config_system.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <regex>
#include <openssl/sha.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace config {

// ============================================================================
// 1. Config Types Implementation
// ============================================================================

json AgentConfig::to_json() const {
    json j;
    j["id"] = id;
    if (name) j["name"] = *name;
    if (model) j["model"] = *model;
    if (provider) j["provider"] = *provider;
    if (context_tokens) j["contextTokens"] = *context_tokens;
    if (max_tokens) j["maxTokens"] = *max_tokens;
    if (temperature) j["temperature"] = *temperature;
    if (system_prompt) j["systemPrompt"] = *system_prompt;
    if (workspace_dir) j["workspaceDir"] = *workspace_dir;
    if (enabled) j["enabled"] = *enabled;
    if (!extra.is_null()) j.update(extra);
    return j;
}

AgentConfig AgentConfig::from_json(const json& j) {
    AgentConfig config;
    config.id = j.value("id", "");
    if (j.contains("name")) config.name = j["name"];
    if (j.contains("model")) config.model = j["model"];
    if (j.contains("provider")) config.provider = j["provider"];
    if (j.contains("contextTokens")) config.context_tokens = j["contextTokens"];
    if (j.contains("maxTokens")) config.max_tokens = j["maxTokens"];
    if (j.contains("temperature")) config.temperature = j["temperature"];
    if (j.contains("systemPrompt")) config.system_prompt = j["systemPrompt"];
    if (j.contains("workspaceDir")) config.workspace_dir = j["workspaceDir"];
    if (j.contains("enabled")) config.enabled = j["enabled"];
    config.extra = j;
    return config;
}

json ModelConfig::to_json() const {
    json j;
    j["id"] = id;
    j["name"] = name;
    if (provider) j["provider"] = *provider;
    if (api) j["api"] = *api;
    if (max_tokens) j["maxTokens"] = *max_tokens;
    if (context_window) j["contextWindow"] = *context_window;
    if (cost) {
        j["cost"] = {
            {"input", cost->input},
            {"output", cost->output},
            {"cacheRead", cost->cache_read},
            {"cacheWrite", cost->cache_write}
        };
    }
    j["input"] = input;
    if (!extra.is_null()) j.update(extra);
    return j;
}

ModelConfig ModelConfig::from_json(const json& j) {
    ModelConfig config;
    config.id = j.value("id", "");
    config.name = j.value("name", "");
    if (j.contains("provider")) config.provider = j["provider"];
    if (j.contains("api")) config.api = j["api"];
    if (j.contains("maxTokens")) config.max_tokens = j["maxTokens"];
    if (j.contains("contextWindow")) config.context_window = j["contextWindow"];
    if (j.contains("cost")) {
        ModelConfig::Cost cost_obj;
        auto cost_json = j["cost"];
        cost_obj.input = cost_json.value("input", 0.0);
        cost_obj.output = cost_json.value("output", 0.0);
        cost_obj.cache_read = cost_json.value("cacheRead", 0.0);
        cost_obj.cache_write = cost_json.value("cacheWrite", 0.0);
        config.cost = cost_obj;
    }
    if (j.contains("input")) config.input = j["input"].get<std::vector<std::string>>();
    config.extra = j;
    return config;
}

json LoggingConfig::to_json() const {
    json j;
    j["level"] = level;
    if (file) j["file"] = *file;
    if (max_file_bytes) j["maxFileBytes"] = *max_file_bytes;
    if (console_level) j["consoleLevel"] = *console_level;
    if (console_style) j["consoleStyle"] = *console_style;
    if (!extra.is_null()) j.update(extra);
    return j;
}

LoggingConfig LoggingConfig::from_json(const json& j) {
    LoggingConfig config;
    config.level = j.value("level", "info");
    if (j.contains("file")) config.file = j["file"];
    if (j.contains("maxFileBytes")) config.max_file_bytes = j["maxFileBytes"];
    if (j.contains("consoleLevel")) config.console_level = j["consoleLevel"];
    if (j.contains("consoleStyle")) config.console_style = j["consoleStyle"];
    config.extra = j;
    return config;
}

json GatewayConfig::to_json() const {
    json j;
    j["enabled"] = enabled;
    if (mode) j["mode"] = *mode;
    if (bind) j["bind"] = *bind;
    if (port) j["port"] = *port;
    if (token) j["token"] = *token;
    if (password) j["password"] = *password;
    if (!extra.is_null()) j.update(extra);
    return j;
}

GatewayConfig GatewayConfig::from_json(const json& j) {
    GatewayConfig config;
    config.enabled = j.value("enabled", false);
    if (j.contains("mode")) config.mode = j["mode"];
    if (j.contains("bind")) config.bind = j["bind"];
    if (j.contains("port")) config.port = j["port"];
    if (j.contains("token")) config.token = j["token"];
    if (j.contains("password")) config.password = j["password"];
    config.extra = j;
    return config;
}

// Similar implementations for other config structs...

json OpenClawConfig::to_json() const {
    json j;
    if (version) j["version"] = *version;
    
    if (!agents.empty()) {
        j["agents"] = json::array();
        for (const auto& agent : agents) {
            j["agents"].push_back(agent.to_json());
        }
    }
    
    if (!models.empty()) {
        j["models"] = json::object();
        for (const auto& [id, model] : models) {
            j["models"][id] = model.to_json();
        }
    }
    
    if (logging) j["logging"] = logging->to_json();
    if (gateway) j["gateway"] = gateway->to_json();
    
    if (!channels.empty()) {
        j["channels"] = json::array();
        for (const auto& ch : channels) {
            j["channels"].push_back(ch.to_json());
        }
    }
    
    if (secrets) j["secrets"] = secrets->to_json();
    if (memory) j["memory"] = memory->to_json();
    if (tools) j["tools"] = tools->to_json();
    if (hooks) j["hooks"] = hooks->to_json();
    
    if (meta) {
        j["_meta"] = json::object();
        if (meta->timestamp) j["_meta"]["timestamp"] = *meta->timestamp;
        if (meta->source) j["_meta"]["source"] = *meta->source;
        if (meta->hash) j["_meta"]["hash"] = *meta->hash;
    }
    
    if (!extra.is_null()) j.update(extra);
    return j;
}

OpenClawConfig OpenClawConfig::from_json(const json& j) {
    OpenClawConfig config;
    
    if (j.contains("version")) config.version = j["version"];
    
    if (j.contains("agents")) {
        for (const auto& agent_json : j["agents"]) {
            config.agents.push_back(AgentConfig::from_json(agent_json));
        }
    }
    
    if (j.contains("models")) {
        for (auto it = j["models"].begin(); it != j["models"].end(); ++it) {
            config.models[it.key()] = ModelConfig::from_json(it.value());
        }
    }
    
    if (j.contains("logging")) config.logging = LoggingConfig::from_json(j["logging"]);
    if (j.contains("gateway")) config.gateway = GatewayConfig::from_json(j["gateway"]);
    
    if (j.contains("channels")) {
        for (const auto& ch_json : j["channels"]) {
            config.channels.push_back(ChannelConfig::from_json(ch_json));
        }
    }
    
    if (j.contains("secrets")) config.secrets = SecretsConfig::from_json(j["secrets"]);
    if (j.contains("memory")) config.memory = MemoryConfig::from_json(j["memory"]);
    if (j.contains("tools")) config.tools = ToolsConfig::from_json(j["tools"]);
    if (j.contains("hooks")) config.hooks = HooksConfig::from_json(j["hooks"]);
    
    if (j.contains("_meta")) {
        OpenClawConfig::Meta meta_obj;
        auto meta_json = j["_meta"];
        if (meta_json.contains("timestamp")) meta_obj.timestamp = meta_json["timestamp"];
        if (meta_json.contains("source")) meta_obj.source = meta_json["source"];
        if (meta_json.contains("hash")) meta_obj.hash = meta_json["hash"];
        config.meta = meta_obj;
    }
    
    config.extra = j;
    return config;
}

// ============================================================================
// 2. Config Validation Implementation
// ============================================================================

struct ConfigValidator::Impl {
    std::unordered_map<std::string, CustomValidator> validators;
};

ConfigValidator::ConfigValidator() : impl_(std::make_unique<Impl>()) {}

ConfigValidationResult ConfigValidator::validate(const json& config_json) const {
    ConfigValidationResult result;
    result.success = true;
    
    // Basic validation
    if (!config_json.is_object()) {
        ConfigValidationIssue issue;
        issue.severity = ConfigValidationSeverity::Error;
        issue.code = "invalid_type";
        issue.message = "Config must be an object";
        result.errors.push_back(issue);
        result.success = false;
        return result;
    }
    
    // Validate agents
    if (config_json.contains("agents")) {
        if (!config_json["agents"].is_array()) {
            ConfigValidationIssue issue;
            issue.severity = ConfigValidationSeverity::Error;
            issue.path = {"agents"};
            issue.code = "invalid_type";
            issue.message = "agents must be an array";
            result.errors.push_back(issue);
            result.success = false;
        } else {
            auto& agents = config_json["agents"];
            for (size_t i = 0; i < agents.size(); ++i) {
                if (!agents[i].contains("id")) {
                    ConfigValidationIssue issue;
                    issue.severity = ConfigValidationSeverity::Error;
                    issue.path = {"agents", std::to_string(i), "id"};
                    issue.code = "required";
                    issue.message = "Agent id is required";
                    result.errors.push_back(issue);
                    result.success = false;
                }
            }
        }
    }
    
    // Validate logging level
    if (config_json.contains("logging")) {
        auto& logging = config_json["logging"];
        if (logging.contains("level")) {
            std::string level = logging["level"];
            std::vector<std::string> allowed = {"silent", "fatal", "error", "warn", "info", "debug", "trace"};
            if (std::find(allowed.begin(), allowed.end(), level) == allowed.end()) {
                ConfigValidationIssue issue;
                issue.severity = ConfigValidationSeverity::Warning;
                issue.path = {"logging", "level"};
                issue.code = "invalid_value";
                issue.message = "Invalid log level: " + level;
                result.warnings.push_back(issue);
            }
        }
    }
    
    // Apply custom validators
    for (const auto& [path, validator] : impl_->validators) {
        auto issues = validator(config_json);
        for (auto& issue : issues) {
            if (issue.severity == ConfigValidationSeverity::Error) {
                result.errors.push_back(issue);
                result.success = false;
            } else {
                result.warnings.push_back(issue);
            }
        }
    }
    
    // If validation passed, parse config
    if (result.success) {
        try {
            result.config = OpenClawConfig::from_json(config_json);
        } catch (const std::exception& e) {
            ConfigValidationIssue issue;
            issue.severity = ConfigValidationSeverity::Error;
            issue.code = "parse_error";
            issue.message = "Failed to parse config: " + std::string(e.what());
            result.errors.push_back(issue);
            result.success = false;
        }
    }
    
    return result;
}

ConfigValidationResult ConfigValidator::validate_with_plugins(
    const json& config_json,
    const std::vector<std::string>& plugin_dirs) const {
    // TODO: Load plugin schemas and validate
    return validate(config_json);
}

ConfigValidationResult ConfigValidator::validate_section(
    const std::string& section,
    const json& section_json) const {
    json temp_config;
    temp_config[section] = section_json;
    return validate(temp_config);
}

void ConfigValidator::add_validator(const std::string& path, CustomValidator validator) {
    impl_->validators[path] = validator;
}

// ============================================================================
// 3. Config I/O Implementation
// ============================================================================

struct ConfigIO::Impl {
    std::unordered_map<std::string, ConfigFileSnapshot> snapshot_cache;
    std::optional<OpenClawConfig> runtime_snapshot;
};

ConfigIO::ConfigIO() : impl_(std::make_unique<Impl>()) {}
ConfigIO::~ConfigIO() = default;

std::optional<OpenClawConfig> ConfigIO::load_config(const std::string& path,
                                                     const ConfigLoadOptions& options) {
    // Read file
    auto snapshot = read_snapshot(path);
    if (!snapshot.exists || snapshot.error) {
        return std::nullopt;
    }
    
    json config_json = snapshot.content;
    
    // Resolve includes
    if (options.resolve_includes) {
        try {
            config_json = ConfigIncludes::resolve_includes(config_json, fs::path(path).parent_path(), 10);
        } catch (const ConfigIncludeError& e) {
            return std::nullopt;
        }
    }
    
    // Substitute environment variables
    if (options.substitute_env_vars) {
        try {
            config_json = EnvSubstitution::resolve_env_vars(config_json, false);
        } catch (const MissingEnvVarError& e) {
            return std::nullopt;
        }
    }
    
    // Migrate legacy config
    if (options.migrate_legacy && ConfigMigration::is_legacy_format(config_json)) {
        try {
            auto migrated = ConfigMigration::migrate_legacy(config_json);
            config_json = migrated.to_json();
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    
    // Normalize paths
    if (options.normalize_paths) {
        ConfigPaths::normalize_paths(config_json, fs::path(path).parent_path());
    }
    
    // Validate
    if (options.validate) {
        ConfigValidator validator;
        auto result = validator.validate(config_json);
        if (!result.success) {
            return std::nullopt;
        }
    }
    
    // Parse config
    OpenClawConfig config = OpenClawConfig::from_json(config_json);
    
    // Apply defaults
    if (options.apply_defaults) {
        ConfigDefaults::apply_all_defaults(config);
    }
    
    return config;
}

ConfigFileSnapshot ConfigIO::read_snapshot(const std::string& path) {
    ConfigFileSnapshot snapshot;
    snapshot.path = path;
    snapshot.exists = fs::exists(path);
    snapshot.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    
    if (!snapshot.exists) {
        snapshot.error = "File not found";
        return snapshot;
    }
    
    // Read file
    std::ifstream file(path);
    if (!file.is_open()) {
        snapshot.error = "Failed to open file";
        return snapshot;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content_str = buffer.str();
    
    snapshot.file_size = content_str.size();
    snapshot.format = detect_config_format(path);
    
    // Parse JSON
    try {
        snapshot.content = json::parse(content_str);
        snapshot.hash = compute_config_hash(snapshot.content);
    } catch (const json::parse_error& e) {
        snapshot.error = "Parse error: " + std::string(e.what());
    }
    
    return snapshot;
}

bool ConfigIO::write_config(const std::string& path,
                            const OpenClawConfig& config,
                            const ConfigWriteOptions& options) {
    json config_json = config.to_json();
    return write_json(path, config_json, options);
}

bool ConfigIO::write_json(const std::string& path,
                          const json& content,
                          const ConfigWriteOptions& options) {
    // Create backup
    if (options.backup && fs::exists(path)) {
        std::string backup_path = path + ".backup";
        try {
            fs::copy_file(path, backup_path, fs::copy_options::overwrite_existing);
        } catch (const fs::filesystem_error&) {
            return false;
        }
    }
    
    // Serialize JSON
    std::string content_str;
    if (options.format == ConfigFormat::JSON || options.format == ConfigFormat::JSON5) {
        content_str = content.dump(options.indent);
    } else {
        content_str = content.dump(options.indent);
    }
    
    // Atomic write
    if (options.atomic) {
        std::string temp_path = path + ".tmp";
        std::ofstream temp_file(temp_path);
        if (!temp_file.is_open()) {
            return false;
        }
        temp_file << content_str;
        temp_file.close();
        
        try {
            fs::rename(temp_path, path);
        } catch (const fs::filesystem_error&) {
            return false;
        }
    } else {
        std::ofstream file(path);
        if (!file.is_open()) {
            return false;
        }
        file << content_str;
    }
    
    return true;
}

std::optional<json> ConfigIO::parse_config_string(const std::string& content,
                                                   ConfigFormat format) {
    try {
        return json::parse(content);
    } catch (const json::parse_error&) {
        return std::nullopt;
    }
}

std::string ConfigIO::stringify_config(const json& config,
                                        ConfigFormat format,
                                        int indent) {
    return config.dump(indent);
}

void ConfigIO::clear_cache() {
    impl_->snapshot_cache.clear();
}

std::optional<ConfigFileSnapshot> ConfigIO::get_cached_snapshot(const std::string& path) {
    auto it = impl_->snapshot_cache.find(path);
    if (it != impl_->snapshot_cache.end()) {
        return it->second;
    }
    return std::nullopt;
}

void ConfigIO::set_cached_snapshot(const std::string& path, const ConfigFileSnapshot& snapshot) {
    impl_->snapshot_cache[path] = snapshot;
}

std::optional<OpenClawConfig> ConfigIO::get_runtime_snapshot() {
    return impl_->runtime_snapshot;
}

void ConfigIO::set_runtime_snapshot(const OpenClawConfig& config) {
    impl_->runtime_snapshot = config;
}

void ConfigIO::clear_runtime_snapshot() {
    impl_->runtime_snapshot = std::nullopt;
}

// ============================================================================
// 4. Config Defaults Implementation
// ============================================================================

void ConfigDefaults::apply_agent_defaults(OpenClawConfig& config) {
    for (auto& agent : config.agents) {
        if (!agent.context_tokens) {
            agent.context_tokens = get_default_context_tokens();
        }
        if (!agent.max_tokens) {
            agent.max_tokens = get_default_max_tokens();
        }
        if (!agent.temperature) {
            agent.temperature = get_default_temperature();
        }
        if (!agent.model) {
            agent.model = get_default_model();
        }
    }
}

void ConfigDefaults::apply_model_defaults(OpenClawConfig& config) {
    for (auto& [id, model] : config.models) {
        if (!model.cost) {
            ModelConfig::Cost cost;
            cost.input = 0.0;
            cost.output = 0.0;
            cost.cache_read = 0.0;
            cost.cache_write = 0.0;
            model.cost = cost;
        }
        if (model.input.empty()) {
            model.input = {"text"};
        }
    }
}

void ConfigDefaults::apply_logging_defaults(OpenClawConfig& config) {
    if (!config.logging) {
        config.logging = LoggingConfig();
    }
    if (config.logging->level.empty()) {
        config.logging->level = get_default_log_level();
    }
}

void ConfigDefaults::apply_session_defaults(OpenClawConfig& config) {
    // Apply session-related defaults
}

void ConfigDefaults::apply_message_defaults(OpenClawConfig& config) {
    // Apply message-related defaults
}

void ConfigDefaults::apply_compaction_defaults(OpenClawConfig& config) {
    // Apply compaction-related defaults
}

void ConfigDefaults::apply_all_defaults(OpenClawConfig& config) {
    apply_agent_defaults(config);
    apply_model_defaults(config);
    apply_logging_defaults(config);
    apply_session_defaults(config);
    apply_message_defaults(config);
    apply_compaction_defaults(config);
}

int ConfigDefaults::get_default_context_tokens() {
    return 200000;
}

int ConfigDefaults::get_default_max_tokens() {
    return 8192;
}

double ConfigDefaults::get_default_temperature() {
    return 0.7;
}

std::string ConfigDefaults::get_default_log_level() {
    return "info";
}

std::string ConfigDefaults::get_default_model() {
    return "anthropic/claude-sonnet-4-6";
}

std::unordered_map<std::string, std::string> ConfigDefaults::get_model_aliases() {
    return {
        {"opus", "anthropic/claude-opus-4-6"},
        {"sonnet", "anthropic/claude-sonnet-4-6"},
        {"gpt", "openai/gpt-5.4"},
        {"gpt-mini", "openai/gpt-5-mini"},
        {"gemini", "google/gemini-3.1-pro-preview"},
        {"gemini-flash", "google/gemini-3-flash-preview"}
    };
}

std::optional<std::string> ConfigDefaults::resolve_model_alias(const std::string& alias) {
    auto aliases = get_model_aliases();
    auto it = aliases.find(alias);
    if (it != aliases.end()) {
        return it->second;
    }
    return std::nullopt;
}

// ============================================================================
// 5. Environment Substitution Implementation
// ============================================================================

json EnvSubstitution::resolve_env_vars(const json& config,
                                        bool allow_missing,
                                        std::vector<EnvSubstitutionWarning>* warnings) {
    json result = config;
    
    // Recursively resolve env vars
    std::function<void(json&, const std::string&)> resolve_recursive;
    resolve_recursive = [&](json& node, const std::string& path) {
        if (node.is_string()) {
            std::string str = node.get<std::string>();
            if (is_env_ref(str)) {
                auto var_name = parse_env_ref(str);
                if (var_name) {
                    const char* env_value = std::getenv(var_name->c_str());
                    if (env_value) {
                        node = std::string(env_value);
                    } else if (!allow_missing) {
                        throw MissingEnvVarError(*var_name);
                    } else if (warnings) {
                        EnvSubstitutionWarning warning;
                        warning.path = path;
                        warning.var_name = *var_name;
                        warning.message = "Missing environment variable: " + *var_name;
                        warnings->push_back(warning);
                    }
                }
            }
        } else if (node.is_object()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                resolve_recursive(it.value(), path + "." + it.key());
            }
        } else if (node.is_array()) {
            for (size_t i = 0; i < node.size(); ++i) {
                resolve_recursive(node[i], path + "[" + std::to_string(i) + "]");
            }
        }
    };
    
    resolve_recursive(result, "");
    return result;
}

json EnvSubstitution::restore_env_refs(const json& config) {
    // TODO: Restore env var references (for writing config)
    return config;
}

bool EnvSubstitution::contains_env_refs(const json& config) {
    if (config.is_string()) {
        return is_env_ref(config.get<std::string>());
    } else if (config.is_object()) {
        for (auto it = config.begin(); it != config.end(); ++it) {
            if (contains_env_refs(it.value())) {
                return true;
            }
        }
    } else if (config.is_array()) {
        for (const auto& item : config) {
            if (contains_env_refs(item)) {
                return true;
            }
        }
    }
    return false;
}

std::string EnvSubstitution::substitute_string(const std::string& str, bool allow_missing) {
    if (is_env_ref(str)) {
        auto var_name = parse_env_ref(str);
        if (var_name) {
            const char* env_value = std::getenv(var_name->c_str());
            if (env_value) {
                return std::string(env_value);
            } else if (!allow_missing) {
                throw MissingEnvVarError(*var_name);
            }
        }
    }
    return str;
}

bool EnvSubstitution::is_env_ref(const std::string& str) {
    return str.size() > 4 && str.substr(0, 2) == "${" && str.back() == '}';
}

std::optional<std::string> EnvSubstitution::parse_env_ref(const std::string& str) {
    if (is_env_ref(str)) {
        return str.substr(2, str.size() - 3);
    }
    return std::nullopt;
}

// ============================================================================
// 6. Config Includes Implementation
// ============================================================================

json ConfigIncludes::resolve_includes(const json& config,
                                       const std::string& base_path,
                                       int max_depth) {
    if (max_depth <= 0) {
        throw ConfigIncludeError(ConfigIncludeError::Code::CircularInclude,
                                 "Maximum include depth exceeded");
    }
    
    json result = config;
    
    // Check for includes field
    if (config.contains("includes") && config["includes"].is_array()) {
        for (const auto& include_path : config["includes"]) {
            if (!include_path.is_string()) {
                continue;
            }
            
            std::string path = include_path.get<std::string>();
            json included = read_include_file(path, base_path);
            json resolved = resolve_includes(included, base_path, max_depth - 1);
            
            // Merge included config
            result = ConfigMerge::merge_configs(result, resolved);
        }
        
        // Remove includes field
        result.erase("includes");
    }
    
    return result;
}

json ConfigIncludes::read_include_file(const std::string& path,
                                        const std::string& base_path) {
    fs::path full_path = fs::path(base_path) / path;
    
    if (!fs::exists(full_path)) {
        throw ConfigIncludeError(ConfigIncludeError::Code::FileNotFound,
                                 "Include file not found: " + full_path.string());
    }
    
    std::ifstream file(full_path);
    if (!file.is_open()) {
        throw ConfigIncludeError(ConfigIncludeError::Code::ReadError,
                                 "Failed to read include file: " + full_path.string());
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    
    try {
        return json::parse(buffer.str());
    } catch (const json::parse_error& e) {
        throw ConfigIncludeError(ConfigIncludeError::Code::InvalidInclude,
                                 "Parse error in include file: " + std::string(e.what()));
    }
}

bool ConfigIncludes::has_includes(const json& config) {
    return config.contains("includes") && config["includes"].is_array() &&
           !config["includes"].empty();
}

std::vector<std::string> ConfigIncludes::extract_include_paths(const json& config) {
    std::vector<std::string> paths;
    if (has_includes(config)) {
        for (const auto& path : config["includes"]) {
            if (path.is_string()) {
                paths.push_back(path.get<std::string>());
            }
        }
    }
    return paths;
}

// ============================================================================
// 7. Path Resolution Implementation
// ============================================================================

std::string ConfigPaths::resolve_config_path(const std::optional<std::string>& path_hint) {
    if (path_hint) {
        return *path_hint;
    }
    
    // Try default candidates
    auto candidates = get_default_config_candidates();
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            return candidate;
        }
    }
    
    // Return first candidate (even if doesn't exist)
    return candidates.empty() ? "openclaw.json" : candidates[0];
}

std::vector<std::string> ConfigPaths::get_default_config_candidates() {
    std::vector<std::string> candidates;
    
    // Current directory
    candidates.push_back("openclaw.json");
    candidates.push_back("openclaw.json5");
    candidates.push_back(".openclaw/config.json");
    
    // Home directory
    const char* home = std::getenv("HOME");
    if (home) {
        candidates.push_back(std::string(home) + "/.openclaw/config.json");
        candidates.push_back(std::string(home) + "/.config/openclaw/config.json");
    }
    
    return candidates;
}

std::string ConfigPaths::resolve_state_dir(const std::optional<std::string>& hint) {
    if (hint) {
        return *hint;
    }
    
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.openclaw/state";
    }
    
    return ".openclaw/state";
}

std::string ConfigPaths::resolve_agent_workspace(const std::string& agent_id,
                                                  const std::optional<std::string>& workspace_hint) {
    if (workspace_hint) {
        return *workspace_hint;
    }
    
    return resolve_state_dir() + "/workspaces/" + agent_id;
}

void ConfigPaths::normalize_paths(json& config, const std::string& base_dir) {
    std::function<void(json&)> normalize_recursive;
    normalize_recursive = [&](json& node) {
        if (node.is_string()) {
            std::string path = node.get<std::string>();
            if (!is_absolute(path)) {
                node = make_absolute(path, base_dir);
            }
        } else if (node.is_object()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                if (it.key().find("Path") != std::string::npos ||
                    it.key().find("Dir") != std::string::npos ||
                    it.key().find("File") != std::string::npos) {
                    normalize_recursive(it.value());
                }
            }
        } else if (node.is_array()) {
            for (auto& item : node) {
                normalize_recursive(item);
            }
        }
    };
    
    normalize_recursive(config);
}

std::string ConfigPaths::expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }
    
    const char* home = std::getenv("HOME");
    if (!home) {
        return path;
    }
    
    return std::string(home) + path.substr(1);
}

std::string ConfigPaths::make_absolute(const std::string& path, const std::string& base_dir) {
    fs::path p(path);
    if (p.is_absolute()) {
        return path;
    }
    return (fs::path(base_dir) / p).string();
}

bool ConfigPaths::is_absolute(const std::string& path) {
    return fs::path(path).is_absolute();
}

// ============================================================================
// 8. Config Merge Implementation
// ============================================================================

json ConfigMerge::apply_merge_patch(const json& target, const json& patch) {
    if (!patch.is_object()) {
        return patch;
    }
    
    json result = target.is_object() ? target : json::object();
    
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.value().is_null()) {
            result.erase(it.key());
        } else {
            if (result.contains(it.key()) && result[it.key()].is_object() && it.value().is_object()) {
                result[it.key()] = apply_merge_patch(result[it.key()], it.value());
            } else {
                result[it.key()] = it.value();
            }
        }
    }
    
    return result;
}

json ConfigMerge::merge_configs(const json& base, const json& override,
                                 MergeStrategy strategy) {
    if (strategy == MergeStrategy::Replace) {
        return override;
    }
    
    if (strategy == MergeStrategy::Override) {
        return override.is_null() ? base : override;
    }
    
    if (!base.is_object() || !override.is_object()) {
        return override;
    }
    
    json result = base;
    
    for (auto it = override.begin(); it != override.end(); ++it) {
        if (!result.contains(it.key())) {
            result[it.key()] = it.value();
        } else if (strategy == MergeStrategy::Append && result[it.key()].is_array() && it.value().is_array()) {
            for (const auto& item : it.value()) {
                result[it.key()].push_back(item);
            }
        } else if (result[it.key()].is_object() && it.value().is_object()) {
            result[it.key()] = merge_configs(result[it.key()], it.value(), strategy);
        } else {
            result[it.key()] = it.value();
        }
    }
    
    return result;
}

json ConfigMerge::merge_config_array(const std::vector<json>& configs,
                                      MergeStrategy strategy) {
    json result = json::object();
    for (const auto& config : configs) {
        result = merge_configs(result, config, strategy);
    }
    return result;
}

bool ConfigMerge::has_prototype_pollution(const json& config) {
    std::vector<std::string> blocked_keys = {"__proto__", "constructor", "prototype"};
    
    std::function<bool(const json&)> check_recursive;
    check_recursive = [&](const json& node) -> bool {
        if (node.is_object()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                if (std::find(blocked_keys.begin(), blocked_keys.end(), it.key()) != blocked_keys.end()) {
                    return true;
                }
                if (check_recursive(it.value())) {
                    return true;
                }
            }
        } else if (node.is_array()) {
            for (const auto& item : node) {
                if (check_recursive(item)) {
                    return true;
                }
            }
        }
        return false;
    };
    
    return check_recursive(config);
}

json ConfigMerge::sanitize_keys(const json& config) {
    std::vector<std::string> blocked_keys = {"__proto__", "constructor", "prototype"};
    
    std::function<json(const json&)> sanitize_recursive;
    sanitize_recursive = [&](const json& node) -> json {
        if (node.is_object()) {
            json result = json::object();
            for (auto it = node.begin(); it != node.end(); ++it) {
                if (std::find(blocked_keys.begin(), blocked_keys.end(), it.key()) == blocked_keys.end()) {
                    result[it.key()] = sanitize_recursive(it.value());
                }
            }
            return result;
        } else if (node.is_array()) {
            json result = json::array();
            for (const auto& item : node) {
                result.push_back(sanitize_recursive(item));
            }
            return result;
        }
        return node;
    };
    
    return sanitize_recursive(config);
}

// ============================================================================
// 9. Legacy Migration Implementation (stub)
// ============================================================================

OpenClawConfig ConfigMigration::migrate_legacy(const json& legacy_config) {
    // TODO: Implement legacy migration logic
    return OpenClawConfig::from_json(legacy_config);
}

std::vector<LegacyConfigIssue> ConfigMigration::find_legacy_issues(const json& config) {
    // TODO: Find legacy config issues
    return {};
}

bool ConfigMigration::is_legacy_format(const json& config) {
    // TODO: Detect legacy format
    return false;
}

std::vector<std::string> ConfigMigration::get_migration_warnings(const json& legacy_config) {
    // TODO: Get migration warnings
    return {};
}

// ============================================================================
// 10. Config Backup Implementation (stub)
// ============================================================================

ConfigBackup::ConfigBackup(const BackupRotationConfig& config) : config_(config) {}

std::string ConfigBackup::create_backup(const std::string& config_path) {
    // TODO: Implement backup creation
    return "";
}

std::vector<std::string> ConfigBackup::list_backups(const std::string& config_path) {
    // TODO: List backups
    return {};
}

bool ConfigBackup::restore_backup(const std::string& backup_path, const std::string& target_path) {
    // TODO: Restore backup
    return false;
}

void ConfigBackup::rotate_backups(const std::string& config_path) {
    // TODO: Rotate backups
}

bool ConfigBackup::delete_backup(const std::string& backup_path) {
    // TODO: Delete backup
    return false;
}

// ============================================================================
// 11. Config Schema Implementation (stub)
// ============================================================================

ConfigSchema ConfigSchemaBuilder::build_schema() {
    // TODO: Build complete JSON Schema
    return json::object();
}

bool ConfigSchemaBuilder::validate_against_schema(const json& config,
                                                   const ConfigSchema& schema,
                                                   std::vector<std::string>* errors) {
    // TODO: Validate against JSON Schema
    return true;
}

ConfigSchema ConfigSchemaBuilder::get_section_schema(const std::string& section) {
    // TODO: Get section schema
    return json::object();
}

std::optional<ConfigSchema> ConfigSchemaBuilder::lookup_schema(
    const ConfigSchema& root,
    const std::vector<std::string>& path) {
    // TODO: Lookup schema by path
    return std::nullopt;
}

// ============================================================================
// 12. Utility Functions
// ============================================================================

std::string compute_config_hash(const json& config) {
    std::string config_str = config.dump();
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(config_str.data()),
           config_str.size(), hash);
    
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return ss.str();
}

bool configs_equal(const json& a, const json& b) {
    return a == b;
}

json redact_config_secrets(const json& config) {
    // 🎯 严格对齐 OpenClaw: redactConfigObject()
    // 使用路径名启发式 + REDACTED_SENTINEL 双向脱敏
    // 委托到 redact_snapshot.cc 的统一实现
    json result = config;
    
    // 递归遍历，基于路径名模式检测敏感字段
    std::vector<std::string> secret_path_keywords = {
        "token", "password", "apikey", "api_key", "secret", "privatekey",
        "private_key", "passwd", "accesstoken", "access_token",
        "refreshtoken", "refresh_token", "credentials",
    };
    
    std::function<void(json&, const std::string&)> redact_recursive;
    redact_recursive = [&](json& node, const std::string& prefix) {
        if (node.is_object()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
                std::string lower_path = path;
                std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
                
                bool is_sensitive = false;
                for (const auto& kw : secret_path_keywords) {
                    if (lower_path.find(kw) != std::string::npos) {
                        is_sensitive = true;
                        break;
                    }
                }
                
                if (is_sensitive && it.value().is_string()) {
                    // 🎯 对齐 OpenClaw: 使用 __OPENCLAW_REDACTED__ 哨兵值
                    std::string val = it.value().get<std::string>();
                    // 跳过 ${...} 环境变量占位符
                    if (!(val.size() >= 4 && val[0] == '$' && val[1] == '{' && val.back() == '}')) {
                        it.value() = "__OPENCLAW_REDACTED__";
                    }
                } else {
                    redact_recursive(it.value(), path);
                }
            }
        } else if (node.is_array()) {
            for (auto& item : node) {
                redact_recursive(item, prefix + "[]");
            }
        }
    };
    
    redact_recursive(result, "");
    return result;
}

std::string get_config_extension(ConfigFormat format) {
    switch (format) {
        case ConfigFormat::JSON: return ".json";
        case ConfigFormat::JSON5: return ".json5";
        case ConfigFormat::YAML: return ".yaml";
        default: return ".json";
    }
}

ConfigFormat detect_config_format(const std::string& path) {
    fs::path p(path);
    std::string ext = p.extension().string();
    
    if (ext == ".json5") return ConfigFormat::JSON5;
    if (ext == ".yaml" || ext == ".yml") return ConfigFormat::YAML;
    return ConfigFormat::JSON;
}

}  // namespace config
}  // namespace hunterclaw
