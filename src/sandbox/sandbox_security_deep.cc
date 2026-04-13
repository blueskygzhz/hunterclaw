// 🎯 Strict alignment: OpenClaw agents/sandbox/validate-sandbox-security.ts (405) 深层
//   + host-paths.ts (44) + network-mode.ts (31) + bind-spec.ts (35)
//   + sanitize-env-vars.ts (111) 深层扩展
// Deep security validation: symlink escape hardening, home subpath blocking, env value validation.

#include "hunterclaw/sandbox/validate_sandbox_security.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <regex>
#include <set>

namespace hunterclaw {
namespace sandbox {
namespace security {

namespace fs = std::filesystem;

// ─── home subpath blocking (对齐 validate-sandbox-security.ts:39-48) ────

static const std::vector<std::string> BLOCKED_HOME_SUBPATHS = {
    ".aws", ".cargo", ".config", ".docker", ".gnupg",
    ".netrc", ".npm", ".ssh"
};

// 🎯 getBlockedHomeRoots (对齐 validate-sandbox-security.ts:158-174)
static std::vector<std::string> get_blocked_home_roots() {
    std::set<std::string> roots;
    const char* home = std::getenv("HOME");
    if (home) {
        std::string h = normalize_host_path(home);
        if (h != "/") {
            roots.insert(h);
            try {
                auto canonical = fs::canonical(h).string();
                if (canonical != "/") roots.insert(normalize_host_path(canonical));
            } catch (...) {}
        }
    }
    return {roots.begin(), roots.end()};
}

// 🎯 getBlockedHostPaths (对齐 validate-sandbox-security.ts:148-156) — 含 home subpaths
static std::vector<std::string> get_full_blocked_host_paths() {
    std::set<std::string> blocked;
    for (const auto& p : blocked_host_paths()) blocked.insert(normalize_host_path(p));
    for (const auto& home : get_blocked_home_roots()) {
        for (const auto& suffix : BLOCKED_HOME_SUBPATHS) {
            blocked.insert(normalize_host_path(home + "/" + suffix));
        }
    }
    return {blocked.begin(), blocked.end()};
}

// ─── network-mode.ts (对齐 network-mode.ts:1-31) ────────────────────────

enum class NetworkModeBlockReason { None, Host, ContainerNamespaceJoin };

// 🎯 normalizeNetworkMode (对齐 network-mode.ts:5-8)
static std::string normalize_network_mode(const std::string& network) {
    std::string r;
    for (char c : network) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto s = r.find_first_not_of(" \t");
    if (s == std::string::npos) return "";
    return r.substr(s, r.find_last_not_of(" \t") - s + 1);
}

// 🎯 getBlockedNetworkModeReason (对齐 network-mode.ts:10-25)
static NetworkModeBlockReason get_blocked_network_reason(
    const std::string& network, bool allow_container_join = false)
{
    auto n = normalize_network_mode(network);
    if (n.empty()) return NetworkModeBlockReason::None;
    if (n == "host") return NetworkModeBlockReason::Host;
    if (n.substr(0, 10) == "container:" && !allow_container_join)
        return NetworkModeBlockReason::ContainerNamespaceJoin;
    return NetworkModeBlockReason::None;
}

// 🎯 isDangerousNetworkMode (对齐 network-mode.ts:27-30)
bool is_dangerous_network_mode(const std::string& network) {
    auto n = normalize_network_mode(network);
    return n == "host" || n.substr(0, 10) == "container:";
}

// ─── Symlink escape hardening (对齐 validate-sandbox-security.ts:332-341)

// 🎯 resolveSandboxHostPathViaExistingAncestor (对齐 host-paths.ts:38-43)
static std::string resolve_via_existing_ancestor(const std::string& source_path) {
    if (source_path.empty() || source_path[0] != '/') return source_path;
    try {
        // Walk up to find existing ancestor
        fs::path p(source_path);
        fs::path suffix;
        while (!p.empty() && p != p.root_path()) {
            if (fs::exists(p)) {
                auto canonical = fs::canonical(p);
                return normalize_host_path((canonical / suffix).string());
            }
            suffix = p.filename() / suffix;
            p = p.parent_path();
        }
        return normalize_host_path(source_path);
    } catch (...) {
        return normalize_host_path(source_path);
    }
}

// ─── Deep bind mount validation (对齐 validate-sandbox-security.ts:239-341)

// 🎯 enforceSourcePathPolicy (对齐 validate-sandbox-security.ts:239-257)
static void enforce_source_path_policy(
    const std::string& bind, const std::string& source,
    const std::vector<std::string>& allowed_roots,
    const std::vector<std::string>& blocked_paths,
    bool allow_outside_roots)
{
    // Check against blocked paths
    auto reason = get_blocked_reason_for_source_path(source);
    if (reason) {
        throw std::runtime_error("Sandbox security: bind mount \"" + bind +
            "\" blocked — source targets protected path \"" + reason->blocked_path + "\"");
    }
    
    // Check against full blocked list (including home subpaths)
    auto full_blocked = get_full_blocked_host_paths();
    for (const auto& bp : full_blocked) {
        if (source == bp || (source.size() > bp.size() && source.substr(0, bp.size() + 1) == bp + "/")) {
            throw std::runtime_error("Sandbox security: bind mount \"" + bind +
                "\" targets blocked path \"" + bp + "\"");
        }
    }
    
    // Check allowed roots
    if (!allow_outside_roots && !allowed_roots.empty()) {
        bool inside = false;
        for (const auto& root : allowed_roots) {
            if (source == root || (source.size() > root.size() && source.substr(0, root.size() + 1) == root + "/")) {
                inside = true; break;
            }
        }
        if (!inside) {
            std::string roots_str;
            for (size_t i = 0; i < allowed_roots.size(); ++i) {
                if (i > 0) roots_str += ", ";
                roots_str += allowed_roots[i];
            }
            throw std::runtime_error("Sandbox security: bind mount \"" + bind +
                "\" source \"" + source + "\" outside allowed roots (" + roots_str + ")");
        }
    }
}

// 🎯 validateBindMounts DEEP (对齐 validate-sandbox-security.ts:292-341) — 含 symlink 加固
void validate_bind_mounts_deep(
    const std::vector<std::string>& binds,
    const std::vector<std::string>& allowed_source_roots,
    bool allow_sources_outside,
    bool allow_reserved_targets)
{
    if (binds.empty()) return;
    
    // Normalize allowed roots (对齐 normalizeAllowedRoots)
    std::vector<std::string> normalized_roots;
    for (const auto& r : allowed_source_roots) {
        auto nr = normalize_host_path(r);
        if (nr[0] == '/') {
            normalized_roots.push_back(nr);
            auto real = resolve_via_existing_ancestor(nr);
            if (real != nr) normalized_roots.push_back(real);
        }
    }
    
    auto full_blocked = get_full_blocked_host_paths();
    static const std::vector<std::string> RESERVED = {"/workspace", "/agent"};
    
    for (const auto& raw_bind : binds) {
        auto bind = raw_bind;
        auto s = bind.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        bind = bind.substr(s);
        auto e = bind.find_last_not_of(" \t");
        bind = bind.substr(0, e + 1);
        if (bind.empty()) continue;
        
        // Fast string-only check
        auto blocked = get_blocked_bind_reason(bind);
        if (blocked) {
            throw std::runtime_error("Sandbox security: bind mount \"" + bind + "\" blocked");
        }
        
        // Reserved target check (对齐 validate-sandbox-security.ts:315-320)
        if (!allow_reserved_targets) {
            auto target = normalize_host_path(parse_bind_target_path(bind));
            for (const auto& reserved : RESERVED) {
                if (target == reserved || (target.size() > reserved.size() &&
                    target.substr(0, reserved.size() + 1) == reserved + "/")) {
                    throw std::runtime_error("Sandbox security: bind mount \"" + bind +
                        "\" targets reserved container path \"" + reserved + "\"");
                }
            }
        }
        
        // Source path policy — string check
        auto source = normalize_host_path(parse_bind_source_path(bind));
        enforce_source_path_policy(bind, source, normalized_roots, full_blocked, allow_sources_outside);
        
        // Symlink escape hardening (对齐 validate-sandbox-security.ts:332-340)
        auto canonical = resolve_via_existing_ancestor(source);
        enforce_source_path_policy(bind, canonical, normalized_roots, full_blocked, allow_sources_outside);
    }
}

// ─── Deep env var validation (扩展 sanitize-env-vars.ts:45-56) ──────────

// 🎯 validateEnvVarValue DEEP (对齐 sanitize-env-vars.ts:45-56)
std::optional<std::string> validate_env_var_value_deep(const std::string& value) {
    if (value.find('\0') != std::string::npos) return "Contains null bytes";
    if (value.size() > 32768) return "Value exceeds maximum length";
    // Base64 credential detection
    static const std::regex base64_re("^[A-Za-z0-9+/=]{80,}$");
    if (std::regex_match(value, base64_re))
        return "Value looks like base64-encoded credential data";
    return std::nullopt;
}

// 🎯 getBlockedPatterns / getAllowedPatterns (对齐 sanitize-env-vars.ts:104-110)
std::vector<std::string> get_blocked_env_patterns() {
    return {
        "^ANTHROPIC_API_KEY$", "^OPENAI_API_KEY$", "^GEMINI_API_KEY$",
        "^OPENROUTER_API_KEY$", "^MINIMAX_API_KEY$", "^ELEVENLABS_API_KEY$",
        "^TELEGRAM_BOT_TOKEN$", "^DISCORD_BOT_TOKEN$",
        "^SLACK_(BOT|APP)_TOKEN$", "^LINE_CHANNEL_SECRET$",
        "^HUNTERCLAW_GATEWAY_(TOKEN|PASSWORD)$",
        "^AWS_(SECRET_ACCESS_KEY|SECRET_KEY|SESSION_TOKEN)$",
        "^(GH|GITHUB)_TOKEN$",
        "_?(API_KEY|TOKEN|PASSWORD|PRIVATE_KEY|SECRET)$"
    };
}

std::vector<std::string> get_allowed_env_patterns() {
    return {"^LANG$", "^LC_.*$", "^PATH$", "^HOME$", "^USER$",
            "^SHELL$", "^TERM$", "^TZ$", "^NODE_ENV$"};
}

}  // namespace security
}  // namespace sandbox
}  // namespace hunterclaw
