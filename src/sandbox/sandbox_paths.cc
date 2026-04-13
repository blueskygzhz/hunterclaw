// 🎯 Strict alignment: OpenClaw agents/sandbox-paths.ts (222)
//   + sandbox-media-paths.ts (81) + sandbox-tool-policy.ts (39)
//   + agents/sandbox/runtime-status.ts (188) + agents/sandbox/shared.ts (47)
// Sandbox path resolution, media path mapping, tool policy, runtime status detection.

#include "hunterclaw/sandbox/sandbox.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <openssl/sha.h>
#include <optional>
#include <regex>
#include <set>
#include <sstream>

namespace hunterclaw {
namespace sandbox {
namespace paths {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ─── Constants (对齐 sandbox-paths.ts:9-12) ──────────────────────────────

static const std::string SANDBOX_CONTAINER_WORKDIR = "/workspace";
static const std::regex UNICODE_SPACES_RE("[\\u00A0\\u2000-\\u200A\\u202F\\u205F\\u3000]");
static const std::regex HTTP_URL_RE("^https?://", std::regex_constants::icase);
static const std::regex DATA_URL_RE("^data:", std::regex_constants::icase);

// ─── sandbox-paths.ts path utilities ─────────────────────────────────────

// 🎯 normalizeUnicodeSpaces (对齐 sandbox-paths.ts:14-16)
static std::string normalize_unicode_spaces(const std::string& str) {
    return std::regex_replace(str, UNICODE_SPACES_RE, " ");
}

// 🎯 normalizeAtPrefix (对齐 sandbox-paths.ts:18-20)
static std::string normalize_at_prefix(const std::string& fp) {
    return (!fp.empty() && fp[0] == '@') ? fp.substr(1) : fp;
}

// 🎯 expandPath (对齐 sandbox-paths.ts:22-31)
static std::string expand_path(const std::string& fp) {
    auto normalized = normalize_unicode_spaces(normalize_at_prefix(fp));
    if (normalized == "~") {
        const char* home = std::getenv("HOME");
        return home ? home : "/root";
    }
    if (normalized.size() >= 2 && normalized.substr(0, 2) == "~/") {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "/root") + normalized.substr(1);
    }
    return normalized;
}

// 🎯 resolveSandboxInputPath (对齐 sandbox-paths.ts:41-43)
std::string resolve_sandbox_input_path(const std::string& fp, const std::string& cwd) {
    auto expanded = expand_path(fp);
    if (!expanded.empty() && expanded[0] == '/') return expanded;
    return (fs::path(cwd) / expanded).lexically_normal().string();
}

// 🎯 resolveSandboxPath (对齐 sandbox-paths.ts:45-59)
struct SandboxPathResult {
    std::string resolved;
    std::string relative;
};

SandboxPathResult resolve_sandbox_path(const std::string& fp, const std::string& cwd,
                                        const std::string& root) {
    auto resolved = resolve_sandbox_input_path(fp, cwd);
    auto root_resolved = fs::path(root).lexically_normal().string();
    auto relative = fs::path(resolved).lexically_relative(root_resolved).string();
    if (relative.empty()) return {resolved, ""};
    if (relative.substr(0, 2) == ".." || fs::path(relative).is_absolute()) {
        throw std::runtime_error("Path escapes sandbox root (" + root_resolved + "): " + fp);
    }
    return {resolved, relative};
}

// 🎯 assertMediaNotDataUrl (对齐 sandbox-paths.ts:82-87)
void assert_media_not_data_url(const std::string& media) {
    auto raw = media;
    while (!raw.empty() && raw.front() == ' ') raw.erase(raw.begin());
    if (std::regex_search(raw, DATA_URL_RE)) {
        throw std::runtime_error("data: URLs are not supported for media. Use buffer instead.");
    }
}

// 🎯 mapContainerWorkspacePath (对齐 sandbox-paths.ts:169-186)
static std::optional<std::string> map_container_workspace_path(
    const std::string& candidate, const std::string& sandbox_root)
{
    std::string normalized = candidate;
    for (auto& c : normalized) if (c == '\\') c = '/';
    if (normalized == SANDBOX_CONTAINER_WORKDIR) return fs::path(sandbox_root).lexically_normal().string();
    auto prefix = SANDBOX_CONTAINER_WORKDIR + "/";
    if (normalized.substr(0, prefix.size()) != prefix) return std::nullopt;
    auto rel = normalized.substr(prefix.size());
    if (rel.empty()) return fs::path(sandbox_root).lexically_normal().string();
    return (fs::path(sandbox_root) / rel).lexically_normal().string();
}

// 🎯 resolveSandboxedMediaSource (对齐 sandbox-paths.ts:89-139)
std::string resolve_sandboxed_media_source(const std::string& media, const std::string& sandbox_root) {
    auto raw = media;
    while (!raw.empty() && raw.front() == ' ') raw.erase(raw.begin());
    while (!raw.empty() && raw.back() == ' ') raw.pop_back();
    if (raw.empty()) return raw;
    if (std::regex_search(raw, HTTP_URL_RE)) return raw;
    
    std::string candidate = raw;
    // Handle file:// URLs
    if (candidate.size() >= 7 && candidate.substr(0, 7) == "file://") {
        auto mapped = map_container_workspace_path(candidate.substr(7), sandbox_root);
        if (mapped) candidate = *mapped;
        else candidate = candidate.substr(7);
    }
    
    // Map container workspace path
    auto mapped = map_container_workspace_path(candidate, sandbox_root);
    if (mapped) candidate = *mapped;
    
    // Assert sandbox boundary
    auto result = resolve_sandbox_path(candidate, sandbox_root, sandbox_root);
    return result.resolved;
}

// ─── shared.ts (对齐 agents/sandbox/shared.ts:1-47) ─────────────────────

// 🎯 hashTextSha256 (对齐 hash.ts:3-5)
static std::string hash_sha256(const std::string& value) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.c_str()), value.size(), hash);
    char hex[65];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) snprintf(hex + i*2, 3, "%02x", hash[i]);
    hex[64] = '\0';
    return hex;
}

// 🎯 slugifySessionKey (对齐 shared.ts:8-16)
std::string slugify_session_key(const std::string& value) {
    auto trimmed = value.empty() ? std::string("session") : value;
    auto hash = hash_sha256(trimmed).substr(0, 8);
    std::string safe;
    for (char c : trimmed) {
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::isalnum(lc) || lc == '.' || lc == '_' || lc == '-') safe += lc;
        else safe += '-';
    }
    while (!safe.empty() && safe.front() == '-') safe.erase(safe.begin());
    while (!safe.empty() && safe.back() == '-') safe.pop_back();
    if (safe.size() > 32) safe = safe.substr(0, 32);
    if (safe.empty()) safe = "session";
    return safe + "-" + hash;
}

// 🎯 resolveSandboxScopeKey (对齐 shared.ts:24-34)
std::string resolve_sandbox_scope_key(const std::string& scope, const std::string& session_key) {
    auto trimmed = session_key.empty() ? std::string("main") : session_key;
    if (scope == "shared") return "shared";
    if (scope == "session") return trimmed;
    // scope == "agent"
    auto parts = trimmed;
    auto colon = parts.find(':');
    if (colon != std::string::npos) {
        auto second = parts.find(':', colon + 1);
        return "agent:" + parts.substr(colon + 1, second == std::string::npos ? std::string::npos : second - colon - 1);
    }
    return "agent:" + trimmed;
}

// 🎯 resolveSandboxAgentId (对齐 shared.ts:36-46)
std::optional<std::string> resolve_sandbox_agent_id(const std::string& scope_key) {
    auto trimmed = scope_key;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
    if (trimmed.empty() || trimmed == "shared") return std::nullopt;
    if (trimmed.substr(0, 6) == "agent:" && trimmed.size() > 6) {
        return trimmed.substr(6);
    }
    auto colon = trimmed.find(':');
    if (colon != std::string::npos && trimmed.size() > colon + 1) {
        auto second = trimmed.find(':', colon + 1);
        return trimmed.substr(colon + 1, second == std::string::npos ? std::string::npos : second - colon - 1);
    }
    return trimmed;
}

// ─── runtime-status.ts (对齐 agents/sandbox/runtime-status.ts:16-85) ────

// 🎯 shouldSandboxSession (对齐 runtime-status.ts:16-24)
static bool should_sandbox_session(const std::string& mode, const std::string& session_key,
                                    const std::string& main_session_key) {
    if (mode == "off") return false;
    if (mode == "all") return true;
    // mode == "non-main"
    auto sk = session_key; auto mk = main_session_key;
    while (!sk.empty() && sk.front() == ' ') sk.erase(sk.begin());
    while (!sk.empty() && sk.back() == ' ') sk.pop_back();
    while (!mk.empty() && mk.front() == ' ') mk.erase(mk.begin());
    while (!mk.empty() && mk.back() == ' ') mk.pop_back();
    return sk != mk;
}

struct RuntimeStatus {
    std::string agent_id;
    std::string session_key;
    std::string main_session_key;
    std::string mode;
    bool sandboxed = false;
};

// 🎯 resolveSandboxRuntimeStatus (对齐 runtime-status.ts:51-85)
RuntimeStatus resolve_sandbox_runtime_status(const json& config, const std::string& session_key) {
    auto agents = config.value("agents", json::object());
    auto defaults = agents.value("defaults", json::object());
    auto sandbox = defaults.value("sandbox", json::object());
    auto mode = sandbox.value("mode", "off");
    
    // Derive agent_id from session_key
    std::string agent_id = "main";
    auto parts = session_key;
    if (parts.substr(0, 6) == "agent:" && parts.size() > 6) {
        auto colon = parts.find(':', 6);
        agent_id = parts.substr(6, colon == std::string::npos ? std::string::npos : colon - 6);
    }
    
    std::string main_key = "agent:" + agent_id + ":main";
    bool sandboxed = should_sandbox_session(mode, session_key, main_key);
    
    return {agent_id, session_key, main_key, mode, sandboxed};
}

// 🎯 formatSandboxToolPolicyBlockedMessage (对齐 runtime-status.ts:128-187)
std::optional<std::string> format_tool_policy_blocked_message(
    const json& config, const std::string& session_key, const std::string& tool_name)
{
    if (tool_name.empty()) return std::nullopt;
    auto runtime = resolve_sandbox_runtime_status(config, session_key);
    if (!runtime.sandboxed) return std::nullopt;
    
    // Check tool policy (simplified — full impl in tool_policy.cc)
    auto agents = config.value("agents", json::object());
    auto defaults = agents.value("defaults", json::object());
    auto sandbox = defaults.value("sandbox", json::object());
    auto tools = sandbox.value("tools", json::object());
    auto deny = tools.value("deny", std::vector<std::string>{});
    
    bool blocked = false;
    for (const auto& d : deny) {
        if (d == tool_name) { blocked = true; break; }
    }
    if (!blocked) return std::nullopt;
    
    return "Tool \"" + tool_name + "\" blocked by sandbox tool policy (mode=" + runtime.mode + ").\n"
           "Session: " + runtime.session_key + "\n"
           "Fix: agents.defaults.sandbox.mode=off (disable sandbox)";
}

}  // namespace paths
}  // namespace sandbox
}  // namespace hunterclaw
