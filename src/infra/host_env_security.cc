// 🎯 对齐 OpenClaw: src/infra/host-env-security.ts
// Implements: sanitizeHostExecEnv, sanitizeSystemRunEnvOverrides,
//             isDangerousHostEnvVarName, isDangerousHostEnvOverrideVarName

#include "hunterclaw/security/host_env_security.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>

namespace hunterclaw {
namespace security {

// ============================================================================
// Internal helpers
// ============================================================================

// 🎯 对齐 OpenClaw: PORTABLE_ENV_VAR_KEY = /^[A-Za-z_][A-Za-z0-9_]*$/
static bool is_portable_env_var_key(const std::string& key) {
    if (key.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(key[0])) && key[0] != '_') return false;
    for (size_t i = 1; i < key.size(); ++i) {
        char c = key[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

// 🎯 对齐 OpenClaw: normalizeEnvVarKey(rawKey, options?)
std::optional<std::string> normalize_env_var_key(
    const std::string& raw_key,
    bool portable
) {
    // trim
    size_t start = raw_key.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::nullopt;
    size_t end = raw_key.find_last_not_of(" \t\r\n");
    std::string key = raw_key.substr(start, end - start + 1);
    if (key.empty()) return std::nullopt;
    if (portable && !is_portable_env_var_key(key)) return std::nullopt;
    return key;
}

// 🎯 对齐 OpenClaw: listNormalizedPortableEnvEntries(source)
static std::vector<std::pair<std::string, std::string>> list_normalized_portable_env_entries(
    const std::map<std::string, std::string>& source
) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& [raw_key, value] : source) {
        auto key = normalize_env_var_key(raw_key, /*portable=*/true);
        if (!key) continue;
        entries.emplace_back(*key, value);
    }
    return entries;
}

// ============================================================================
// Public API
// ============================================================================

// 🎯 对齐 OpenClaw: isDangerousHostEnvVarName(rawKey)
bool is_dangerous_host_env_var_name(const std::string& raw_key) {
    auto key = normalize_env_var_key(raw_key);
    if (!key) return false;
    std::string upper = *key;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    // Check exact match
    for (const auto& blocked : HOST_DANGEROUS_ENV_KEY_VALUES) {
        if (upper == blocked) return true;
    }
    // Check prefix match
    for (const auto& prefix : HOST_DANGEROUS_ENV_PREFIXES) {
        if (upper.size() >= prefix.size() &&
            upper.substr(0, prefix.size()) == prefix) {
            return true;
        }
    }
    return false;
}

// 🎯 对齐 OpenClaw: isDangerousHostEnvOverrideVarName(rawKey)
bool is_dangerous_host_env_override_var_name(const std::string& raw_key) {
    auto key = normalize_env_var_key(raw_key);
    if (!key) return false;
    std::string upper = *key;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    // Check exact match
    for (const auto& blocked : HOST_DANGEROUS_OVERRIDE_ENV_KEY_VALUES) {
        if (upper == blocked) return true;
    }
    // Check prefix match
    for (const auto& prefix : HOST_DANGEROUS_OVERRIDE_ENV_PREFIXES) {
        if (upper.size() >= prefix.size() &&
            upper.substr(0, prefix.size()) == prefix) {
            return true;
        }
    }
    return false;
}

// 🎯 对齐 OpenClaw: sanitizeHostExecEnv(params?)
std::map<std::string, std::string> sanitize_host_exec_env(
    const std::map<std::string, std::string>* base_env,
    const std::map<std::string, std::string>* overrides,
    bool block_path_overrides
) {
    // Build base env from process environment if not provided
    std::map<std::string, std::string> process_env_map;
    if (!base_env) {
        // Read from process environment
        extern char** environ;
        for (char** env = environ; env && *env; ++env) {
            std::string entry(*env);
            auto pos = entry.find('=');
            if (pos != std::string::npos) {
                process_env_map[entry.substr(0, pos)] = entry.substr(pos + 1);
            }
        }
        base_env = &process_env_map;
    }

    std::map<std::string, std::string> merged;

    // Step 1: Filter base env
    for (const auto& [key, value] : list_normalized_portable_env_entries(*base_env)) {
        if (is_dangerous_host_env_var_name(key)) continue;
        merged[key] = value;
    }

    if (!overrides) {
        return merged;
    }

    // Step 2: Apply overrides (filtered)
    for (const auto& [key, value] : list_normalized_portable_env_entries(*overrides)) {
        std::string upper = key;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

        // PATH is part of the security boundary (command resolution + safe-bin checks).
        // Never allow request-scoped PATH overrides from agents/gateways.
        if (block_path_overrides && upper == "PATH") {
            continue;
        }
        if (is_dangerous_host_env_var_name(upper) || is_dangerous_host_env_override_var_name(upper)) {
            continue;
        }
        merged[key] = value;
    }

    return merged;
}

// 🎯 对齐 OpenClaw: sanitizeSystemRunEnvOverrides(params?)
std::optional<std::map<std::string, std::string>> sanitize_system_run_env_overrides(
    const std::map<std::string, std::string>* overrides,
    bool shell_wrapper
) {
    if (!overrides) return std::nullopt;

    if (!shell_wrapper) {
        // Non-shell-wrapper: return overrides as-is
        return *overrides;
    }

    // Shell wrapper: only allow HOST_SHELL_WRAPPER_ALLOWED_OVERRIDE_ENV_KEY_VALUES
    std::unordered_set<std::string> allowed_set(
        HOST_SHELL_WRAPPER_ALLOWED_OVERRIDE_ENV_KEY_VALUES.begin(),
        HOST_SHELL_WRAPPER_ALLOWED_OVERRIDE_ENV_KEY_VALUES.end()
    );

    std::map<std::string, std::string> filtered;
    for (const auto& [key, value] : list_normalized_portable_env_entries(*overrides)) {
        std::string upper = key;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        if (!allowed_set.count(upper)) continue;
        filtered[key] = value;
    }

    if (filtered.empty()) return std::nullopt;
    return filtered;
}

}  // namespace security
}  // namespace hunterclaw
