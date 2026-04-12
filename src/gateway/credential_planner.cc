// ============================================================================
// credential_planner.cc
// 🎯 严格对齐 OpenClaw: src/gateway/credential-planner.ts (167 行)
//
// 网关认证凭据的优先级解析和规划引擎
// 支持 token 和 password 两种认证方式
// 每种方式有 local 和 remote 两个来源 + 环境变量
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <cstdlib>
#include <regex>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ── Types ────────────────────────────────────────────────────────────────────

// 🎯 对齐: GatewayCredentialInputPath
enum class CredentialInputPath {
    AuthToken,        // "gateway.auth.token"
    AuthPassword,     // "gateway.auth.password"
    RemoteToken,      // "gateway.remote.token"
    RemotePassword,   // "gateway.remote.password"
};

static std::string credential_path_to_string(CredentialInputPath p) {
    switch (p) {
        case CredentialInputPath::AuthToken:      return "gateway.auth.token";
        case CredentialInputPath::AuthPassword:    return "gateway.auth.password";
        case CredentialInputPath::RemoteToken:     return "gateway.remote.token";
        case CredentialInputPath::RemotePassword:  return "gateway.remote.password";
    }
    return "";
}

// 🎯 对齐: GatewayConfiguredCredentialInput
struct ConfiguredCredentialInput {
    CredentialInputPath path;
    bool configured = false;
    std::optional<std::string> value;
    bool has_secret_ref = false;
};

// 🎯 对齐: GatewayCredentialPlan — 完整的凭据计划
struct CredentialPlan {
    std::string configured_mode;  // "local" | "remote"
    std::string auth_mode;
    std::optional<std::string> env_token;
    std::optional<std::string> env_password;
    ConfiguredCredentialInput local_token;
    ConfiguredCredentialInput local_password;
    ConfiguredCredentialInput remote_token;
    ConfiguredCredentialInput remote_password;
    bool local_token_can_win = false;
    bool local_password_can_win = false;
    bool local_token_surface_active = false;
    bool token_can_win = false;
    bool password_can_win = false;
    bool remote_mode = false;
    bool remote_url_configured = false;
    bool tailscale_remote_exposure = false;
    bool remote_configured_surface = false;
    bool remote_token_fallback_active = false;
    bool remote_token_active = false;
    bool remote_password_fallback_active = false;
    bool remote_password_active = false;
};

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string trim_string(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// 🎯 对齐: normalizeOptionalString (trimToUndefined)
static std::optional<std::string> normalize_optional_string(const std::string& s) {
    auto trimmed = trim_string(s);
    return trimmed.empty() ? std::nullopt : std::optional<std::string>(trimmed);
}

// 🎯 对齐: containsEnvVarReference — detect ${VAR} patterns
static bool contains_env_var_reference(const std::string& s) {
    static const std::regex env_var_regex(R"(\$\{[A-Z_][A-Z0-9_]*\})");
    return std::regex_search(s, env_var_regex);
}

// 🎯 对齐: trimCredentialToUndefined(value)
// Rejects unresolved env var placeholders like ${HUNTERCLAW_GATEWAY_TOKEN}
static std::optional<std::string> trim_credential_to_undefined(const std::string& value) {
    auto trimmed = normalize_optional_string(value);
    if (trimmed.has_value() && contains_env_var_reference(*trimmed)) {
        return std::nullopt;
    }
    return trimmed;
}

static std::string get_env_or_empty(const char* name) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : "";
}

// 🎯 对齐: hasGatewayTokenEnvCandidate
static bool has_gateway_token_env_candidate() {
    return normalize_optional_string(
        get_env_or_empty("HUNTERCLAW_GATEWAY_TOKEN")).has_value();
}

// 🎯 对齐: hasGatewayPasswordEnvCandidate
static bool has_gateway_password_env_candidate() {
    return normalize_optional_string(
        get_env_or_empty("HUNTERCLAW_GATEWAY_PASSWORD")).has_value();
}

// 🎯 对齐: resolveConfiguredGatewayCredentialInput
static ConfiguredCredentialInput resolve_configured_credential_input(
    const json& config, CredentialInputPath path) {

    ConfiguredCredentialInput input;
    input.path = path;

    std::string config_path;
    switch (path) {
        case CredentialInputPath::AuthToken:
            config_path = "gateway.auth.token"; break;
        case CredentialInputPath::AuthPassword:
            config_path = "gateway.auth.password"; break;
        case CredentialInputPath::RemoteToken:
            config_path = "gateway.remote.token"; break;
        case CredentialInputPath::RemotePassword:
            config_path = "gateway.remote.password"; break;
    }

    // Navigate JSON path
    std::string value_str;
    try {
        if (path == CredentialInputPath::AuthToken &&
            config.contains("gateway") && config["gateway"].contains("auth") &&
            config["gateway"]["auth"].contains("token")) {
            value_str = config["gateway"]["auth"]["token"].get<std::string>();
        } else if (path == CredentialInputPath::AuthPassword &&
            config.contains("gateway") && config["gateway"].contains("auth") &&
            config["gateway"]["auth"].contains("password")) {
            value_str = config["gateway"]["auth"]["password"].get<std::string>();
        } else if (path == CredentialInputPath::RemoteToken &&
            config.contains("gateway") && config["gateway"].contains("remote") &&
            config["gateway"]["remote"].contains("token")) {
            value_str = config["gateway"]["remote"]["token"].get<std::string>();
        } else if (path == CredentialInputPath::RemotePassword &&
            config.contains("gateway") && config["gateway"].contains("remote") &&
            config["gateway"]["remote"].contains("password")) {
            value_str = config["gateway"]["remote"]["password"].get<std::string>();
        }
    } catch (...) {
        // JSON type mismatch — treat as unconfigured
    }

    auto trimmed = normalize_optional_string(value_str);
    input.configured = trimmed.has_value();
    input.value = trim_credential_to_undefined(value_str);
    input.has_secret_ref = false;  // Secret ref resolution handled at config layer

    return input;
}

// ── createGatewayCredentialPlan ─────────────────────────────────────────────
// 🎯 对齐: createGatewayCredentialPlan(params) — 核心函数

CredentialPlan create_gateway_credential_plan(const json& config) {
    CredentialPlan plan;

    // Gateway config section
    json gateway;
    if (config.contains("gateway")) gateway = config["gateway"];
    json remote;
    if (gateway.contains("remote")) remote = gateway["remote"];

    // Auth mode
    plan.auth_mode = "";
    if (gateway.contains("auth") && gateway["auth"].contains("mode")) {
        plan.auth_mode = gateway["auth"]["mode"].get<std::string>();
    }

    // Env credentials
    std::string env_token_str = get_env_or_empty("HUNTERCLAW_GATEWAY_TOKEN");
    std::string env_password_str = get_env_or_empty("HUNTERCLAW_GATEWAY_PASSWORD");
    plan.env_token = normalize_optional_string(env_token_str);
    plan.env_password = normalize_optional_string(env_password_str);

    // Resolve four credential inputs
    plan.local_token = resolve_configured_credential_input(
        config, CredentialInputPath::AuthToken);
    plan.local_password = resolve_configured_credential_input(
        config, CredentialInputPath::AuthPassword);
    plan.remote_token = resolve_configured_credential_input(
        config, CredentialInputPath::RemoteToken);
    plan.remote_password = resolve_configured_credential_input(
        config, CredentialInputPath::RemotePassword);

    // 🎯 对齐: 凭据竞争逻辑
    plan.local_token_can_win =
        plan.auth_mode != "password" &&
        plan.auth_mode != "none" &&
        plan.auth_mode != "trusted-proxy";

    plan.token_can_win =
        plan.env_token.has_value() ||
        plan.local_token.configured ||
        plan.remote_token.configured;

    plan.password_can_win =
        plan.auth_mode == "password" ||
        (plan.auth_mode != "token" &&
         plan.auth_mode != "none" &&
         plan.auth_mode != "trusted-proxy" &&
         !plan.token_can_win);

    plan.local_password_can_win = plan.password_can_win;

    plan.local_token_surface_active =
        plan.local_token_can_win &&
        !plan.env_token.has_value() &&
        (plan.auth_mode == "token" ||
         (plan.auth_mode.empty() &&
          !plan.env_password.has_value() &&
          !plan.local_password.configured));

    // Remote mode
    plan.remote_mode = false;
    if (gateway.contains("mode")) {
        plan.remote_mode = gateway["mode"].get<std::string>() == "remote";
    }
    plan.configured_mode = plan.remote_mode ? "remote" : "local";

    // Remote URL
    std::string remote_url;
    if (remote.contains("url") && remote["url"].is_string()) {
        remote_url = trim_string(remote["url"].get<std::string>());
    }
    plan.remote_url_configured = !remote_url.empty();

    // Tailscale exposure
    plan.tailscale_remote_exposure = false;
    if (gateway.contains("tailscale") && gateway["tailscale"].contains("mode")) {
        std::string ts_mode = gateway["tailscale"]["mode"].get<std::string>();
        plan.tailscale_remote_exposure = (ts_mode == "serve" || ts_mode == "funnel");
    }

    plan.remote_configured_surface =
        plan.remote_mode || plan.remote_url_configured || plan.tailscale_remote_exposure;

    plan.remote_token_fallback_active =
        plan.local_token_can_win &&
        !plan.env_token.has_value() &&
        !plan.local_token.configured;

    plan.remote_password_fallback_active =
        !plan.env_password.has_value() &&
        !plan.local_password.configured &&
        plan.password_can_win;

    plan.remote_token_active =
        plan.remote_configured_surface || plan.remote_token_fallback_active;
    plan.remote_password_active =
        plan.remote_configured_surface || plan.remote_password_fallback_active;

    return plan;
}

// ── JSON serialization ──────────────────────────────────────────────────────

json credential_plan_to_json(const CredentialPlan& plan) {
    json j;
    j["configuredMode"] = plan.configured_mode;
    j["authMode"] = plan.auth_mode;
    if (plan.env_token.has_value()) j["envToken"] = "[redacted]";
    if (plan.env_password.has_value()) j["envPassword"] = "[redacted]";
    j["localTokenCanWin"] = plan.local_token_can_win;
    j["localPasswordCanWin"] = plan.local_password_can_win;
    j["localTokenSurfaceActive"] = plan.local_token_surface_active;
    j["tokenCanWin"] = plan.token_can_win;
    j["passwordCanWin"] = plan.password_can_win;
    j["remoteMode"] = plan.remote_mode;
    j["remoteUrlConfigured"] = plan.remote_url_configured;
    j["tailscaleRemoteExposure"] = plan.tailscale_remote_exposure;
    j["remoteConfiguredSurface"] = plan.remote_configured_surface;
    j["remoteTokenActive"] = plan.remote_token_active;
    j["remotePasswordActive"] = plan.remote_password_active;
    return j;
}

}  // namespace gateway
}  // namespace hunterclaw
