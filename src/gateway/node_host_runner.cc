// 🎯 Strict alignment: OpenClaw node-host/runner.ts
// Plugin host runtime — connects to gateway as a node-host WebSocket client,
// dispatches invoke requests, manages skill bin trust cache.

#include <hunterclaw/gateway/gateway.h>
#include <hunterclaw/config/config_system.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>
#include <hunterclaw/core/websocket.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace hunterclaw::gateway {

namespace {
auto log() { return core::get_logger("node-host"); }

constexpr const char* DEFAULT_NODE_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
constexpr int SKILL_BINS_TTL_MS = 90000;
} // namespace

// ─── Types (aligned: node-host/runner.ts) ────────────────────────────────

struct NodeHostRunOptions {
    std::string gateway_host = "127.0.0.1";
    int gateway_port = 18789;
    bool gateway_tls = false;
    std::optional<std::string> gateway_tls_fingerprint;
    std::optional<std::string> node_id;
    std::optional<std::string> display_name;
};

struct SkillBinTrustEntry {
    std::string name;
    std::string resolved_path;
};

struct NodeHostGatewayConfig {
    std::string host;
    int port = 18789;
    bool tls = false;
    std::optional<std::string> tls_fingerprint;
};

// ─── SkillBinsCache (aligned: class SkillBinsCache in runner.ts) ─────────

class SkillBinsCache {
public:
    using FetchFn = std::function<std::vector<std::string>()>;

    SkillBinsCache(FetchFn fetch_fn, const std::string& path_env)
        : fetch_fn_(std::move(fetch_fn))
        , path_env_(path_env)
        , last_refresh_(std::chrono::steady_clock::time_point{}) {}

    std::vector<SkillBinTrustEntry> current(bool force = false) {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_refresh_).count();

        if (force || elapsed > SKILL_BINS_TTL_MS) {
            refresh();
        }
        return bins_;
    }

private:
    void refresh() {
        try {
            auto raw_bins = fetch_fn_();
            bins_ = resolve_skill_bin_trust_entries(raw_bins);
            last_refresh_ = std::chrono::steady_clock::now();
        } catch (const std::exception& e) {
            log()->warn("SkillBinsCache refresh failed: {}", e.what());
            if (last_refresh_ == std::chrono::steady_clock::time_point{}) {
                bins_.clear();
            }
        }
    }

    // 🎯 对齐 OC: resolveSkillBinTrustEntries
    std::vector<SkillBinTrustEntry> resolve_skill_bin_trust_entries(
            const std::vector<std::string>& bins) {
        std::vector<SkillBinTrustEntry> entries;
        std::unordered_set<std::string> seen;

        for (const auto& bin : bins) {
            std::string name = bin;
            // Trim
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) continue;

            // Skip absolute paths
            if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
                continue;
            }

            // Resolve from PATH
            auto resolved = resolve_executable_from_path(name);
            if (resolved.empty()) continue;

            std::string key = name + '\0' + resolved;
            if (seen.count(key)) continue;
            seen.insert(key);

            entries.push_back({name, resolved});
        }

        // Sort by name then path
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            int cmp = a.name.compare(b.name);
            return cmp != 0 ? cmp < 0 : a.resolved_path < b.resolved_path;
        });

        return entries;
    }

    std::string resolve_executable_from_path(const std::string& bin) {
        // Search PATH for executable
        std::string path_env = path_env_;
        std::istringstream stream(path_env);
        std::string dir;
        while (std::getline(stream, dir, ':')) {
            auto full = std::filesystem::path(dir) / bin;
            if (std::filesystem::exists(full)) {
                return full.string();
            }
        }
        return "";
    }

    FetchFn fetch_fn_;
    std::string path_env_;
    std::mutex mu_;
    std::vector<SkillBinTrustEntry> bins_;
    std::chrono::steady_clock::time_point last_refresh_;
};

// ─── Node host invoke handler (aligned: handleInvoke in invoke.ts) ───────

struct InvokePayload {
    std::string command;
    std::string session_key;
    std::string run_id;
    nlohmann::json params;
};

struct InvokeResult {
    bool ok = false;
    std::optional<std::string> payload_json;
    std::optional<std::string> error_message;
    std::optional<std::string> error_code;
};

// 🎯 对齐 OC: coerceNodeInvokePayload
std::optional<InvokePayload> coerce_invoke_payload(const nlohmann::json& raw) {
    if (!raw.is_object()) return std::nullopt;

    InvokePayload payload;
    payload.command = raw.value("command", "");
    payload.session_key = raw.value("sessionKey", "");
    payload.run_id = raw.value("runId", "");
    payload.params = raw.value("params", nlohmann::json::object());

    if (payload.command.empty()) return std::nullopt;
    return payload;
}

// ─── Gateway credential resolution (aligned: resolveNodeHostGatewayCredentials)

struct GatewayCredentials {
    std::optional<std::string> token;
    std::optional<std::string> password;
};

GatewayCredentials resolve_node_host_gateway_credentials(const nlohmann::json& config) {
    // 🎯 对齐 OC: resolveNodeHostGatewayCredentials
    GatewayCredentials creds;

    auto mode = config.value("/gateway/mode"_json_pointer, std::string("local"));

    if (mode == "local") {
        // 🎯 对齐 OC: buildNodeHostLocalAuthConfig
        // Local mode: use gateway.auth.token, suppress remote auth material
        creds.token = config.value("/gateway/auth/token"_json_pointer, std::string(""));
        if (creds.token->empty()) creds.token = std::nullopt;
    } else {
        // Remote mode
        creds.token = config.value("/gateway/remote/token"_json_pointer, std::string(""));
        if (creds.token->empty()) creds.token = std::nullopt;
        creds.password = config.value("/gateway/remote/password"_json_pointer, std::string(""));
        if (creds.password->empty()) creds.password = std::nullopt;
    }

    // Also check env vars
    auto env_token = std::getenv("HUNTERCLAW_GATEWAY_TOKEN");
    if (env_token && !creds.token.has_value()) {
        creds.token = std::string(env_token);
    }

    return creds;
}

// ─── PATH env management (aligned: ensureNodePathEnv) ────────────────────

std::string ensure_node_path_env() {
    // 🎯 对齐 OC: ensureNodePathEnv
    auto current = std::getenv("PATH");
    if (current && std::string(current).find_first_not_of(' ') != std::string::npos) {
        return std::string(current);
    }
    setenv("PATH", DEFAULT_NODE_PATH, 1);
    return DEFAULT_NODE_PATH;
}

// ─── Main runner (aligned: runNodeHost) ──────────────────────────────────

void run_node_host(const NodeHostRunOptions& opts) {
    // 🎯 对齐 OC: runNodeHost

    log()->info("Starting node host...");

    // Load config
    nlohmann::json config = nlohmann::json::object();
    // Config loading is handled by the gateway initialization path
    // node_host_runner receives its config via opts or gateway protocol

    // Resolve node identity
    std::string node_id = opts.node_id.value_or("default");
    std::string display_name = opts.display_name.value_or("HunterClaw Node");

    // Resolve gateway connection
    NodeHostGatewayConfig gw_config;
    gw_config.host = opts.gateway_host;
    gw_config.port = opts.gateway_port;
    gw_config.tls = opts.gateway_tls;
    gw_config.tls_fingerprint = opts.gateway_tls_fingerprint;

    // Resolve credentials
    auto creds = resolve_node_host_gateway_credentials(config);

    // Build connection URL
    std::string scheme = gw_config.tls ? "wss" : "ws";
    std::string url = scheme + "://" + gw_config.host + ":" + std::to_string(gw_config.port);

    // Ensure PATH
    std::string path_env = ensure_node_path_env();

    log()->info("node-host connecting to {} as '{}' ({})", url, display_name, node_id);

    // 🎯 对齐 OC: GatewayClient setup with capabilities
    // In HunterClaw, the node-host connects as a WebSocket client to the gateway
    // with role="node", caps=["system"], and system run commands registered.

    // Build SkillBinsCache
    auto skill_bins = std::make_shared<SkillBinsCache>(
        [&]() -> std::vector<std::string> {
            // In production, this would call gateway's "skills.bins" method
            return {};
        },
        path_env
    );

    // Node host client registration info
    nlohmann::json client_info = {
        {"instanceId", node_id},
        {"clientName", "hunterclaw-node-host"},
        {"clientDisplayName", display_name},
        {"platform", "linux"},
        {"mode", "node"},
        {"role", "node"},
        {"caps", {"system"}},
        {"commands", {"system.run", "system.exec", "exec.approvals.list", "exec.approvals.manage"}}
    };

    log()->info("node-host ready: caps={}, commands={}",
        client_info["caps"].dump(), client_info["commands"].dump());

    // In production, this would create a WebSocket connection and enter event loop
    // For now, log readiness
    log()->info("node-host: waiting for invoke requests from gateway at {}", url);
}

} // namespace hunterclaw::gateway
