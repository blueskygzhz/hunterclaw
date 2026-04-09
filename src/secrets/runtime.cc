#include "hunterclaw/secrets/runtime.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

namespace hunterclaw {
namespace secrets {

// ============================================================================
// SecretRef Implementation
// ============================================================================

std::string SecretRef::key() const {
    return source_to_string(source) + ":" + provider + ":" + ref_id;
}

nlohmann::json SecretRef::to_json() const {
    return {
        {"source", source_to_string(source)},
        {"provider", provider},
        {"refId", ref_id}
    };
}

SecretRef SecretRef::from_json(const nlohmann::json& j) {
    SecretRef ref;
    ref.source = string_to_source(j.at("source").get<std::string>());
    ref.provider = j.at("provider").get<std::string>();
    ref.ref_id = j.at("refId").get<std::string>();
    return ref;
}

// ============================================================================
// FileSecretProviderConfig Implementation
// ============================================================================

nlohmann::json FileSecretProviderConfig::to_json() const {
    nlohmann::json j = {
        {"type", type},
        {"path", path},
        {"format", format}
    };
    if (max_bytes) j["maxBytes"] = *max_bytes;
    if (timeout_ms) j["timeoutMs"] = *timeout_ms;
    return j;
}

FileSecretProviderConfig FileSecretProviderConfig::from_json(const nlohmann::json& j) {
    FileSecretProviderConfig cfg;
    cfg.type = j.value("type", "file");
    cfg.path = j.at("path").get<std::string>();
    cfg.format = j.value("format", "json");
    if (j.contains("maxBytes")) cfg.max_bytes = j["maxBytes"].get<int>();
    if (j.contains("timeoutMs")) cfg.timeout_ms = j["timeoutMs"].get<int>();
    return cfg;
}

// ============================================================================
// ExecSecretProviderConfig Implementation
// ============================================================================

nlohmann::json ExecSecretProviderConfig::to_json() const {
    nlohmann::json j = {
        {"type", type},
        {"command", command},
        {"format", format}
    };
    if (!args.empty()) j["args"] = args;
    if (timeout_ms) j["timeoutMs"] = *timeout_ms;
    if (max_output_bytes) j["maxOutputBytes"] = *max_output_bytes;
    return j;
}

ExecSecretProviderConfig ExecSecretProviderConfig::from_json(const nlohmann::json& j) {
    ExecSecretProviderConfig cfg;
    cfg.type = j.value("type", "exec");
    cfg.command = j.at("command").get<std::string>();
    cfg.format = j.value("format", "json");
    if (j.contains("args")) {
        for (const auto& arg : j["args"]) {
            cfg.args.push_back(arg.get<std::string>());
        }
    }
    if (j.contains("timeoutMs")) cfg.timeout_ms = j["timeoutMs"].get<int>();
    if (j.contains("maxOutputBytes")) cfg.max_output_bytes = j["maxOutputBytes"].get<int>();
    return cfg;
}

// ============================================================================
// SecretProviderConfig Implementation
// ============================================================================

nlohmann::json SecretProviderConfig::to_json() const {
    if (is_file()) {
        return file_config->to_json();
    } else if (is_exec()) {
        return exec_config->to_json();
    }
    return {{"type", type}};
}

SecretProviderConfig SecretProviderConfig::from_json(const nlohmann::json& j) {
    SecretProviderConfig cfg;
    cfg.type = j.at("type").get<std::string>();
    if (cfg.type == "file") {
        cfg.file_config = FileSecretProviderConfig::from_json(j);
    } else if (cfg.type == "exec") {
        cfg.exec_config = ExecSecretProviderConfig::from_json(j);
    }
    return cfg;
}

// ============================================================================
// SecretResolverWarning Implementation
// ============================================================================

nlohmann::json SecretResolverWarning::to_json() const {
    return {
        {"code", code},
        {"path", path},
        {"message", message}
    };
}

SecretResolverWarning SecretResolverWarning::from_json(const nlohmann::json& j) {
    SecretResolverWarning warning;
    warning.code = j.at("code").get<std::string>();
    warning.path = j.at("path").get<std::string>();
    warning.message = j.at("message").get<std::string>();
    return warning;
}

// ============================================================================
// SecretRefResolveCache Implementation
// ============================================================================

std::optional<SecretResolutionResult> SecretRefResolveCache::get_resolved(
    const std::string& ref_key
) const {
    auto it = resolved_by_ref_key_.find(ref_key);
    if (it != resolved_by_ref_key_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void SecretRefResolveCache::set_resolved(
    const std::string& ref_key,
    const SecretResolutionResult& result
) {
    resolved_by_ref_key_[ref_key] = result;
}

void SecretRefResolveCache::clear() {
    resolved_by_ref_key_.clear();
}

// ============================================================================
// AuthProfileStore Implementation
// ============================================================================

nlohmann::json AuthProfileStore::to_json() const {
    return {
        {"agentDir", agent_dir},
        {"profiles", profiles}
    };
}

AuthProfileStore AuthProfileStore::from_json(const nlohmann::json& j) {
    AuthProfileStore store;
    store.agent_dir = j.at("agentDir").get<std::string>();
    store.profiles = j.at("profiles").get<std::map<std::string, nlohmann::json>>();
    return store;
}

// ============================================================================
// PreparedSecretsRuntimeSnapshot Implementation
// ============================================================================

nlohmann::json PreparedSecretsRuntimeSnapshot::to_json() const {
    nlohmann::json auth_stores_json = nlohmann::json::array();
    for (const auto& entry : auth_stores) {
        auth_stores_json.push_back({
            {"agentDir", entry.agent_dir},
            {"store", entry.store.to_json()}
        });
    }
    
    nlohmann::json warnings_json = nlohmann::json::array();
    for (const auto& warning : warnings) {
        warnings_json.push_back(warning.to_json());
    }
    
    return {
        {"sourceConfig", source_config},
        {"config", config},
        {"authStores", auth_stores_json},
        {"warnings", warnings_json}
    };
}

PreparedSecretsRuntimeSnapshot PreparedSecretsRuntimeSnapshot::from_json(const nlohmann::json& j) {
    PreparedSecretsRuntimeSnapshot snapshot;
    snapshot.source_config = j.at("sourceConfig");
    snapshot.config = j.at("config");
    
    for (const auto& entry_json : j.at("authStores")) {
        AuthStoreEntry entry;
        entry.agent_dir = entry_json.at("agentDir").get<std::string>();
        entry.store = AuthProfileStore::from_json(entry_json.at("store"));
        snapshot.auth_stores.push_back(entry);
    }
    
    for (const auto& warning_json : j.at("warnings")) {
        snapshot.warnings.push_back(SecretResolverWarning::from_json(warning_json));
    }
    
    return snapshot;
}

// ============================================================================
// SecretsRuntimeManager Implementation
// ============================================================================

PreparedSecretsRuntimeSnapshot SecretsRuntimeManager::prepare_snapshot(
    const nlohmann::json& config,
    const std::map<std::string, std::string>& env,
    const std::vector<std::string>& agent_dirs,
    std::shared_ptr<SecretRefResolveCache> cache
) {
    PreparedSecretsRuntimeSnapshot snapshot;
    snapshot.source_config = config;  // Clone original config
    snapshot.config = config;         // Start with original, will be modified
    
    if (!cache) {
        cache = std::make_shared<SecretRefResolveCache>();
    }
    
    // Step 1: Collect all secret references from config
    std::vector<SecretRef> refs;
    std::vector<SecretResolverWarning> warnings;
    collect_secret_refs_from_config(snapshot.config, "", refs, warnings);
    
    // Step 2: Resolve secret references
    auto resolved = resolve_secret_refs(refs, config, env, cache);
    
    // Step 3: Replace secret references in config with resolved values
    replace_secret_refs_in_config(snapshot.config, resolved, warnings);
    
    // Step 4: Load auth profile stores (if agent_dirs provided)
    for (const auto& agent_dir : agent_dirs) {
        // TODO: Implement auth profile store loading
        // For now, just create empty store
        PreparedSecretsRuntimeSnapshot::AuthStoreEntry entry;
        entry.agent_dir = agent_dir;
        entry.store.agent_dir = agent_dir;
        snapshot.auth_stores.push_back(entry);
    }
    
    snapshot.warnings = warnings;
    
    return snapshot;
}

void SecretsRuntimeManager::activate_snapshot(const PreparedSecretsRuntimeSnapshot& snapshot) {
    active_snapshot_ = snapshot;
}

std::optional<PreparedSecretsRuntimeSnapshot> SecretsRuntimeManager::get_active_snapshot() const {
    return active_snapshot_;
}

void SecretsRuntimeManager::clear_snapshot() {
    active_snapshot_ = std::nullopt;
}

std::map<std::string, SecretResolutionResult> SecretsRuntimeManager::resolve_secret_refs(
    const std::vector<SecretRef>& refs,
    const nlohmann::json& config,
    const std::map<std::string, std::string>& env,
    std::shared_ptr<SecretRefResolveCache> cache
) {
    std::map<std::string, SecretResolutionResult> results;
    
    if (!cache) {
        cache = std::make_shared<SecretRefResolveCache>();
    }
    
    // Group refs by provider
    std::map<std::string, std::vector<SecretRef>> refs_by_provider;
    for (const auto& ref : refs) {
        refs_by_provider[ref.provider].push_back(ref);
    }
    
    // Resolve each provider
    for (const auto& [provider_name, provider_refs] : refs_by_provider) {
        // Check cache first
        bool all_cached = true;
        for (const auto& ref : provider_refs) {
            if (!cache->get_resolved(ref.key())) {
                all_cached = false;
                break;
            }
        }
        
        if (all_cached) {
            // All refs cached, skip resolution
            for (const auto& ref : provider_refs) {
                auto cached = cache->get_resolved(ref.key());
                if (cached) {
                    results[ref.key()] = *cached;
                }
            }
            continue;
        }
        
        // Get provider config from config
        std::map<std::string, SecretResolutionResult> provider_results;
        
        if (config.contains("secrets") && config["secrets"].contains("providers")) {
            const auto& providers = config["secrets"]["providers"];
            if (providers.contains(provider_name)) {
                auto provider_config = SecretProviderConfig::from_json(providers[provider_name]);
                
                if (provider_config.is_file()) {
                    provider_results = resolve_file_secret_provider(
                        provider_name, *provider_config.file_config, provider_refs, cache
                    );
                } else if (provider_config.is_exec()) {
                    provider_results = resolve_exec_secret_provider(
                        provider_name, *provider_config.exec_config, provider_refs, cache
                    );
                }
            }
        } else if (provider_name == "env") {
            // Environment variable provider (built-in)
            provider_results = resolve_env_secret_provider(provider_refs, env);
        }
        
        // Merge provider results
        for (const auto& [key, result] : provider_results) {
            results[key] = result;
            cache->set_resolved(key, result);
        }
    }
    
    return results;
}

// ============================================================================
// Secret Reference Collection
// ============================================================================

void SecretsRuntimeManager::collect_secret_refs_from_config(
    const nlohmann::json& config,
    const std::string& path_prefix,
    std::vector<SecretRef>& refs,
    std::vector<SecretResolverWarning>& warnings
) {
    if (config.is_object()) {
        for (auto it = config.begin(); it != config.end(); ++it) {
            std::string new_path = path_prefix.empty() ? it.key() : path_prefix + "." + it.key();
            collect_secret_refs_from_config(it.value(), new_path, refs, warnings);
        }
    } else if (config.is_array()) {
        for (size_t i = 0; i < config.size(); ++i) {
            std::string new_path = path_prefix + "[" + std::to_string(i) + "]";
            collect_secret_refs_from_config(config[i], new_path, refs, warnings);
        }
    } else if (config.is_string()) {
        std::string value = config.get<std::string>();
        auto ref = parse_secret_ref_string(value);
        if (ref) {
            refs.push_back(*ref);
        }
    }
}

std::optional<SecretRef> SecretsRuntimeManager::parse_secret_ref_string(const std::string& value) {
    // Match patterns: ${secret:refId}, ${secret:provider:refId}, ${file:path#key}, ${exec:command#key}
    std::regex secret_ref_pattern(R"(\$\{(secret|file|exec):([^:}]+)(?::([^}]+))?\})");
    std::smatch match;
    
    if (!std::regex_match(value, match, secret_ref_pattern)) {
        return std::nullopt;
    }
    
    SecretRef ref;
    std::string type = match[1].str();
    std::string first_part = match[2].str();
    std::string second_part = match[3].str();
    
    if (type == "secret") {
        if (!second_part.empty()) {
            // ${secret:provider:refId}
            ref.source = SecretRefSource::File;  // Default to file
            ref.provider = first_part;
            ref.ref_id = second_part;
        } else {
            // ${secret:refId} - use default provider
            ref.source = SecretRefSource::File;
            ref.provider = "default";
            ref.ref_id = first_part;
        }
    } else if (type == "file") {
        // ${file:path#key}
        ref.source = SecretRefSource::File;
        ref.provider = "file";
        ref.ref_id = first_part;
    } else if (type == "exec") {
        // ${exec:command#key}
        ref.source = SecretRefSource::Exec;
        ref.provider = "exec";
        ref.ref_id = first_part;
    }
    
    return ref;
}

// ============================================================================
// Secret Resolution - File Provider
// ============================================================================

std::map<std::string, SecretResolutionResult> SecretsRuntimeManager::resolve_file_secret_provider(
    const std::string& provider_name,
    const FileSecretProviderConfig& provider_config,
    const std::vector<SecretRef>& refs,
    std::shared_ptr<SecretRefResolveCache> cache
) {
    std::map<std::string, SecretResolutionResult> results;
    
    // Expand ~ in path
    std::string path = provider_config.path;
    if (path.compare(0, 2, "~/") == 0) {
        const char* home = std::getenv("HOME");
        if (home) {
            path = std::string(home) + path.substr(1);
        }
    }
    
    // Read file
    std::ifstream file(path);
    if (!file.is_open()) {
        for (const auto& ref : refs) {
            results[ref.key()] = SecretResolutionResult::fail("Failed to open file: " + path);
        }
        return results;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // Parse based on format
    if (provider_config.format == "json") {
        try {
            auto json_data = nlohmann::json::parse(content);
            for (const auto& ref : refs) {
                if (json_data.contains(ref.ref_id)) {
                    results[ref.key()] = SecretResolutionResult::ok(json_data[ref.ref_id]);
                } else {
                    results[ref.key()] = SecretResolutionResult::fail("Key not found: " + ref.ref_id);
                }
            }
        } catch (const std::exception& e) {
            for (const auto& ref : refs) {
                results[ref.key()] = SecretResolutionResult::fail("JSON parse error: " + std::string(e.what()));
            }
        }
    } else if (provider_config.format == "env") {
        // Parse .env format: KEY=value
        std::map<std::string, std::string> env_vars;
        std::istringstream stream(content);
        std::string line;
        while (std::getline(stream, line)) {
            size_t eq_pos = line.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = line.substr(0, eq_pos);
                std::string value = line.substr(eq_pos + 1);
                env_vars[key] = value;
            }
        }
        
        for (const auto& ref : refs) {
            if (env_vars.count(ref.ref_id)) {
                results[ref.key()] = SecretResolutionResult::ok(env_vars[ref.ref_id]);
            } else {
                results[ref.key()] = SecretResolutionResult::fail("Key not found: " + ref.ref_id);
            }
        }
    }
    
    return results;
}

// ============================================================================
// Secret Resolution - Exec Provider
// ============================================================================

std::map<std::string, SecretResolutionResult> SecretsRuntimeManager::resolve_exec_secret_provider(
    const std::string& provider_name,
    const ExecSecretProviderConfig& provider_config,
    const std::vector<SecretRef>& refs,
    std::shared_ptr<SecretRefResolveCache> cache
) {
    std::map<std::string, SecretResolutionResult> results;
    
    // 🎯 严格对齐 OpenClaw: secrets exec provider
    // 通过外部命令解析 secret values (e.g. `op read op://vault/item/password`)
    for (const auto& ref : refs) {
        // Check cache first
        if (cache) {
            auto cached = cache->get(ref.key());
            if (cached.has_value()) {
                results[ref.key()] = SecretResolutionResult::ok(*cached);
                continue;
            }
        }

        // Build command: replace {ref} placeholder with ref_id
        std::string cmd = provider_config.command;
        size_t pos = cmd.find("{ref}");
        if (pos != std::string::npos) {
            cmd.replace(pos, 5, ref.ref_id);
        } else {
            cmd += " " + ref.ref_id;
        }

        // Execute command with popen (对齐 OpenClaw: child_process.execSync)
        std::string output;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            results[ref.key()] = SecretResolutionResult::fail(
                "Failed to execute command: " + cmd);
            continue;
        }

        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            output += buffer;
        }
        int exit_code = pclose(pipe);

        // Trim trailing newline
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }

        if (exit_code != 0 || output.empty()) {
            results[ref.key()] = SecretResolutionResult::fail(
                "Exec provider returned exit code " + std::to_string(exit_code) +
                " for ref: " + ref.ref_id);
        } else {
            results[ref.key()] = SecretResolutionResult::ok(output);
            // Update cache
            if (cache) {
                cache->put(ref.key(), output);
            }
        }
    }
    
    return results;
}

// ============================================================================
// Secret Resolution - Env Provider
// ============================================================================

std::map<std::string, SecretResolutionResult> SecretsRuntimeManager::resolve_env_secret_provider(
    const std::vector<SecretRef>& refs,
    const std::map<std::string, std::string>& env
) {
    std::map<std::string, SecretResolutionResult> results;
    
    for (const auto& ref : refs) {
        auto it = env.find(ref.ref_id);
        if (it != env.end()) {
            results[ref.key()] = SecretResolutionResult::ok(it->second);
        } else {
            results[ref.key()] = SecretResolutionResult::fail("Environment variable not found: " + ref.ref_id);
        }
    }
    
    return results;
}

// ============================================================================
// Config Replacement
// ============================================================================

void SecretsRuntimeManager::replace_secret_refs_in_config(
    nlohmann::json& config,
    const std::map<std::string, SecretResolutionResult>& resolved_refs,
    std::vector<SecretResolverWarning>& warnings
) {
    if (config.is_object()) {
        for (auto it = config.begin(); it != config.end(); ++it) {
            replace_secret_refs_in_config(it.value(), resolved_refs, warnings);
        }
    } else if (config.is_array()) {
        for (auto& element : config) {
            replace_secret_refs_in_config(element, resolved_refs, warnings);
        }
    } else if (config.is_string()) {
        std::string value = config.get<std::string>();
        auto ref = parse_secret_ref_string(value);
        if (ref) {
            auto it = resolved_refs.find(ref->key());
            if (it != resolved_refs.end() && it->second.success) {
                config = *it->second.value;
            } else {
                // Failed to resolve - add warning
                SecretResolverWarning warning;
                warning.code = "SECRETS_REF_RESOLUTION_FAILED";
                warning.path = ref->key();
                warning.message = it != resolved_refs.end() ? *it->second.error : "Secret not found";
                warnings.push_back(warning);
            }
        }
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

std::string source_to_string(SecretRefSource source) {
    switch (source) {
        case SecretRefSource::File: return "file";
        case SecretRefSource::Exec: return "exec";
        case SecretRefSource::Env:  return "env";
        default:                    return "unknown";
    }
}

SecretRefSource string_to_source(const std::string& source_str) {
    if (source_str == "file") return SecretRefSource::File;
    if (source_str == "exec") return SecretRefSource::Exec;
    if (source_str == "env")  return SecretRefSource::Env;
    return SecretRefSource::Unknown;
}

}  // namespace secrets
}  // namespace hunterclaw
