// 对齐 OC: src/config/defaults.ts (383行) + src/config/io.ts (1814行)
// 配置默认值系统 + 配置 IO 核心
#include "hunterclaw/config/config_system.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <regex>
#include <set>
#include <map>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace config {

// =========================================================================
// 对齐 OC defaults.ts: Constants
// =========================================================================

// 对齐 OC: DEFAULT_MODEL_ALIASES
static const std::map<std::string, std::string> DEFAULT_MODEL_ALIASES = {
    {"opus", "anthropic/claude-opus-4-6"},
    {"sonnet", "anthropic/claude-sonnet-4-6"},
    {"gpt", "openai/gpt-5.4"},
    {"gpt-mini", "openai/gpt-5.4-mini"},
    {"gpt-nano", "openai/gpt-5.4-nano"},
    {"gemini", "google/gemini-3.1-pro-preview"},
    {"gemini-flash", "google/gemini-3-flash-preview"},
    {"gemini-flash-lite", "google/gemini-3.1-flash-lite-preview"},
};

static const int DEFAULT_MODEL_MAX_TOKENS = 8192;
static const int DEFAULT_AGENT_MAX_CONCURRENT = 4;
static const int DEFAULT_SUBAGENT_MAX_CONCURRENT = 2;

struct ModelCost {
    double input = 0.0;
    double output = 0.0;
    double cache_read = 0.0;
    double cache_write = 0.0;
};

struct ModelDefinition {
    std::string id;
    std::string name;
    std::string provider;
    int max_tokens = DEFAULT_MODEL_MAX_TOKENS;
    int context_window = 0;
    std::vector<std::string> input = {"text"};
    ModelCost cost;
};

// 对齐 OC: resolveModelCost
static ModelCost resolve_model_cost(const json& raw) {
    ModelCost cost;
    if (!raw.is_object()) return cost;
    if (raw.contains("input") && raw["input"].is_number())
        cost.input = raw["input"].get<double>();
    if (raw.contains("output") && raw["output"].is_number())
        cost.output = raw["output"].get<double>();
    if (raw.contains("cacheRead") && raw["cacheRead"].is_number())
        cost.cache_read = raw["cacheRead"].get<double>();
    if (raw.contains("cacheWrite") && raw["cacheWrite"].is_number())
        cost.cache_write = raw["cacheWrite"].get<double>();
    return cost;
}

// 对齐 OC: resolveModelAlias
std::string resolve_model_alias(const std::string& alias) {
    auto it = DEFAULT_MODEL_ALIASES.find(alias);
    if (it != DEFAULT_MODEL_ALIASES.end()) return it->second;
    return alias;
}

// 对齐 OC: buildModelAliasIndex (from defaults)
std::map<std::string, std::string> build_default_model_alias_index() {
    return DEFAULT_MODEL_ALIASES;
}

// 对齐 OC: applyConfigDefaults
json apply_config_defaults(const json& raw_config) {
    json cfg = raw_config;

    // Ensure basic structure
    if (!cfg.contains("agents") || !cfg["agents"].is_object()) {
        cfg["agents"] = json::object();
    }
    if (!cfg["agents"].contains("defaults")) {
        cfg["agents"]["defaults"] = json::object();
    }

    // 对齐 OC: DEFAULT_AGENT_MAX_CONCURRENT
    auto& defaults = cfg["agents"]["defaults"];
    if (!defaults.contains("maxConcurrent")) {
        defaults["maxConcurrent"] = DEFAULT_AGENT_MAX_CONCURRENT;
    }
    if (!defaults.contains("subagentMaxConcurrent")) {
        defaults["subagentMaxConcurrent"] = DEFAULT_SUBAGENT_MAX_CONCURRENT;
    }

    // 对齐 OC: normalize talk config defaults
    if (!cfg.contains("talk")) {
        cfg["talk"] = json::object();
    }

    // 对齐 OC: normalize provider config defaults
    if (cfg.contains("providers") && cfg["providers"].is_object()) {
        for (auto& [key, provider] : cfg["providers"].items()) {
            if (!provider.is_object()) continue;
            // Ensure provider has normalized id
            if (!provider.contains("id")) {
                provider["id"] = key;
            }
        }
    }

    return cfg;
}

// =========================================================================
// 对齐 OC io.ts: Config File I/O (核心读写)
// =========================================================================

// 对齐 OC: CONFIG_FILE_NAMES
static const std::vector<std::string> CONFIG_FILE_NAMES = {
    "hunterclaw.json5",
    "hunterclaw.json",
    ".hunterclaw.json5",
    ".hunterclaw.json",
};

struct ConfigLoadResult {
    json config;
    std::string source_path;
    std::vector<std::string> warnings;
    bool loaded = false;
    std::string error;
};

// ConfigWriteOptions 已在 config_system.h 中定义 (backup, atomic, indent, sort_keys)

struct ConfigWriteResult {
    bool ok = false;
    std::string path;
    std::string error;
    json previous;
    json current;
};

// 对齐 OC: resolveConfigPath
static std::string resolve_config_path(const std::string& dir) {
    for (const auto& name : CONFIG_FILE_NAMES) {
        fs::path candidate = fs::path(dir) / name;
        if (fs::exists(candidate)) {
            return candidate.string();
        }
    }
    return (fs::path(dir) / CONFIG_FILE_NAMES[0]).string();
}

// 对齐 OC: loadConfigFromFile (simplified JSON5 → JSON)
static ConfigLoadResult load_config_from_file(const std::string& path) {
    ConfigLoadResult result;
    result.source_path = path;

    if (!fs::exists(path)) {
        result.loaded = false;
        return result;
    }

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            result.error = "Failed to open config file: " + path;
            return result;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

        // Strip JSON5 comments (single-line and multi-line)
        std::string stripped;
        bool in_string = false;
        bool in_single_comment = false;
        bool in_multi_comment = false;

        for (size_t i = 0; i < content.size(); ++i) {
            if (in_single_comment) {
                if (content[i] == '\n') {
                    in_single_comment = false;
                    stripped += '\n';
                }
                continue;
            }
            if (in_multi_comment) {
                if (content[i] == '*' && i + 1 < content.size() && content[i + 1] == '/') {
                    in_multi_comment = false;
                    ++i;
                }
                continue;
            }
            if (in_string) {
                stripped += content[i];
                if (content[i] == '"' && (i == 0 || content[i - 1] != '\\')) {
                    in_string = false;
                }
                continue;
            }
            if (content[i] == '"') {
                in_string = true;
                stripped += content[i];
                continue;
            }
            if (content[i] == '/' && i + 1 < content.size()) {
                if (content[i + 1] == '/') {
                    in_single_comment = true;
                    continue;
                }
                if (content[i + 1] == '*') {
                    in_multi_comment = true;
                    ++i;
                    continue;
                }
            }
            // Handle trailing commas in objects/arrays (JSON5 feature)
            stripped += content[i];
        }

        // Remove trailing commas before } or ]
        std::regex trailing_comma(R"(,(\s*[}\]]))");
        stripped = std::regex_replace(stripped, trailing_comma, "$1");

        result.config = json::parse(stripped);
        result.loaded = true;
    } catch (const std::exception& e) {
        result.error = std::string("Config parse error: ") + e.what();
    }

    return result;
}

// 对齐 OC: resolveConfigEnvVars
static json resolve_config_env_vars(const json& config) {
    if (config.is_string()) {
        std::string val = config.get<std::string>();
        // 对齐 OC: ${ENV_VAR} substitution
        std::regex env_ref(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)\})");
        std::smatch match;
        std::string result = val;
        while (std::regex_search(result, match, env_ref)) {
            const char* env_val = std::getenv(match[1].str().c_str());
            if (env_val) {
                result = match.prefix().str() + std::string(env_val) + match.suffix().str();
            } else {
                break; // Leave unresolved refs as-is
            }
        }
        return json(result);
    }
    if (config.is_object()) {
        json result = json::object();
        for (auto& [key, value] : config.items()) {
            result[key] = resolve_config_env_vars(value);
        }
        return result;
    }
    if (config.is_array()) {
        json result = json::array();
        for (const auto& item : config) {
            result.push_back(resolve_config_env_vars(item));
        }
        return result;
    }
    return config;
}

// 对齐 OC: loadConfig (main entry)
ConfigLoadResult load_config(const std::string& config_dir) {
    std::string path = resolve_config_path(config_dir);
    auto result = load_config_from_file(path);

    if (!result.loaded) return result;

    // Apply env var substitution
    result.config = resolve_config_env_vars(result.config);

    // Apply defaults
    result.config = apply_config_defaults(result.config);

    return result;
}

// 对齐 OC: writeConfig
ConfigWriteResult write_config(const std::string& config_path,
                                const json& config,
                                const ConfigWriteOptions& options) {
    ConfigWriteResult result;
    result.path = config_path;

    try {
        // Read existing config for audit
        if (fs::exists(config_path)) {
            std::ifstream existing(config_path);
            std::string content((std::istreambuf_iterator<char>(existing)),
                               std::istreambuf_iterator<char>());
            try {
                result.previous = json::parse(content);
            } catch (...) {
                // Ignore parse errors on existing config
            }
        }

        // 对齐 OC: maintainConfigBackups
        if (options.backup && fs::exists(config_path)) {
            std::string backup_dir = (fs::path(config_path).parent_path() / "backups").string();
            fs::create_directories(backup_dir);

            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            std::stringstream ts;
            ts << std::put_time(std::localtime(&time_t), "%Y%m%d-%H%M%S");

            std::string backup_path = backup_dir + "/config-" + ts.str() + ".json";
            fs::copy_file(config_path, backup_path,
                         fs::copy_options::overwrite_existing);

            // 对齐 OC: prune old backups (keep last 10)
            std::vector<fs::directory_entry> backups;
            for (const auto& entry : fs::directory_iterator(backup_dir)) {
                if (entry.is_regular_file() && entry.path().filename().string().find("config-") == 0) {
                    backups.push_back(entry);
                }
            }
            if (backups.size() > 10) {
                std::sort(backups.begin(), backups.end(),
                    [](const auto& a, const auto& b) {
                        return a.last_write_time() < b.last_write_time();
                    });
                for (size_t i = 0; i < backups.size() - 10; ++i) {
                    fs::remove(backups[i].path());
                }
            }
        }

        // Write the config with pretty formatting
        std::string serialized = config.dump(2);
        std::ofstream out(config_path);
        if (!out.is_open()) {
            result.error = "Failed to open config file for writing: " + config_path;
            return result;
        }
        out << serialized;
        out.close();

        result.ok = true;
        result.current = config;
    } catch (const std::exception& e) {
        result.error = std::string("Config write error: ") + e.what();
    }

    return result;
}

// 对齐 OC: createMergePatch
json create_merge_patch(const json& source, const json& target) {
    if (!source.is_object() || !target.is_object()) {
        return target;
    }

    json patch = json::object();
    // Keys in target but different from source
    for (auto& [key, value] : target.items()) {
        if (!source.contains(key) || source[key] != value) {
            if (source.contains(key) && source[key].is_object() && value.is_object()) {
                auto sub_patch = create_merge_patch(source[key], value);
                if (!sub_patch.empty()) {
                    patch[key] = sub_patch;
                }
            } else {
                patch[key] = value;
            }
        }
    }
    // Keys removed from source
    for (auto& [key, value] : source.items()) {
        if (!target.contains(key)) {
            patch[key] = nullptr;
        }
    }
    return patch;
}

// 对齐 OC: collectChangedPaths
std::vector<std::string> collect_changed_paths(const json& patch,
                                                 const std::string& prefix) {
    std::vector<std::string> paths;
    if (!patch.is_object()) return paths;

    for (auto& [key, value] : patch.items()) {
        std::string current_path = prefix.empty() ? key : (prefix + "." + key);
        if (value.is_object() && !value.is_null()) {
            auto sub_paths = collect_changed_paths(value, current_path);
            paths.insert(paths.end(), sub_paths.begin(), sub_paths.end());
        } else {
            paths.push_back(current_path);
        }
    }
    return paths;
}

}  // namespace config
}  // namespace hunterclaw
