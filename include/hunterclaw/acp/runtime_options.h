#pragma once

// ============================================================================
// runtime_options.h
// Aligned with openclaw: src/acp/control-plane/runtime-options.ts (350 lines)
//
// Runtime option validation, merge, normalize, and control signature.
// ============================================================================

#include "hunterclaw/acp/runtime_errors.h"
#include <string>
#include <map>
#include <optional>
#include <vector>
#include <algorithm>
#include <regex>
#include <cmath>
#include <sstream>

namespace hunterclaw {
namespace acp {

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr size_t MAX_RUNTIME_MODE_LENGTH = 64;
static constexpr size_t MAX_MODEL_LENGTH = 200;
static constexpr size_t MAX_PERMISSION_PROFILE_LENGTH = 80;
static constexpr size_t MAX_CWD_LENGTH = 4096;
static constexpr int MIN_TIMEOUT_SECONDS = 1;
static constexpr int MAX_TIMEOUT_SECONDS = 24 * 60 * 60;
static constexpr size_t MAX_BACKEND_OPTION_KEY_LENGTH = 64;
static constexpr size_t MAX_BACKEND_OPTION_VALUE_LENGTH = 512;
static constexpr size_t MAX_BACKEND_EXTRAS = 32;

// ── AcpSessionRuntimeOptions ─────────────────────────────────────────────────
// Aligned with openclaw: AcpSessionRuntimeOptions (sessions/types.ts)
struct AcpSessionRuntimeOptions {
    std::optional<std::string> runtime_mode;
    std::optional<std::string> model;
    std::optional<std::string> cwd;
    std::optional<std::string> permission_profile;
    std::optional<int> timeout_seconds;
    std::map<std::string, std::string> backend_extras;

    bool empty() const {
        return !runtime_mode && !model && !cwd && !permission_profile
            && !timeout_seconds && backend_extras.empty();
    }
};

// ── Helpers ──────────────────────────────────────────────────────────────────

inline std::optional<std::string> normalize_text(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::nullopt;
    size_t end = s.find_last_not_of(" \t\r\n");
    std::string trimmed = s.substr(start, end - start + 1);
    return trimmed.empty() ? std::nullopt : std::optional<std::string>(trimmed);
}

inline void validate_no_control_chars(const std::string& value, const std::string& field) {
    for (char c : value) {
        if (c < 32 || c == 127) {
            throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
                field + " must not include control characters.");
        }
    }
}

inline std::string validate_bounded_text(const std::string& value, const std::string& field, size_t max_len) {
    auto normalized = normalize_text(value);
    if (!normalized || normalized->empty()) {
        throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
            field + " must not be empty.");
    }
    if (normalized->size() > max_len) {
        throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
            field + " must be at most " + std::to_string(max_len) + " characters.");
    }
    validate_no_control_chars(*normalized, field);
    return *normalized;
}

// ── Validators ───────────────────────────────────────────────────────────────

inline std::string validate_runtime_mode(const std::string& raw) {
    return validate_bounded_text(raw, "Runtime mode", MAX_RUNTIME_MODE_LENGTH);
}

inline std::string validate_model(const std::string& raw) {
    return validate_bounded_text(raw, "Model id", MAX_MODEL_LENGTH);
}

inline std::string validate_cwd(const std::string& raw) {
    auto cwd = validate_bounded_text(raw, "Working directory", MAX_CWD_LENGTH);
    if (cwd.empty() || cwd[0] != '/') {
        throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
            "Working directory must be an absolute path. Received \"" + cwd + "\".");
    }
    return cwd;
}

inline std::string validate_permission_profile(const std::string& raw) {
    return validate_bounded_text(raw, "Permission profile", MAX_PERMISSION_PROFILE_LENGTH);
}

inline int validate_timeout_seconds(int raw) {
    int timeout = static_cast<int>(std::round(raw));
    if (timeout < MIN_TIMEOUT_SECONDS || timeout > MAX_TIMEOUT_SECONDS) {
        throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
            "Timeout must be between " + std::to_string(MIN_TIMEOUT_SECONDS) +
            " and " + std::to_string(MAX_TIMEOUT_SECONDS) + " seconds.");
    }
    return timeout;
}

inline std::string validate_config_option_key(const std::string& raw) {
    auto key = validate_bounded_text(raw, "ACP config key", MAX_BACKEND_OPTION_KEY_LENGTH);
    static const std::regex safe_re("^[a-zA-Z0-9][a-zA-Z0-9._:-]*$");
    if (!std::regex_match(key, safe_re)) {
        throw AcpRuntimeError(AcpErrorCode::ACP_INVALID_RUNTIME_OPTION,
            "ACP config key must use letters, numbers, dots, colons, underscores, or dashes.");
    }
    return key;
}

inline std::string validate_config_option_value(const std::string& raw) {
    return validate_bounded_text(raw, "ACP config value", MAX_BACKEND_OPTION_VALUE_LENGTH);
}

// ── Normalize ────────────────────────────────────────────────────────────────
// Aligned with openclaw: normalizeRuntimeOptions
inline AcpSessionRuntimeOptions normalize_runtime_options(const AcpSessionRuntimeOptions& opts) {
    AcpSessionRuntimeOptions result;
    if (opts.runtime_mode) {
        auto t = normalize_text(*opts.runtime_mode);
        if (t) result.runtime_mode = *t;
    }
    if (opts.model) {
        auto t = normalize_text(*opts.model);
        if (t) result.model = *t;
    }
    if (opts.cwd) {
        auto t = normalize_text(*opts.cwd);
        if (t) result.cwd = *t;
    }
    if (opts.permission_profile) {
        auto t = normalize_text(*opts.permission_profile);
        if (t) result.permission_profile = *t;
    }
    if (opts.timeout_seconds && *opts.timeout_seconds > 0) {
        result.timeout_seconds = *opts.timeout_seconds;
    }
    for (auto& [k, v] : opts.backend_extras) {
        auto nk = normalize_text(k);
        auto nv = normalize_text(v);
        if (nk && nv) {
            result.backend_extras[*nk] = *nv;
        }
    }
    return result;
}

// ── Merge ────────────────────────────────────────────────────────────────────
// Aligned with openclaw: mergeRuntimeOptions
inline AcpSessionRuntimeOptions merge_runtime_options(
    const AcpSessionRuntimeOptions& current,
    const AcpSessionRuntimeOptions& patch
) {
    auto c = normalize_runtime_options(current);
    auto p = normalize_runtime_options(patch);
    AcpSessionRuntimeOptions result = c;
    if (p.runtime_mode) result.runtime_mode = p.runtime_mode;
    if (p.model) result.model = p.model;
    if (p.cwd) result.cwd = p.cwd;
    if (p.permission_profile) result.permission_profile = p.permission_profile;
    if (p.timeout_seconds) result.timeout_seconds = p.timeout_seconds;
    for (auto& [k, v] : p.backend_extras) {
        result.backend_extras[k] = v;
    }
    return normalize_runtime_options(result);
}

// ── Control signature ────────────────────────────────────────────────────────
// Aligned with openclaw: buildRuntimeControlSignature
inline std::string build_runtime_control_signature(const AcpSessionRuntimeOptions& opts) {
    auto n = normalize_runtime_options(opts);
    std::ostringstream oss;
    oss << "{\"runtimeMode\":" << (n.runtime_mode ? "\"" + *n.runtime_mode + "\"" : "null");
    oss << ",\"model\":" << (n.model ? "\"" + *n.model + "\"" : "null");
    oss << ",\"permissionProfile\":" << (n.permission_profile ? "\"" + *n.permission_profile + "\"" : "null");
    oss << ",\"timeoutSeconds\":" << (n.timeout_seconds ? std::to_string(*n.timeout_seconds) : "null");
    oss << ",\"backendExtras\":[";
    bool first = true;
    for (auto& [k, v] : n.backend_extras) {
        if (!first) oss << ",";
        oss << "[\"" << k << "\",\"" << v << "\"]";
        first = false;
    }
    oss << "]}";
    return oss.str();
}

// ── Config option pairs ──────────────────────────────────────────────────────
// Aligned with openclaw: buildRuntimeConfigOptionPairs
inline std::vector<std::pair<std::string, std::string>> build_runtime_config_option_pairs(
    const AcpSessionRuntimeOptions& opts
) {
    auto n = normalize_runtime_options(opts);
    std::vector<std::pair<std::string, std::string>> pairs;
    if (n.model) pairs.emplace_back("model", *n.model);
    if (n.permission_profile) pairs.emplace_back("approval_policy", *n.permission_profile);
    if (n.timeout_seconds) pairs.emplace_back("timeout", std::to_string(*n.timeout_seconds));
    for (auto& [k, v] : n.backend_extras) {
        // Don't duplicate keys already added above
        bool dup = false;
        for (auto& [pk, _] : pairs) {
            if (pk == k) { dup = true; break; }
        }
        if (!dup) pairs.emplace_back(k, v);
    }
    return pairs;
}

// ── Options equality ─────────────────────────────────────────────────────────
// Aligned with openclaw: runtimeOptionsEqual
inline bool runtime_options_equal(
    const AcpSessionRuntimeOptions& a,
    const AcpSessionRuntimeOptions& b
) {
    return build_runtime_control_signature(a) == build_runtime_control_signature(b);
}

} // namespace acp
} // namespace hunterclaw
