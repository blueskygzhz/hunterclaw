#pragma once
// Aligned with openclaw: src/agents/sandbox/validate-sandbox-security.ts (344 lines)
// Sandbox security validation — blocks dangerous Docker configurations.

#include <string>
#include <vector>
#include <optional>
#include <stdexcept>
#include <filesystem>

namespace hunterclaw {
namespace sandbox {
namespace security {

// ============================================================================
// Blocked Host Paths (对齐 BLOCKED_HOST_PATHS)
// ============================================================================
inline const std::vector<std::string>& blocked_host_paths() {
    static const std::vector<std::string> paths = {
        "/etc", "/private/etc", "/proc", "/sys", "/dev", "/root", "/boot",
        "/run", "/var/run", "/private/var/run",
        "/var/run/docker.sock", "/private/var/run/docker.sock", "/run/docker.sock",
    };
    return paths;
}

// ============================================================================
// BlockedBindReason (对齐 BlockedBindReason union type)
// ============================================================================
enum class BlockedBindReasonKind {
    Targets,              // targets blocked path
    Covers,               // covers system root
    NonAbsolute,          // non-absolute source path
    OutsideAllowedRoots,  // outside allowed roots
    ReservedTarget        // targets reserved container path
};

struct BlockedBindReason {
    BlockedBindReasonKind kind;
    std::string blocked_path;
    std::string source_path;
    std::string target_path;
    std::string reserved_path;
    std::vector<std::string> allowed_roots;
};

// ============================================================================
// Path normalization (对齐 normalizeHostPath)
// ============================================================================
inline std::string normalize_host_path(const std::string& raw) {
    if (raw.empty()) return raw;
    try {
        auto p = std::filesystem::path(raw).lexically_normal();
        std::string result = p.string();
        // Strip trailing slash (unless root)
        if (result.size() > 1 && result.back() == '/') result.pop_back();
        return result;
    } catch (...) {
        return raw;
    }
}

// ============================================================================
// Parse bind spec (对齐 parseBindSourcePath / parseBindTargetPath)
// ============================================================================
inline std::string parse_bind_source_path(const std::string& bind) {
    auto trimmed = bind;
    auto s = trimmed.find_first_not_of(" \t");
    if (s != std::string::npos) trimmed = trimmed.substr(s);
    auto e = trimmed.find_last_not_of(" \t");
    if (e != std::string::npos) trimmed = trimmed.substr(0, e + 1);
    // Format: source:target[:mode]
    auto colon = trimmed.find(':');
    if (colon == std::string::npos) return trimmed;
    // Handle Windows drive letters (C:\path)
    if (colon == 1 && std::isalpha(trimmed[0]) && trimmed.size() > 2 && (trimmed[2] == '\\' || trimmed[2] == '/')) {
        auto next_colon = trimmed.find(':', 2);
        if (next_colon == std::string::npos) return trimmed;
        return trimmed.substr(0, next_colon);
    }
    return trimmed.substr(0, colon);
}

inline std::string parse_bind_target_path(const std::string& bind) {
    auto source = parse_bind_source_path(bind);
    auto rest = bind.substr(source.size());
    if (rest.empty() || rest[0] != ':') return "";
    rest = rest.substr(1);
    auto colon = rest.find(':');
    return colon == std::string::npos ? rest : rest.substr(0, colon);
}

// ============================================================================
// Blocked reason checks (对齐 getBlockedBindReason / getBlockedReasonForSourcePath)
// ============================================================================
inline std::optional<BlockedBindReason> get_blocked_reason_for_source_path(const std::string& source_normalized) {
    if (source_normalized == "/") {
        return BlockedBindReason{BlockedBindReasonKind::Covers, "/", source_normalized, "", "", {}};
    }
    for (const auto& blocked : blocked_host_paths()) {
        if (source_normalized == blocked || (source_normalized.find(blocked + "/") == 0)) {
            return BlockedBindReason{BlockedBindReasonKind::Targets, blocked, source_normalized, "", "", {}};
        }
    }
    return std::nullopt;
}

inline std::optional<BlockedBindReason> get_blocked_bind_reason(const std::string& bind) {
    auto source_raw = parse_bind_source_path(bind);
    // Trim
    auto s = source_raw.find_first_not_of(" \t");
    if (s != std::string::npos) source_raw = source_raw.substr(s);
    if (!source_raw.empty() && source_raw[0] != '/') {
        return BlockedBindReason{BlockedBindReasonKind::NonAbsolute, "", source_raw, "", "", {}};
    }
    auto normalized = normalize_host_path(source_raw);
    return get_blocked_reason_for_source_path(normalized);
}

// ============================================================================
// Validate functions (对齐 validate* functions)
// ============================================================================

// Aligned with openclaw: validateBindMounts
inline void validate_bind_mounts(const std::vector<std::string>& binds,
                                  const std::vector<std::string>& allowed_source_roots = {},
                                  bool allow_sources_outside_allowed_roots = false,
                                  bool allow_reserved_container_targets = false) {
    static const std::vector<std::string> RESERVED_TARGETS = {"/workspace", "/agent-workspace"};

    for (const auto& raw_bind : binds) {
        auto bind = raw_bind;
        auto s = bind.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        bind = bind.substr(s);
        auto e = bind.find_last_not_of(" \t");
        bind = bind.substr(0, e + 1);
        if (bind.empty()) continue;

        auto blocked = get_blocked_bind_reason(bind);
        if (blocked) {
            throw std::runtime_error("Sandbox security: bind mount \"" + bind +
                "\" blocked (" + std::to_string(static_cast<int>(blocked->kind)) + ")");
        }

        if (!allow_reserved_container_targets) {
            auto target = normalize_host_path(parse_bind_target_path(bind));
            for (const auto& reserved : RESERVED_TARGETS) {
                if (target == reserved || target.find(reserved + "/") == 0) {
                    throw std::runtime_error("Sandbox security: bind mount \"" + bind +
                        "\" targets reserved container path \"" + reserved + "\"");
                }
            }
        }
    }
}

// Aligned with openclaw: validateNetworkMode
inline void validate_network_mode(const std::string& network,
                                   bool allow_container_namespace_join = false) {
    if (network == "host") {
        throw std::runtime_error("Sandbox security: network mode \"host\" is blocked");
    }
    if (!allow_container_namespace_join && network.find("container:") == 0) {
        throw std::runtime_error("Sandbox security: network mode \"" + network + "\" is blocked");
    }
}

// Aligned with openclaw: validateSeccompProfile
inline void validate_seccomp_profile(const std::string& profile) {
    std::string lower = profile;
    for (auto& c : lower) c = std::tolower(c);
    auto s = lower.find_first_not_of(" \t");
    if (s != std::string::npos) lower = lower.substr(s);
    auto e = lower.find_last_not_of(" \t");
    if (e != std::string::npos) lower = lower.substr(0, e + 1);
    if (lower == "unconfined") {
        throw std::runtime_error("Sandbox security: seccomp profile \"unconfined\" is blocked");
    }
}

// Aligned with openclaw: validateApparmorProfile
inline void validate_apparmor_profile(const std::string& profile) {
    std::string lower = profile;
    for (auto& c : lower) c = std::tolower(c);
    auto s = lower.find_first_not_of(" \t");
    if (s != std::string::npos) lower = lower.substr(s);
    auto e = lower.find_last_not_of(" \t");
    if (e != std::string::npos) lower = lower.substr(0, e + 1);
    if (lower == "unconfined") {
        throw std::runtime_error("Sandbox security: apparmor profile \"unconfined\" is blocked");
    }
}

// Aligned with openclaw: validateSandboxSecurity (统一入口)
struct SandboxSecurityConfig {
    std::vector<std::string> binds;
    std::string network;
    std::string seccomp_profile;
    std::string apparmor_profile;
    std::vector<std::string> allowed_source_roots;
    bool allow_sources_outside_allowed_roots = false;
    bool allow_reserved_container_targets = false;
    bool dangerously_allow_container_namespace_join = false;
};

inline void validate_sandbox_security(const SandboxSecurityConfig& cfg) {
    validate_bind_mounts(cfg.binds, cfg.allowed_source_roots,
                         cfg.allow_sources_outside_allowed_roots,
                         cfg.allow_reserved_container_targets);
    validate_network_mode(cfg.network, cfg.dangerously_allow_container_namespace_join);
    if (!cfg.seccomp_profile.empty()) validate_seccomp_profile(cfg.seccomp_profile);
    if (!cfg.apparmor_profile.empty()) validate_apparmor_profile(cfg.apparmor_profile);
}

} // namespace security
} // namespace sandbox
} // namespace hunterclaw
