// ============================================================================
// connection_details.cc
// 🎯 严格对齐 OpenClaw: src/gateway/connection-details.ts (105 行)
//
// 网关连接 URL 多层解析优先级链：
// 1. CLI --url 参数 (最高)
// 2. 环境变量 HUNTERCLAW_GATEWAY_URL
// 3. 配置文件 gateway.remote.url
// 4. 本地回环 ws(s)://127.0.0.1:<port> (默认)
//
// 安全特性：非回环地址使用 ws:// 时抛出安全错误
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <sstream>
#include <cstdlib>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ── Types ────────────────────────────────────────────────────────────────────

// 🎯 对齐: GatewayConnectionDetails
struct GatewayConnectionDetails {
    std::string url;
    std::string url_source;
    std::optional<std::string> bind_detail;
    std::optional<std::string> remote_fallback_note;
    std::string message;
};

// 🎯 对齐: options parameter
struct ConnectionDetailOptions {
    json config;                            // OpenClawConfig equivalent
    std::optional<std::string> url;         // CLI --url override
    std::optional<std::string> config_path; // config file path
    std::optional<std::string> url_source;  // "cli" | "env"
};

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string normalize_optional_string(const std::string& s) {
    std::string trimmed = s;
    size_t start = trimmed.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = trimmed.find_last_not_of(" \t\n\r");
    return trimmed.substr(start, end - start + 1);
}

static std::string get_env_or_empty(const char* name) {
    const char* val = std::getenv(name);
    return val ? normalize_optional_string(val) : "";
}

// 🎯 对齐: isSecureWebSocketUrl(url, { allowPrivateWs })
// Returns true if: wss://, ws://127.0.0.1, ws://localhost, or allowPrivateWs
static bool is_secure_websocket_url(const std::string& url, bool allow_private_ws) {
    // wss:// is always secure
    if (url.rfind("wss://", 0) == 0) return true;

    // ws:// to loopback is safe
    if (url.rfind("ws://127.0.0.1", 0) == 0) return true;
    if (url.rfind("ws://localhost", 0) == 0) return true;
    if (url.rfind("ws://[::1]", 0) == 0) return true;

    // Private network escape hatch
    if (allow_private_ws && url.rfind("ws://", 0) == 0) return true;

    return false;
}

// ── buildGatewayConnectionDetailsWithResolvers ──────────────────────────────
// 🎯 对齐: buildGatewayConnectionDetailsWithResolvers(options, resolvers)

GatewayConnectionDetails build_gateway_connection_details(
    const ConnectionDetailOptions& options) {

    // Resolve config
    json config = options.config.is_null() ? json::object() : options.config;

    // Config path
    std::string config_path = options.config_path.value_or(
        get_env_or_empty("HUNTERCLAW_CONFIG_PATH"));
    if (config_path.empty()) config_path = "~/.config/hunterclaw/config.yaml";

    // Remote mode
    bool is_remote_mode = false;
    if (config.contains("gateway") && config["gateway"].contains("mode")) {
        is_remote_mode = config["gateway"]["mode"] == "remote";
    }

    json remote;
    if (is_remote_mode && config.contains("gateway") && config["gateway"].contains("remote")) {
        remote = config["gateway"]["remote"];
    }

    // TLS
    bool tls_enabled = false;
    if (config.contains("gateway") && config["gateway"].contains("tls") &&
        config["gateway"]["tls"].contains("enabled")) {
        tls_enabled = config["gateway"]["tls"]["enabled"].get<bool>();
    }

    // Port
    int local_port = 18789;  // default gateway port
    if (config.contains("gateway") && config["gateway"].contains("port")) {
        local_port = config["gateway"]["port"].get<int>();
    }

    // Bind mode
    std::string bind_mode = "loopback";
    if (config.contains("gateway") && config["gateway"].contains("bind")) {
        bind_mode = config["gateway"]["bind"].get<std::string>();
    }

    std::string scheme = tls_enabled ? "wss" : "ws";
    std::string local_url = scheme + "://127.0.0.1:" + std::to_string(local_port);

    // CLI override
    std::string cli_url_override;
    if (options.url.has_value()) {
        cli_url_override = normalize_optional_string(*options.url);
    }

    // Env override
    std::string env_url_override;
    if (cli_url_override.empty()) {
        env_url_override = get_env_or_empty("HUNTERCLAW_GATEWAY_URL");
    }

    std::string url_override = !cli_url_override.empty() ? cli_url_override : env_url_override;

    // Remote URL from config
    std::string remote_url;
    if (!remote.is_null() && remote.contains("url")) {
        remote_url = normalize_optional_string(remote["url"].get<std::string>());
    }

    bool remote_misconfigured = is_remote_mode && url_override.empty() && remote_url.empty();

    // URL source hint
    std::string url_source_hint;
    if (options.url_source.has_value()) {
        url_source_hint = *options.url_source;
    } else if (!cli_url_override.empty()) {
        url_source_hint = "cli";
    } else if (!env_url_override.empty()) {
        url_source_hint = "env";
    }

    // 🎯 对齐: URL 解析优先级链
    std::string url = !url_override.empty() ? url_override
                    : !remote_url.empty() ? remote_url
                    : local_url;

    // URL source description
    std::string url_source;
    if (!url_override.empty()) {
        url_source = url_source_hint == "env"
            ? "env HUNTERCLAW_GATEWAY_URL"
            : "cli --url";
    } else if (!remote_url.empty()) {
        url_source = "config gateway.remote.url";
    } else if (remote_misconfigured) {
        url_source = "missing gateway.remote.url (fallback local)";
    } else {
        url_source = "local loopback";
    }

    std::optional<std::string> bind_detail;
    if (url_override.empty() && remote_url.empty()) {
        bind_detail = "Bind: " + bind_mode;
    }

    std::optional<std::string> remote_fallback_note;
    if (remote_misconfigured) {
        remote_fallback_note =
            "Warn: gateway.mode=remote but gateway.remote.url is missing; "
            "set gateway.remote.url or switch gateway.mode=local.";
    }

    // 🎯 对齐: 安全检查 — 非回环 ws:// 地址
    bool allow_private_ws =
        get_env_or_empty("HUNTERCLAW_ALLOW_INSECURE_PRIVATE_WS") == "1";

    if (!is_secure_websocket_url(url, allow_private_ws)) {
        throw std::runtime_error(
            "SECURITY ERROR: Gateway URL \"" + url +
            "\" uses plaintext ws:// to a non-loopback address.\n"
            "Both credentials and chat data would be exposed to network interception.\n"
            "Source: " + url_source + "\n"
            "Config: " + config_path + "\n"
            "Fix: Use wss:// for remote gateway URLs.\n"
            "Safe remote access defaults:\n"
            "- keep gateway.bind=loopback and use an SSH tunnel\n"
            "- or use Tailscale Serve/Funnel for HTTPS remote access\n"
            + (allow_private_ws ? "" :
               "Break-glass (trusted private networks only): "
               "set HUNTERCLAW_ALLOW_INSECURE_PRIVATE_WS=1\n")
        );
    }

    // Build message
    std::ostringstream msg;
    msg << "Gateway target: " << url << "\n"
        << "Source: " << url_source << "\n"
        << "Config: " << config_path;
    if (bind_detail.has_value()) {
        msg << "\n" << *bind_detail;
    }
    if (remote_fallback_note.has_value()) {
        msg << "\n" << *remote_fallback_note;
    }

    GatewayConnectionDetails result;
    result.url = url;
    result.url_source = url_source;
    result.bind_detail = bind_detail;
    result.remote_fallback_note = remote_fallback_note;
    result.message = msg.str();

    return result;
}

// ── JSON serialization ──────────────────────────────────────────────────────

json connection_details_to_json(const GatewayConnectionDetails& details) {
    json j;
    j["url"] = details.url;
    j["urlSource"] = details.url_source;
    if (details.bind_detail.has_value())
        j["bindDetail"] = *details.bind_detail;
    if (details.remote_fallback_note.has_value())
        j["remoteFallbackNote"] = *details.remote_fallback_note;
    j["message"] = details.message;
    return j;
}

}  // namespace gateway
}  // namespace hunterclaw
