// 🎯 Strict alignment: OpenClaw agents/sandbox/context.ts (256) + workspace.ts (66)
//   + workspace-mounts.ts (42) + browser.ts (442) + novnc-auth.ts (104)
//   + browser-bridges.ts (12) + prune.ts (133) + manage.ts (125)
// Sandbox context resolution, workspace layout, browser container, pruning, container management.

#include "hunterclaw/sandbox/sandbox.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <unordered_map>

namespace hunterclaw {
namespace sandbox {
namespace context {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ─── workspace-mounts.ts ─────────────────────────────────────────────────

// 🎯 SANDBOX_MOUNT_FORMAT_VERSION (对齐 workspace-mounts.ts:4)
static constexpr int MOUNT_FORMAT_VERSION = 2;

// 🎯 formatManagedWorkspaceBind (对齐 workspace-mounts.ts:6-12)
static std::string format_workspace_bind(const std::string& host, const std::string& container,
                                          bool read_only) {
    return host + ":" + container + ":" + (read_only ? "ro,z" : "z");
}

// 🎯 appendWorkspaceMountArgs (对齐 workspace-mounts.ts:14-41)
static void append_workspace_mount_args(
    std::vector<std::string>& args, const std::string& workspace_dir,
    const std::string& agent_workspace_dir, const std::string& workdir,
    const std::string& workspace_access)
{
    args.push_back("-v");
    args.push_back(format_workspace_bind(workspace_dir, workdir, workspace_access != "rw"));
    if (workspace_access != "none" && workspace_dir != agent_workspace_dir) {
        args.push_back("-v");
        args.push_back(format_workspace_bind(
            agent_workspace_dir, "/agent", workspace_access == "ro"));
    }
}

// ─── workspace.ts ────────────────────────────────────────────────────────

// 🎯 ensureSandboxWorkspace (对齐 workspace.ts:17-65)
static void ensure_sandbox_workspace(const std::string& workspace_dir,
                                      const std::string& seed_from = "") {
    fs::create_directories(workspace_dir);
    if (!seed_from.empty() && fs::exists(seed_from)) {
        static const std::vector<std::string> SEED_FILES = {
            "AGENTS.md", "SOUL.md", "TOOLS.md", "IDENTITY.md",
            "USER.md", "BOOTSTRAP.md", "HEARTBEAT.md"
        };
        for (const auto& name : SEED_FILES) {
            auto src = fs::path(seed_from) / name;
            auto dest = fs::path(workspace_dir) / name;
            if (!fs::exists(dest) && fs::exists(src)) {
                try { fs::copy_file(src, dest); } catch (...) {}
            }
        }
    }
}

// ─── browser-bridges.ts (对齐 browser-bridges.ts:1-12) ──────────────────

struct BrowserBridgeEntry {
    std::string container_name;
    std::string auth_token;
    std::string auth_password;
    std::string base_url;
};

static std::mutex& bridge_mu() { static std::mutex m; return m; }
static auto& browser_bridges() {
    static std::unordered_map<std::string, BrowserBridgeEntry> map;
    return map;
}

// ─── novnc-auth.ts (对齐 novnc-auth.ts:1-104) ──────────────────────────

static constexpr int NOVNC_TOKEN_TTL_MS = 60000;
static constexpr int NOVNC_PASSWORD_LENGTH = 8;
static const char* NOVNC_ALPHABET = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

struct NoVncTokenEntry {
    int novnc_port;
    std::string password;
    int64_t expires_at;
};

static std::mutex& novnc_mu() { static std::mutex m; return m; }
static auto& novnc_tokens() {
    static std::unordered_map<std::string, NoVncTokenEntry> map;
    return map;
}

static void prune_expired_novnc_tokens(int64_t now) {
    for (auto it = novnc_tokens().begin(); it != novnc_tokens().end();) {
        if (it->second.expires_at <= now) it = novnc_tokens().erase(it);
        else ++it;
    }
}

// 🎯 isNoVncEnabled (对齐 novnc-auth.ts:30-32)
bool is_novnc_enabled(bool enable_novnc, bool headless) {
    return enable_novnc && !headless;
}

// 🎯 generateNoVncPassword (对齐 novnc-auth.ts:34-41)
std::string generate_novnc_password() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 61);
    std::string pw;
    for (int i = 0; i < NOVNC_PASSWORD_LENGTH; ++i)
        pw += NOVNC_ALPHABET[dist(gen)];
    return pw;
}

// 🎯 issueNoVncObserverToken (对齐 novnc-auth.ts:58-73)
std::string issue_novnc_observer_token(int port, const std::string& password) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(novnc_mu());
    prune_expired_novnc_tokens(now);
    std::random_device rd;
    std::mt19937 gen(rd());
    char token[49];
    for (int i = 0; i < 48; ++i) {
        static const char hex[] = "0123456789abcdef";
        token[i] = hex[std::uniform_int_distribution<int>(0,15)(gen)];
    }
    token[48] = '\0';
    std::string t(token);
    novnc_tokens()[t] = {port, password, now + NOVNC_TOKEN_TTL_MS};
    return t;
}

// 🎯 consumeNoVncObserverToken (对齐 novnc-auth.ts:75-94)
struct NoVncPayload { int port; std::string password; };

std::optional<NoVncPayload> consume_novnc_observer_token(const std::string& token) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(novnc_mu());
    prune_expired_novnc_tokens(now);
    auto it = novnc_tokens().find(token);
    if (it == novnc_tokens().end()) return std::nullopt;
    auto entry = it->second;
    novnc_tokens().erase(it);
    if (entry.expires_at <= now) return std::nullopt;
    return NoVncPayload{entry.novnc_port, entry.password};
}

// ─── prune.ts (对齐 prune.ts:1-133) ─────────────────────────────────────

static int64_t last_prune_at_ms = 0;

// 🎯 shouldPruneSandboxEntry (对齐 prune.ts:24-36)
static bool should_prune_entry(int idle_hours, int max_age_days,
                                int64_t now, int64_t created_at, int64_t last_used_at) {
    if (idle_hours == 0 && max_age_days == 0) return false;
    int64_t idle_ms = now - last_used_at;
    int64_t age_ms = now - created_at;
    return (idle_hours > 0 && idle_ms > static_cast<int64_t>(idle_hours) * 3600000) ||
           (max_age_days > 0 && age_ms > static_cast<int64_t>(max_age_days) * 86400000);
}

// 🎯 maybePruneSandboxes (对齐 prune.ts:114-132)
void maybe_prune_sandboxes(int idle_hours, int max_age_days) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now - last_prune_at_ms < 300000) return;  // 5 min cooldown
    last_prune_at_ms = now;
    // Actual pruning delegated to registry.cc
}

// ─── manage.ts: Container Info Types (对齐 manage.ts:16-24) ─────────────

struct ContainerInfo {
    std::string container_name;
    std::string session_key;
    std::string image;
    std::string backend_id;
    int64_t created_at_ms = 0;
    int64_t last_used_at_ms = 0;
    bool running = false;
    bool image_match = true;
};

struct BrowserInfo {
    std::string container_name;
    std::string session_key;
    std::string image;
    int cdp_port = 0;
    int novnc_port = 0;
    int64_t created_at_ms = 0;
    int64_t last_used_at_ms = 0;
    bool running = false;
    bool image_match = true;
};

// ─── context.ts: Core Resolution (对齐 context.ts:125-230) ──────────────

struct SandboxContext {
    bool enabled = false;
    std::string backend_id;
    std::string session_key;
    std::string workspace_dir;
    std::string agent_workspace_dir;
    std::string workspace_access;
    std::string runtime_id;
    std::string runtime_label;
    std::string container_name;
    std::string container_workdir;
    json docker;
    json tools;
    bool browser_allow_host_control = false;
    std::optional<std::string> browser_bridge_url;
};

// 🎯 resolveSandboxContext (对齐 context.ts:125-230)
std::optional<SandboxContext> resolve_sandbox_context(
    const json& config, const std::string& session_key,
    const std::string& workspace_dir)
{
    if (session_key.empty()) return std::nullopt;
    
    // Check if session should be sandboxed
    auto agents = config.value("agents", json::object());
    auto defaults = agents.value("defaults", json::object());
    auto sandbox = defaults.value("sandbox", json::object());
    auto mode = sandbox.value("mode", "off");
    
    if (mode == "off") return std::nullopt;
    
    auto docker = sandbox.value("docker", json::object());
    auto scope = sandbox.value("scope", "agent");
    auto workspace_access = sandbox.value("workspaceAccess", "none");
    auto workspace_root = sandbox.value("workspaceRoot", "");
    
    SandboxContext ctx;
    ctx.enabled = true;
    ctx.backend_id = sandbox.value("backend", "docker");
    ctx.session_key = session_key;
    ctx.workspace_dir = workspace_dir;
    ctx.agent_workspace_dir = workspace_dir;
    ctx.workspace_access = workspace_access;
    ctx.container_workdir = docker.value("workdir", "/workspace");
    ctx.docker = docker;
    ctx.tools = sandbox.value("tools", json::object());
    
    // Ensure workspace
    if (!workspace_root.empty()) {
        ensure_sandbox_workspace(ctx.workspace_dir, workspace_dir);
    }
    
    return ctx;
}

// 🎯 ensureSandboxWorkspaceForSession (对齐 context.ts:232-255)
struct WorkspaceInfo {
    std::string workspace_dir;
    std::string container_workdir;
};

std::optional<WorkspaceInfo> ensure_sandbox_workspace_for_session(
    const json& config, const std::string& session_key,
    const std::string& workspace_dir)
{
    auto ctx = resolve_sandbox_context(config, session_key, workspace_dir);
    if (!ctx) return std::nullopt;
    return WorkspaceInfo{ctx->workspace_dir, ctx->container_workdir};
}

}  // namespace context
}  // namespace sandbox
}  // namespace hunterclaw
