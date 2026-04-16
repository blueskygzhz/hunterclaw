// ✅ 严格对齐 OpenClaw: src/infra/diagnostic-flags.ts (90行)
// 诊断功能标志系统实现

#include "hunterclaw/observability/diagnostic_flags.h"
#include <algorithm>
#include <sstream>
#include <cstdlib>
#include <set>

namespace hunterclaw {
namespace observability {

// ============================================================================
// parseEnvFlags — 严格对齐 OpenClaw: parseEnvFlags (L6-25)
// ============================================================================

static std::string normalize_lowercase(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    // Trim
    size_t start = lower.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = lower.find_last_not_of(" \t\r\n");
    return lower.substr(start, end - start + 1);
}

static std::vector<std::string> parse_env_flags(const char* raw) {
    if (!raw) return {};

    std::string trimmed(raw);
    std::string lowered = normalize_lowercase(trimmed);
    if (lowered.empty()) return {};

    // ✅ 对齐 OpenClaw: off values
    if (lowered == "0" || lowered == "false" || lowered == "off" || lowered == "none") {
        return {};
    }

    // ✅ 对齐 OpenClaw: wildcard values
    if (lowered == "1" || lowered == "true" || lowered == "all" || lowered == "*") {
        return {"*"};
    }

    // ✅ 对齐 OpenClaw: split by comma or whitespace
    std::vector<std::string> result;
    std::istringstream stream(trimmed);
    std::string token;
    // Split by comma first
    while (std::getline(stream, token, ',')) {
        std::istringstream inner(token);
        std::string word;
        while (inner >> word) {
            std::string normalized = normalize_lowercase(word);
            if (!normalized.empty()) {
                result.push_back(normalized);
            }
        }
    }
    return result;
}

// ============================================================================
// uniqueFlags — 严格对齐 OpenClaw: uniqueFlags (L27-38)
// ============================================================================

static std::vector<std::string> unique_flags(const std::vector<std::string>& flags) {
    std::set<std::string> seen;
    std::vector<std::string> out;
    for (const auto& flag : flags) {
        std::string normalized = normalize_lowercase(flag);
        if (normalized.empty() || seen.count(normalized)) continue;
        seen.insert(normalized);
        out.push_back(normalized);
    }
    return out;
}

// ============================================================================
// resolveDiagnosticFlags — 严格对齐 OpenClaw: resolveDiagnosticFlags (L41-48)
// ============================================================================

std::vector<std::string> resolve_diagnostic_flags(const json& cfg) {
    std::vector<std::string> config_flags;

    // ✅ 对齐 OpenClaw: cfg?.diagnostics?.flags
    if (cfg.contains("diagnostics") && cfg["diagnostics"].is_object() &&
        cfg["diagnostics"].contains("flags") && cfg["diagnostics"]["flags"].is_array()) {
        for (const auto& item : cfg["diagnostics"]["flags"]) {
            if (item.is_string()) {
                config_flags.push_back(item.get<std::string>());
            }
        }
    }

    // ✅ 对齐 OpenClaw: env[DIAGNOSTICS_ENV]
    auto env_flags = parse_env_flags(std::getenv(DIAGNOSTICS_ENV));

    // ✅ 对齐 OpenClaw: uniqueFlags([...configFlags, ...envFlags])
    std::vector<std::string> combined;
    combined.insert(combined.end(), config_flags.begin(), config_flags.end());
    combined.insert(combined.end(), env_flags.begin(), env_flags.end());

    return unique_flags(combined);
}

// ============================================================================
// matchesDiagnosticFlag — 严格对齐 OpenClaw: matchesDiagnosticFlag (L50-80)
// ============================================================================

bool matches_diagnostic_flag(
    const std::string& flag,
    const std::vector<std::string>& enabled_flags) {

    std::string target = normalize_lowercase(flag);
    if (target.empty()) return false;

    for (const auto& raw : enabled_flags) {
        std::string enabled = normalize_lowercase(raw);
        if (enabled.empty()) continue;

        // ✅ 对齐 OpenClaw: wildcard "*" or "all"
        if (enabled == "*" || enabled == "all") {
            return true;
        }

        // ✅ 对齐 OpenClaw: prefix.* pattern (e.g., "webhook.*")
        if (enabled.size() >= 3 && enabled.substr(enabled.size() - 2) == ".*") {
            std::string prefix = enabled.substr(0, enabled.size() - 2);
            if (target == prefix || (target.size() > prefix.size() &&
                target.substr(0, prefix.size() + 1) == prefix + ".")) {
                return true;
            }
        }

        // ✅ 对齐 OpenClaw: prefix* pattern (e.g., "webhook*")
        if (enabled.back() == '*' && enabled.size() >= 2) {
            std::string prefix = enabled.substr(0, enabled.size() - 1);
            if (target.substr(0, prefix.size()) == prefix) {
                return true;
            }
        }

        // ✅ 对齐 OpenClaw: exact match
        if (enabled == target) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// isDiagnosticFlagEnabled — 严格对齐 OpenClaw: isDiagnosticFlagEnabled (L82-89)
// ============================================================================

bool is_diagnostic_flag_enabled(const std::string& flag, const json& cfg) {
    auto flags = resolve_diagnostic_flags(cfg);
    return matches_diagnostic_flag(flag, flags);
}

// ============================================================================
// isDiagnosticsEnabled — 严格对齐 OpenClaw: isDiagnosticsEnabled (L191-193)
// ============================================================================

bool is_diagnostics_enabled(const json& cfg) {
    if (!cfg.contains("diagnostics") || !cfg["diagnostics"].is_object()) {
        return false;
    }
    return cfg["diagnostics"].value("enabled", false);
}

}  // namespace observability
}  // namespace hunterclaw
