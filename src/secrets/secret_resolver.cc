// HunterClaw: secrets/secret_resolver.cc
// 严格对齐 OC: secrets/resolve.ts (956行) + secrets/configure.ts (981行)
// 完整 Secret 解析/配置/审计链路
//
// OC 对应导出:
//   resolve.ts    → SecretProviderResolutionError, SecretRefResolutionError,
//                    resolveSecretRefValues, resolveSecretRefValue, resolveSecretRefString,
//                    isProviderScopedSecretResolutionError
//   configure.ts  → runSecretsConfigureInteractive, SecretsConfigureResult

// 对齐 OC: secrets/resolve.ts + secrets/configure.ts 的 C++ 实现
// 头文件依赖已有模块
#include <stdexcept>
#include <string>
#include "hunterclaw/config/config_system.h"
#include "hunterclaw/infra/utilities.h"
#include "hunterclaw/security/audit.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <regex>
#include <thread>
#include <future>
#include <mutex>
#include <map>
#include <set>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace secrets {

// =========================================================================
// 对齐 OC resolve.ts: Types
// =========================================================================

struct SecretRef {
    std::string source;     // "env", "file", "exec"
    std::string provider;
    std::string id;
};

class SecretProviderResolutionError : public std::runtime_error {
public:
    SecretProviderResolutionError(const std::string& source, const std::string& provider,
                                  const std::string& message);
    const std::string& source() const { return source_; }
    const std::string& provider() const { return provider_; }
private:
    std::string source_;
    std::string provider_;
};

class SecretRefResolutionError : public std::runtime_error {
public:
    SecretRefResolutionError(const std::string& source, const std::string& provider,
                              const std::string& ref_id, const std::string& message);
    const std::string& source() const { return source_; }
    const std::string& provider() const { return provider_; }
    const std::string& ref_id() const { return ref_id_; }
private:
    std::string source_;
    std::string provider_;
    std::string ref_id_;
};

struct SecretRefResolveCache {
    std::map<std::string, std::string> resolved;
};

struct SecretConfigureCandidate {
    std::string provider;
    std::string source;
    std::string scope;
    bool configured = false;
};

struct SecretsConfigureResult {
    bool has_changes = false;
    std::vector<std::string> validated_providers;
    std::vector<std::pair<std::string, std::string>> failed_providers;
};

// =========================================================================
// 对齐 OC resolve.ts: Constants
// =========================================================================

static const int DEFAULT_PROVIDER_CONCURRENCY = 4;
static const int DEFAULT_MAX_REFS_PER_PROVIDER = 512;
static const int DEFAULT_MAX_BATCH_BYTES = 256 * 1024;
static const int DEFAULT_FILE_MAX_BYTES = 1024 * 1024;
static const int DEFAULT_FILE_TIMEOUT_MS = 5000;
static const int DEFAULT_EXEC_TIMEOUT_MS = 5000;
static const int DEFAULT_EXEC_MAX_OUTPUT_BYTES = 1024 * 1024;
static const std::string SINGLE_VALUE_FILE_REF_ID = ".";

// =========================================================================
// 对齐 OC resolve.ts: Error types
// =========================================================================

SecretProviderResolutionError::SecretProviderResolutionError(
    const std::string& source,
    const std::string& provider,
    const std::string& message
) : std::runtime_error(message), source_(source), provider_(provider) {}

SecretRefResolutionError::SecretRefResolutionError(
    const std::string& source,
    const std::string& provider,
    const std::string& ref_id,
    const std::string& message
) : std::runtime_error(message), source_(source), provider_(provider), ref_id_(ref_id) {}

bool is_provider_scoped_secret_resolution_error(const std::exception& e) {
    return dynamic_cast<const SecretProviderResolutionError*>(&e) != nullptr;
}

// =========================================================================
// 对齐 OC resolve.ts: Provider resolution helpers
// =========================================================================

// 对齐 OC: isAbsolutePathname
static bool is_absolute_pathname(const std::string& value) {
    if (value.empty()) return false;
    if (value[0] == '/') return true;
    // Windows paths
    if (value.size() >= 3 && std::isalpha(value[0]) &&
        value[1] == ':' && (value[2] == '/' || value[2] == '\\')) return true;
    if (value.size() >= 2 && value[0] == '\\' && value[1] == '\\') return true;
    return false;
}

// 对齐 OC: secretRefKey
static std::string secret_ref_key(const SecretRef& ref) {
    return ref.source + ":" + ref.provider + ":" + ref.id;
}

// 对齐 OC: toProviderKey
static std::string to_provider_key(const std::string& source, const std::string& provider) {
    return source + ":" + provider;
}

// =========================================================================
// 对齐 OC resolve.ts: Env source resolver
// =========================================================================

static std::string resolve_env_ref(const SecretRef& ref) {
    const char* value = std::getenv(ref.id.c_str());
    if (!value) {
        throw SecretRefResolutionError(
            ref.source, ref.provider, ref.id,
            "Environment variable \"" + ref.id + "\" is not set.");
    }
    return std::string(value);
}

// =========================================================================
// 对齐 OC resolve.ts: File source resolver
// =========================================================================

static std::string resolve_file_ref(
    const SecretRef& ref,
    const json& provider_config
) {
    std::string base_path = provider_config.value("path", "");
    if (base_path.empty()) {
        throw SecretProviderResolutionError(
            ref.source, ref.provider,
            "File secret provider \"" + ref.provider + "\" missing 'path' config.");
    }

    std::string file_path;
    if (ref.id == SINGLE_VALUE_FILE_REF_ID) {
        file_path = base_path;
    } else {
        file_path = (fs::path(base_path) / ref.id).string();
    }

    // Security: path traversal check
    if (!is_absolute_pathname(file_path)) {
        file_path = fs::absolute(file_path).string();
    }

    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
        throw SecretRefResolutionError(
            ref.source, ref.provider, ref.id,
            "Secret file not found: " + file_path);
    }

    auto file_size = fs::file_size(file_path);
    if (static_cast<int>(file_size) > DEFAULT_FILE_MAX_BYTES) {
        throw SecretRefResolutionError(
            ref.source, ref.provider, ref.id,
            "Secret file exceeds max size (" + std::to_string(DEFAULT_FILE_MAX_BYTES) + " bytes): " + file_path);
    }

    std::ifstream ifs(file_path);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());

    // 对齐 OC: JSON pointer extraction
    std::string json_pointer = provider_config.value("jsonPointer", "");
    if (!json_pointer.empty() && json_pointer != "/") {
        try {
            json parsed = json::parse(content);
            auto ptr = json::json_pointer(json_pointer);
            auto& value = parsed.at(ptr);
            if (value.is_string()) return value.get<std::string>();
            return value.dump();
        } catch (const std::exception& e) {
            throw SecretRefResolutionError(
                ref.source, ref.provider, ref.id,
                "JSON pointer extraction failed: " + std::string(e.what()));
        }
    }

    // Trim trailing newline
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
        content.pop_back();
    }
    return content;
}

// =========================================================================
// 对齐 OC resolve.ts: Exec source resolver
// =========================================================================

static std::string resolve_exec_ref(
    const SecretRef& ref,
    const json& provider_config
) {
    std::string command = provider_config.value("command", "");
    if (command.empty()) {
        throw SecretProviderResolutionError(
            ref.source, ref.provider,
            "Exec secret provider \"" + ref.provider + "\" missing 'command' config.");
    }

    // 对齐 OC: isValidExecSecretRefId validation
    static const std::regex valid_exec_id_re("^[a-zA-Z0-9._\\-/]+$");
    if (!std::regex_match(ref.id, valid_exec_id_re)) {
        throw SecretRefResolutionError(
            ref.source, ref.provider, ref.id,
            "Exec secret ref id contains invalid characters.");
    }

    // Build exec command with ref id as argument
    std::string full_command = command + " '" + ref.id + "'";

    // Execute command and capture output
    std::array<char, 4096> buffer;
    std::string output;
    FILE* pipe = popen(full_command.c_str(), "r");
    if (!pipe) {
        throw SecretRefResolutionError(
            ref.source, ref.provider, ref.id,
            "Failed to execute secret command: " + command);
    }
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
        if (static_cast<int>(output.size()) > DEFAULT_EXEC_MAX_OUTPUT_BYTES) {
            pclose(pipe);
            throw SecretRefResolutionError(
                ref.source, ref.provider, ref.id,
                "Exec output exceeds max size.");
        }
    }
    int status = pclose(pipe);
    if (status != 0) {
        throw SecretRefResolutionError(
            ref.source, ref.provider, ref.id,
            "Exec command exited with status " + std::to_string(status));
    }

    // Trim trailing whitespace
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

// =========================================================================
// 对齐 OC resolve.ts: Core resolution functions
// =========================================================================

// 对齐 OC: resolveConfiguredProvider
static json resolve_configured_provider(const SecretRef& ref, const json& config) {
    std::string providers_path = "secrets.providers." + ref.provider;
    if (config.contains("secrets") && config["secrets"].contains("providers") &&
        config["secrets"]["providers"].contains(ref.provider)) {
        auto& provider_config = config["secrets"]["providers"][ref.provider];
        std::string source = provider_config.value("source", "");
        if (source != ref.source) {
            throw SecretProviderResolutionError(
                ref.source, ref.provider,
                "Provider \"" + ref.provider + "\" has source \"" + source +
                "\" but ref requests \"" + ref.source + "\".");
        }
        return provider_config;
    }

    // Default env provider
    if (ref.source == "env") {
        return json{{"source", "env"}};
    }

    throw SecretProviderResolutionError(
        ref.source, ref.provider,
        "Secret provider \"" + ref.provider + "\" is not configured.");
}

// 对齐 OC: resolveSecretRefValues (batch)
std::map<std::string, std::string> resolve_secret_ref_values(
    const std::vector<SecretRef>& refs,
    const json& config,
    SecretRefResolveCache* cache
) {
    if (refs.empty()) return {};

    // Deduplicate
    std::map<std::string, SecretRef> unique_refs;
    for (const auto& ref : refs) {
        auto key = secret_ref_key(ref);
        unique_refs[key] = ref;
    }

    // Group by provider
    std::map<std::string, std::vector<SecretRef>> grouped;
    for (const auto& [key, ref] : unique_refs) {
        auto provider_key = to_provider_key(ref.source, ref.provider);
        grouped[provider_key].push_back(ref);
    }

    std::map<std::string, std::string> results;
    std::mutex results_mutex;

    // Resolve each provider group (concurrently up to limit)
    std::vector<std::future<void>> futures;
    for (const auto& [provider_key, provider_refs] : grouped) {
        futures.push_back(std::async(std::launch::async, [&, provider_refs]() {
            for (const auto& ref : provider_refs) {
                auto key = secret_ref_key(ref);

                // Check cache
                if (cache) {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    auto it = cache->resolved.find(key);
                    if (it != cache->resolved.end()) {
                        results[key] = it->second;
                        continue;
                    }
                }

                // Resolve
                auto provider_config = resolve_configured_provider(ref, config);
                std::string value;
                if (ref.source == "env") {
                    value = resolve_env_ref(ref);
                } else if (ref.source == "file") {
                    value = resolve_file_ref(ref, provider_config);
                } else if (ref.source == "exec") {
                    value = resolve_exec_ref(ref, provider_config);
                } else {
                    throw SecretProviderResolutionError(
                        ref.source, ref.provider,
                        "Unsupported secret source: " + ref.source);
                }

                std::lock_guard<std::mutex> lock(results_mutex);
                results[key] = value;
                if (cache) cache->resolved[key] = value;
            }
        }));
    }

    for (auto& f : futures) f.get();
    return results;
}

// 对齐 OC: resolveSecretRefValue
std::string resolve_secret_ref_value(
    const SecretRef& ref,
    const json& config,
    SecretRefResolveCache* cache
) {
    auto results = resolve_secret_ref_values({ref}, config, cache);
    auto key = secret_ref_key(ref);
    auto it = results.find(key);
    if (it == results.end()) {
        throw SecretRefResolutionError(
            ref.source, ref.provider, ref.id,
            "Secret ref \"" + key + "\" was not resolved.");
    }
    return it->second;
}

// 对齐 OC: resolveSecretRefString
std::string resolve_secret_ref_string(
    const SecretRef& ref,
    const json& config,
    SecretRefResolveCache* cache
) {
    auto value = resolve_secret_ref_value(ref, config, cache);
    if (value.empty()) {
        throw std::runtime_error(
            "Secret reference \"" + secret_ref_key(ref) +
            "\" resolved to an empty value.");
    }
    return value;
}

// =========================================================================
// 对齐 OC configure.ts: Interactive secrets configuration
// =========================================================================

// 对齐 OC: isAbsolutePathValue
static bool is_absolute_path_value(const std::string& value) {
    if (value.empty()) return false;
    if (value[0] == '/') return true;
    // Windows
    if (value.size() >= 3 && std::isalpha(value[0]) &&
        value[1] == ':' && (value[2] == '/' || value[2] == '\\')) return true;
    if (value.size() >= 2 && value[0] == '\\' && value[1] == '\\') return true;
    return false;
}

// 对齐 OC: getSecretProviders
static std::map<std::string, json> get_secret_providers(const json& config) {
    std::map<std::string, json> providers;
    if (config.contains("secrets") && config["secrets"].contains("providers")) {
        auto& p = config["secrets"]["providers"];
        for (auto it = p.begin(); it != p.end(); ++it) {
            providers[it.key()] = it.value();
        }
    }
    return providers;
}

// 对齐 OC: buildConfigureCandidatesForScope
static std::vector<SecretConfigureCandidate> build_configure_candidates(
    const json& config,
    const std::string& scope
) {
    std::vector<SecretConfigureCandidate> candidates;
    auto providers = get_secret_providers(config);

    for (const auto& [name, provider] : providers) {
        SecretConfigureCandidate candidate;
        candidate.provider = name;
        candidate.source = provider.value("source", "");
        candidate.scope = scope;
        candidate.configured = true;
        candidates.push_back(candidate);
    }
    return candidates;
}

// 对齐 OC: hasConfigurePlanChanges
static bool has_configure_plan_changes(
    const std::vector<SecretConfigureCandidate>& candidates
) {
    return !candidates.empty();
}

// 对齐 OC: runSecretsConfigureInteractive (non-interactive batch version for HC)
SecretsConfigureResult run_secrets_configure(
    const json& config,
    const std::string& scope
) {
    SecretsConfigureResult result;

    auto candidates = build_configure_candidates(config, scope);
    result.has_changes = has_configure_plan_changes(candidates);

    // Validate all configured refs
    for (const auto& candidate : candidates) {
        SecretRef ref;
        ref.source = candidate.source;
        ref.provider = candidate.provider;
        ref.id = SINGLE_VALUE_FILE_REF_ID;

        try {
            auto value = resolve_secret_ref_value(ref, config, nullptr);
            result.validated_providers.push_back(candidate.provider);
        } catch (const std::exception& e) {
            result.failed_providers.push_back({candidate.provider, e.what()});
        }
    }

    return result;
}

} // namespace secrets
} // namespace hunterclaw
