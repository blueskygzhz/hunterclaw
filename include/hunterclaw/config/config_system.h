#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <functional>
#include <memory>
#include <variant>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace config {

// ============================================================================
// Config System - 严格对齐 OpenClaw src/config/
// ============================================================================
// 对齐文件:
// - config/types.ts          (完整类型定义)
// - config/io.ts             (配置读写和缓存)
// - config/validation.ts     (配置验证)
// - config/schema.ts         (JSON Schema)
// - config/defaults.ts       (默认值应用)
// - config/env-substitution.ts (环境变量替换)
// - config/includes.ts       (配置包含/合并)
// - config/paths.ts          (路径解析)
// - config/legacy-migrate.ts (旧配置迁移)
// - config/merge-patch.ts    (配置合并)
// - config/normalize-paths.ts (路径规范化)
// ============================================================================

using json = nlohmann::json;

// ============================================================================
// 1. Config Types (对齐 config/types.ts)
// ============================================================================

// Agent Configuration
struct AgentConfig {
    std::string id;
    std::optional<std::string> name;
    std::optional<std::string> model;
    std::optional<std::string> provider;
    std::optional<int> context_tokens;
    std::optional<int> max_tokens;
    std::optional<double> temperature;
    std::optional<std::string> system_prompt;
    std::optional<std::string> workspace_dir;
    std::optional<bool> enabled;
    json extra;
    
    json to_json() const;
    static AgentConfig from_json(const json& j);
};

// Model Configuration
struct ModelConfig {
    std::string id;
    std::string name;
    std::optional<std::string> provider;
    std::optional<std::string> api;
    std::optional<int> max_tokens;
    std::optional<int> context_window;
    
    struct Cost {
        double input = 0.0;
        double output = 0.0;
        double cache_read = 0.0;
        double cache_write = 0.0;
    };
    std::optional<Cost> cost;
    
    std::vector<std::string> input;  // ["text", "image", "audio", "video"]
    json extra;
    
    json to_json() const;
    static ModelConfig from_json(const json& j);
};

// Logging Configuration
struct LoggingConfig {
    std::string level = "info";  // silent, fatal, error, warn, info, debug, trace
    std::optional<std::string> file;
    std::optional<int64_t> max_file_bytes;
    std::optional<std::string> console_level;
    std::optional<std::string> console_style;
    json extra;
    
    json to_json() const;
    static LoggingConfig from_json(const json& j);
};

// Gateway Configuration
struct GatewayConfig {
    bool enabled = false;
    std::optional<std::string> mode;  // "http", "websocket", etc.
    std::optional<std::string> bind;
    std::optional<int> port;
    std::optional<std::string> token;
    std::optional<std::string> password;
    json extra;
    
    json to_json() const;
    static GatewayConfig from_json(const json& j);
};

// Channel Configuration
struct ChannelConfig {
    std::string channel_id;
    bool enabled = false;
    json credentials;
    json settings;
    json extra;
    
    json to_json() const;
    static ChannelConfig from_json(const json& j);
};

// Secrets Configuration
struct SecretsConfig {
    std::optional<std::string> provider;  // "file", "env", "keychain", etc.
    json settings;
    json extra;
    
    json to_json() const;
    static SecretsConfig from_json(const json& j);
};

// Memory Configuration
struct MemoryConfig {
    bool enabled = true;
    std::optional<std::string> storage;  // "local", "remote", etc.
    std::optional<std::string> compaction;
    json settings;
    json extra;
    
    json to_json() const;
    static MemoryConfig from_json(const json& j);
};

// Tools Configuration
struct ToolsConfig {
    std::vector<std::string> enabled;
    std::vector<std::string> disabled;
    json settings;
    json extra;
    
    json to_json() const;
    static ToolsConfig from_json(const json& j);
};

// Hooks Configuration
struct HooksConfig {
    std::optional<std::string> module_path;
    std::vector<std::string> hooks;
    json extra;
    
    json to_json() const;
    static HooksConfig from_json(const json& j);
};

// Complete OpenClaw Configuration
struct OpenClawConfig {
    std::optional<std::string> version;
    std::vector<AgentConfig> agents;
    std::unordered_map<std::string, ModelConfig> models;
    std::optional<LoggingConfig> logging;
    std::optional<GatewayConfig> gateway;
    std::vector<ChannelConfig> channels;
    std::optional<SecretsConfig> secrets;
    std::optional<MemoryConfig> memory;
    std::optional<ToolsConfig> tools;
    std::optional<HooksConfig> hooks;
    
    // Metadata
    struct Meta {
        std::optional<std::string> timestamp;
        std::optional<std::string> source;
        std::optional<std::string> hash;
    };
    std::optional<Meta> meta;
    
    json extra;  // For future extensions
    
    json to_json() const;
    static OpenClawConfig from_json(const json& j);
};

// ============================================================================
// 2. Config Validation (对齐 config/validation.ts)
// ============================================================================

enum class ConfigValidationSeverity {
    Error,
    Warning,
    Info
};

struct ConfigValidationIssue {
    ConfigValidationSeverity severity;
    std::vector<std::string> path;
    std::string code;
    std::string message;
    std::optional<json> meta;
    
    json to_json() const;
    static ConfigValidationIssue from_json(const json& j);
};

struct ConfigValidationResult {
    bool success;
    std::vector<ConfigValidationIssue> errors;
    std::vector<ConfigValidationIssue> warnings;
    std::optional<OpenClawConfig> config;
    
    bool has_errors() const { return !errors.empty(); }
    bool has_warnings() const { return !warnings.empty(); }
};

// Config Validator
class ConfigValidator {
public:
    ConfigValidator();
    
    // Validate config object
    ConfigValidationResult validate(const json& config_json) const;
    
    // Validate config with plugins
    ConfigValidationResult validate_with_plugins(const json& config_json,
                                                  const std::vector<std::string>& plugin_dirs) const;
    
    // Validate specific config section
    ConfigValidationResult validate_section(const std::string& section,
                                             const json& section_json) const;
    
    // Add custom validator
    using CustomValidator = std::function<std::vector<ConfigValidationIssue>(const json&)>;
    void add_validator(const std::string& path, CustomValidator validator);
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// 3. Config I/O (对齐 config/io.ts)
// ============================================================================

// Config file format
enum class ConfigFormat {
    JSON,
    JSON5,
    YAML,
    Auto  // Auto-detect from extension
};

// Config snapshot (read-only view)
struct ConfigFileSnapshot {
    std::string path;
    std::string hash;
    int64_t timestamp;
    json content;
    ConfigFormat format;
    
    // Metadata
    bool exists;
    int64_t file_size;
    std::optional<std::string> error;
};

// Config load options
struct ConfigLoadOptions {
    bool apply_defaults = true;
    bool validate = true;
    bool resolve_includes = true;
    bool substitute_env_vars = true;
    bool normalize_paths = true;
    bool migrate_legacy = false;
    std::optional<std::string> state_dir;
    std::optional<std::vector<std::string>> plugin_dirs;
};

// Config write options
struct ConfigWriteOptions {
    ConfigFormat format = ConfigFormat::JSON;
    bool backup = true;
    bool atomic = true;
    int indent = 2;
    bool sort_keys = false;
};

// Config I/O Manager
class ConfigIO {
public:
    ConfigIO();
    ~ConfigIO();
    
    // Read config from file
    std::optional<OpenClawConfig> load_config(const std::string& path,
                                              const ConfigLoadOptions& options = {});
    
    // Read config snapshot (no processing)
    ConfigFileSnapshot read_snapshot(const std::string& path);
    
    // Write config to file
    bool write_config(const std::string& path,
                      const OpenClawConfig& config,
                      const ConfigWriteOptions& options = {});
    
    // Write raw JSON
    bool write_json(const std::string& path,
                    const json& content,
                    const ConfigWriteOptions& options = {});
    
    // Parse config from string
    std::optional<json> parse_config_string(const std::string& content,
                                            ConfigFormat format = ConfigFormat::Auto);
    
    // Stringify config
    std::string stringify_config(const json& config,
                                  ConfigFormat format = ConfigFormat::JSON,
                                  int indent = 2);
    
    // Cache management
    void clear_cache();
    std::optional<ConfigFileSnapshot> get_cached_snapshot(const std::string& path);
    void set_cached_snapshot(const std::string& path, const ConfigFileSnapshot& snapshot);
    
    // Runtime config snapshot (in-memory view)
    std::optional<OpenClawConfig> get_runtime_snapshot();
    void set_runtime_snapshot(const OpenClawConfig& config);
    void clear_runtime_snapshot();
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// 4. Config Defaults (对齐 config/defaults.ts)
// ============================================================================

class ConfigDefaults {
public:
    // Apply defaults to config
    static void apply_agent_defaults(OpenClawConfig& config);
    static void apply_model_defaults(OpenClawConfig& config);
    static void apply_logging_defaults(OpenClawConfig& config);
    static void apply_session_defaults(OpenClawConfig& config);
    static void apply_message_defaults(OpenClawConfig& config);
    static void apply_compaction_defaults(OpenClawConfig& config);
    static void apply_all_defaults(OpenClawConfig& config);
    
    // Get default values
    static int get_default_context_tokens();
    static int get_default_max_tokens();
    static double get_default_temperature();
    static std::string get_default_log_level();
    static std::string get_default_model();
    
    // Model aliases (对齐 DEFAULT_MODEL_ALIASES)
    static std::unordered_map<std::string, std::string> get_model_aliases();
    static std::optional<std::string> resolve_model_alias(const std::string& alias);
};

// ============================================================================
// 5. Environment Substitution (对齐 config/env-substitution.ts)
// ============================================================================

// Env var substitution error
class MissingEnvVarError : public std::runtime_error {
public:
    explicit MissingEnvVarError(const std::string& var_name)
        : std::runtime_error("Missing environment variable: " + var_name),
          var_name_(var_name) {}
    
    const std::string& var_name() const { return var_name_; }
    
private:
    std::string var_name_;
};

struct EnvSubstitutionWarning {
    std::string path;
    std::string var_name;
    std::string message;
};

// Environment variable substitution
class EnvSubstitution {
public:
    // Resolve env vars in config
    static json resolve_env_vars(const json& config,
                                  bool allow_missing = false,
                                  std::vector<EnvSubstitutionWarning>* warnings = nullptr);
    
    // Restore env var refs (for writing config)
    static json restore_env_refs(const json& config);
    
    // Check if config contains env var references
    static bool contains_env_refs(const json& config);
    
    // Substitute single string
    static std::string substitute_string(const std::string& str,
                                          bool allow_missing = false);
    
    // Check if string contains env var reference
    static bool is_env_ref(const std::string& str);
    
    // Parse env var reference (e.g., "${API_KEY}" -> "API_KEY")
    static std::optional<std::string> parse_env_ref(const std::string& str);
};

// ============================================================================
// 6. Config Includes (对齐 config/includes.ts)
// ============================================================================

// Include resolution error
class ConfigIncludeError : public std::runtime_error {
public:
    enum class Code {
        FileNotFound,
        CircularInclude,
        InvalidInclude,
        ReadError
    };
    
    ConfigIncludeError(Code code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    
    Code code() const { return code_; }
    
private:
    Code code_;
};

// Config includes resolver
class ConfigIncludes {
public:
    // Resolve all includes in config
    static json resolve_includes(const json& config,
                                  const std::string& base_path,
                                  int max_depth = 10);
    
    // Read single include file
    static json read_include_file(const std::string& path,
                                   const std::string& base_path);
    
    // Check if config has includes
    static bool has_includes(const json& config);
    
    // Extract include paths
    static std::vector<std::string> extract_include_paths(const json& config);
};

// ============================================================================
// 7. Path Resolution (对齐 config/paths.ts)
// ============================================================================

class ConfigPaths {
public:
    // Resolve config path
    static std::string resolve_config_path(const std::optional<std::string>& path_hint = std::nullopt);
    
    // Get default config candidates
    static std::vector<std::string> get_default_config_candidates();
    
    // Resolve state directory
    static std::string resolve_state_dir(const std::optional<std::string>& hint = std::nullopt);
    
    // Resolve agent workspace directory
    static std::string resolve_agent_workspace(const std::string& agent_id,
                                                const std::optional<std::string>& workspace_hint = std::nullopt);
    
    // Normalize paths in config
    static void normalize_paths(json& config, const std::string& base_dir);
    
    // Expand home directory (~)
    static std::string expand_home(const std::string& path);
    
    // Make path absolute
    static std::string make_absolute(const std::string& path, const std::string& base_dir);
    
    // Is path absolute?
    static bool is_absolute(const std::string& path);
};

// ============================================================================
// 8. Config Merge (对齐 config/merge-patch.ts)
// ============================================================================

// Merge strategy
enum class MergeStrategy {
    Replace,      // Replace target with source
    Merge,        // Deep merge objects
    Append,       // Append arrays
    Override      // Override if source is not null/undefined
};

class ConfigMerge {
public:
    // Apply merge patch (RFC 7386 JSON Merge Patch)
    static json apply_merge_patch(const json& target, const json& patch);
    
    // Deep merge configs
    static json merge_configs(const json& base, const json& override,
                              MergeStrategy strategy = MergeStrategy::Merge);
    
    // Merge config array (e.g., multiple config files)
    static json merge_config_array(const std::vector<json>& configs,
                                    MergeStrategy strategy = MergeStrategy::Merge);
    
    // Check for prototype pollution
    static bool has_prototype_pollution(const json& config);
    
    // Remove blocked keys (__proto__, constructor, prototype)
    static json sanitize_keys(const json& config);
};

// ============================================================================
// 9. Legacy Migration (对齐 config/legacy-migrate.ts)
// ============================================================================

struct LegacyConfigIssue {
    std::string code;
    std::string message;
    std::string path;
    std::optional<std::string> suggestion;
};

class ConfigMigration {
public:
    // Migrate legacy config to current format
    static OpenClawConfig migrate_legacy(const json& legacy_config);
    
    // Find legacy config issues
    static std::vector<LegacyConfigIssue> find_legacy_issues(const json& config);
    
    // Check if config is legacy format
    static bool is_legacy_format(const json& config);
    
    // Get migration warnings
    static std::vector<std::string> get_migration_warnings(const json& legacy_config);
};

// ============================================================================
// 10. Config Backup & Rotation (对齐 config/backup-rotation.ts)
// ============================================================================

struct BackupRotationConfig {
    bool enabled = true;
    int max_backups = 10;
    int64_t max_age_ms = 30LL * 24 * 60 * 60 * 1000;  // 30 days
    std::string backup_dir;
};

class ConfigBackup {
public:
    explicit ConfigBackup(const BackupRotationConfig& config);
    
    // Create backup
    std::string create_backup(const std::string& config_path);
    
    // List backups
    std::vector<std::string> list_backups(const std::string& config_path);
    
    // Restore from backup
    bool restore_backup(const std::string& backup_path, const std::string& target_path);
    
    // Rotate old backups
    void rotate_backups(const std::string& config_path);
    
    // Delete backup
    bool delete_backup(const std::string& backup_path);
    
private:
    BackupRotationConfig config_;
};

// ============================================================================
// 11. Config Schema (对齐 config/schema.ts)
// ============================================================================

// JSON Schema type
using ConfigSchema = json;

class ConfigSchemaBuilder {
public:
    // Generate JSON Schema for OpenClaw config
    static ConfigSchema build_schema();
    
    // Validate against schema
    static bool validate_against_schema(const json& config, const ConfigSchema& schema,
                                        std::vector<std::string>* errors = nullptr);
    
    // Get schema for specific section
    static ConfigSchema get_section_schema(const std::string& section);
    
    // Lookup schema by path
    static std::optional<ConfigSchema> lookup_schema(const ConfigSchema& root,
                                                      const std::vector<std::string>& path);
};

// ============================================================================
// 12. Utility Functions
// ============================================================================

// Generate config hash
std::string compute_config_hash(const json& config);

// Compare configs
bool configs_equal(const json& a, const json& b);

// Redact sensitive values in config
json redact_config_secrets(const json& config);

// Get config file extension
std::string get_config_extension(ConfigFormat format);

// Detect config format from file extension
ConfigFormat detect_config_format(const std::string& path);

}  // namespace config
}  // namespace hunterclaw
