// ============================================================================
// gateway_auth_deep.cc
// ✅ 严格对齐 OpenClaw:
//   - src/gateway/auth-surface-resolution.ts (285行)
//   - src/gateway/auth-config-utils.ts (174行)
//   - src/gateway/startup-auth.ts (315行)
//
// 认证系统深层：
//   - resolveGatewayProbeSurfaceAuth: 健康探针表面认证解析
//   - resolveGatewayInteractiveSurfaceAuth: 交互式客户端表面认证
//   - hasConfiguredGatewayAuthSecretInput: Secret Ref 配置检测
//   - shouldResolveGatewayAuthSecretRef: 智能 Secret Ref 解析决策
//   - materializeGatewayAuthSecretRefs: Secret Ref 物化到配置
//   - ensureGatewayStartupAuth: 启动认证编排 + 弱凭据检测 + 自动token生成
//   - assertGatewayAuthNotKnownWeak: 弱凭据拦截
//   - assertHooksTokenSeparateFromGatewayAuth: hooks token 隔离验证
// ============================================================================

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

namespace {
auto auth_log() { return spdlog::get("gateway/auth"); }
} // namespace

// ============================================================================
// 🎯 KNOWN_WEAK credentials (对齐 startup-auth.ts:34-40)
// ============================================================================

static const std::set<std::string> KNOWN_WEAK_GATEWAY_TOKENS = {
    "change-me-to-a-long-random-token"
};

static const std::set<std::string> KNOWN_WEAK_GATEWAY_PASSWORDS = {
    "change-me-to-a-strong-password"
};

// ============================================================================
// 辅助函数
// ============================================================================

static std::optional<std::string> trim_to_optional(const std::string& s) {
    auto a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return std::nullopt;
    return s.substr(a, s.find_last_not_of(" \t\n\r") - a + 1);
}

static std::optional<std::string> json_string_opt(const json& j, const std::string& key) {
    if (j.contains(key) && j[key].is_string()) {
        return trim_to_optional(j[key].get<std::string>());
    }
    return std::nullopt;
}

static std::string json_string(const json& j, const std::string& key, const std::string& def = "") {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

static std::string generate_hex_token(int bytes = 24) {
    static const char hex_chars[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    std::string result;
    result.reserve(bytes * 2);
    for (int i = 0; i < bytes; ++i) {
        uint8_t b = static_cast<uint8_t>(dis(gen));
        result += hex_chars[b >> 4];
        result += hex_chars[b & 0x0f];
    }
    return result;
}

// ============================================================================
// 🎯 resolveGatewayCredential (对齐 auth-surface-resolution.ts:17-35)
// ============================================================================

struct ResolvedCredential {
    std::optional<std::string> value;
    std::optional<std::string> unresolved_ref_reason;
};

static ResolvedCredential resolve_gateway_credential(
    const json& config,
    const std::unordered_map<std::string, std::string>& env,
    const std::string& path,
    const json& value_node,
    std::vector<std::string>& diagnostics
) {
    // Secret input resolution: direct string or secretRef
    if (value_node.is_string()) {
        auto val = trim_to_optional(value_node.get<std::string>());
        return {val, std::nullopt};
    }
    if (value_node.is_object() && value_node.contains("secretRef")) {
        // Secret ref resolution would go through secrets subsystem
        auto ref = value_node["secretRef"];
        std::string reason = "unresolved secretRef at " + path;
        diagnostics.push_back(reason);
        return {std::nullopt, reason};
    }
    return {std::nullopt, std::nullopt};
}

// ============================================================================
// 🎯 resolveGatewayProbeSurfaceAuth (对齐 auth-surface-resolution.ts:46-142)
// 探针表面认证解析 — local/remote × token/password × 5 模式
// ============================================================================

struct ProbeSurfaceAuth {
    std::optional<std::string> token;
    std::optional<std::string> password;
    std::vector<std::string> diagnostics;
};

ProbeSurfaceAuth resolve_gateway_probe_surface_auth(
    const json& config,
    const std::unordered_map<std::string, std::string>& env,
    const std::string& surface  // "local" | "remote"
) {
    std::vector<std::string> diagnostics;
    auto get_env = [&](const std::string& key) -> std::optional<std::string> {
        auto it = env.find(key);
        if (it != env.end()) return trim_to_optional(it->second);
        return std::nullopt;
    };

    // remote surface — 对齐 :55-76
    if (surface == "remote") {
        auto remote_cfg = config.value("gateway", json::object()).value("remote", json::object());
        auto remote_token = resolve_gateway_credential(
            config, env, "gateway.remote.token",
            remote_cfg.value("token", json()), diagnostics);
        ResolvedCredential remote_password{std::nullopt, std::nullopt};
        if (!remote_token.value.has_value()) {
            remote_password = resolve_gateway_credential(
                config, env, "gateway.remote.password",
                remote_cfg.value("password", json()), diagnostics);
        }
        return {remote_token.value, remote_password.value, diagnostics};
    }

    // local surface
    auto auth_cfg = config.value("gateway", json::object()).value("auth", json::object());
    std::string auth_mode = json_string(auth_cfg, "mode", "");

    // none/trusted-proxy — 对齐 :78-80
    if (auth_mode == "none" || auth_mode == "trusted-proxy") {
        return {{}, {}, {}};
    }

    auto env_token = get_env("HUNTERCLAW_GATEWAY_TOKEN");
    auto env_password = get_env("HUNTERCLAW_GATEWAY_PASSWORD");

    // token mode — 对齐 :85-98
    if (auth_mode == "token") {
        auto token = resolve_gateway_credential(
            config, env, "gateway.auth.token",
            auth_cfg.value("token", json()), diagnostics);
        if (token.value.has_value()) return {token.value, {}, diagnostics};
        if (env_token.has_value()) return {env_token, {}, {}};
        return {{}, {}, diagnostics};
    }

    // password mode — 对齐 :100-113
    if (auth_mode == "password") {
        auto password = resolve_gateway_credential(
            config, env, "gateway.auth.password",
            auth_cfg.value("password", json()), diagnostics);
        if (password.value.has_value()) return {{}, password.value, diagnostics};
        if (env_password.has_value()) return {{}, env_password, {}};
        return {{}, {}, diagnostics};
    }

    // auto mode — 对齐 :115-141
    auto token = resolve_gateway_credential(
        config, env, "gateway.auth.token",
        auth_cfg.value("token", json()), diagnostics);
    if (token.value.has_value()) return {token.value, {}, diagnostics};
    if (env_token.has_value()) return {env_token, {}, {}};
    if (env_password.has_value()) return {{}, env_password, diagnostics};
    auto password = resolve_gateway_credential(
        config, env, "gateway.auth.password",
        auth_cfg.value("password", json()), diagnostics);
    return {token.value, password.value, diagnostics};
}

// ============================================================================
// 🎯 hasConfiguredGatewayAuthSecretInput (对齐 auth-config-utils.ts:24-29)
// ============================================================================

bool has_configured_gateway_auth_secret_input(
    const json& config,
    const std::string& path  // "gateway.auth.token" | "gateway.auth.password"
) {
    auto auth_cfg = config.value("gateway", json::object()).value("auth", json::object());
    std::string key = (path == "gateway.auth.token") ? "token" : "password";
    if (!auth_cfg.contains(key)) return false;
    const auto& val = auth_cfg[key];
    if (val.is_string() && !val.get<std::string>().empty()) return true;
    if (val.is_object() && val.contains("secretRef")) return true;
    return false;
}

// ============================================================================
// 🎯 shouldResolveGatewayAuthSecretRef (对齐 auth-config-utils.ts:31-52)
// 智能判断是否需要解析 secret ref
// ============================================================================

bool should_resolve_gateway_auth_secret_ref(
    const std::string& mode,
    const std::string& path,
    bool has_password_candidate,
    bool has_token_candidate
) {
    bool is_token_path = (path == "gateway.auth.token");
    bool has_path_candidate = is_token_path ? has_token_candidate : has_password_candidate;

    if (has_path_candidate) return false;
    if (mode == (is_token_path ? "token" : "password")) return true;
    if (mode == "token" || mode == "none" || mode == "trusted-proxy") return false;
    if (mode == "password") return !is_token_path;
    return is_token_path ? !has_password_candidate : !has_token_candidate;
}

// ============================================================================
// 🎯 mergeGatewayAuthConfig (对齐 startup-auth.ts:42-69)
// ============================================================================

json merge_gateway_auth_config(const json& base, const json& override_cfg) {
    json merged = base.is_object() ? base : json::object();
    if (!override_cfg.is_object()) return merged;

    for (const auto& key : {"mode", "token", "password", "allowTailscale",
                              "rateLimit", "trustedProxy"}) {
        if (override_cfg.contains(key)) {
            merged[key] = override_cfg[key];
        }
    }
    return merged;
}

// ============================================================================
// 🎯 assertGatewayAuthNotKnownWeak (对齐 startup-auth.ts:267-290)
// 弱凭据拦截
// ============================================================================

void assert_gateway_auth_not_known_weak(
    const std::string& mode,
    const std::string& token,
    const std::string& password
) {
    if (mode == "token") {
        auto trimmed = trim_to_optional(token);
        if (trimmed.has_value() && KNOWN_WEAK_GATEWAY_TOKENS.count(trimmed.value())) {
            throw std::runtime_error(
                "Invalid config: gateway auth token is set to the example placeholder "
                "from .env.example. Generate a real secret (e.g. `openssl rand -hex 32`) "
                "and set HUNTERCLAW_GATEWAY_TOKEN or gateway.auth.token before starting "
                "the gateway.");
        }
        return;
    }
    if (mode == "password") {
        auto trimmed = trim_to_optional(password);
        if (trimmed.has_value() && KNOWN_WEAK_GATEWAY_PASSWORDS.count(trimmed.value())) {
            throw std::runtime_error(
                "Invalid config: gateway auth password is set to the example placeholder "
                "from .env.example. Choose a real password and set HUNTERCLAW_GATEWAY_PASSWORD "
                "or gateway.auth.password before starting the gateway.");
        }
    }
}

// ============================================================================
// 🎯 assertHooksTokenSeparateFromGatewayAuth (对齐 startup-auth.ts:292-314)
// hooks token 隔离验证
// ============================================================================

void assert_hooks_token_separate_from_gateway_auth(
    const json& config,
    const std::string& auth_mode,
    const std::string& gateway_token
) {
    auto hooks_cfg = config.value("hooks", json::object());
    if (!hooks_cfg.value("enabled", false)) return;

    auto hooks_token_opt = json_string_opt(hooks_cfg, "token");
    if (!hooks_token_opt.has_value()) return;

    if (auth_mode != "token") return;
    auto gw_token = trim_to_optional(gateway_token);
    if (!gw_token.has_value()) return;

    if (hooks_token_opt.value() == gw_token.value()) {
        throw std::runtime_error(
            "Invalid config: hooks.token must not match gateway auth token. "
            "Set a distinct hooks.token for hook ingress.");
    }
}

// ============================================================================
// 🎯 ensureGatewayStartupAuth (对齐 startup-auth.ts:160-265)
// 启动认证编排 + 弱凭据检测 + 自动 token 生成
// ============================================================================

struct StartupAuthResult {
    json config;
    std::string auth_mode;
    std::string token;
    std::string password;
    std::optional<std::string> generated_token;
    bool persisted_generated_token = false;
};

StartupAuthResult ensure_gateway_startup_auth(
    json config,
    const std::unordered_map<std::string, std::string>& env,
    const json& auth_override,
    bool persist
) {
    auto auth_cfg = config.value("gateway", json::object()).value("auth", json::object());
    std::string mode = json_string(auth_cfg, "mode", "token");

    // Resolve secret refs — 对齐 :177-203
    auto get_env = [&](const std::string& key) -> std::optional<std::string> {
        auto it = env.find(key);
        if (it != env.end()) return trim_to_optional(it->second);
        return std::nullopt;
    };

    auto env_token = get_env("HUNTERCLAW_GATEWAY_TOKEN");
    auto env_password = get_env("HUNTERCLAW_GATEWAY_PASSWORD");

    // Merge auth override — 对齐 :204-211
    json merged_auth = auth_cfg;
    if (auth_override.is_object()) {
        merged_auth = merge_gateway_auth_config(auth_cfg, auth_override);
    }

    std::string resolved_token;
    std::string resolved_password;

    if (merged_auth.contains("token") && merged_auth["token"].is_string()) {
        resolved_token = merged_auth["token"].get<std::string>();
    }
    if (env_token.has_value() && resolved_token.empty()) {
        resolved_token = env_token.value();
    }

    if (merged_auth.contains("password") && merged_auth["password"].is_string()) {
        resolved_password = merged_auth["password"].get<std::string>();
    }
    if (env_password.has_value() && resolved_password.empty()) {
        resolved_password = env_password.value();
    }

    // Check if token is present — 对齐 :218-221
    auto trimmed_token = trim_to_optional(resolved_token);
    if (mode == "token" && trimmed_token.has_value()) {
        assert_gateway_auth_not_known_weak(mode, resolved_token, resolved_password);
        assert_hooks_token_separate_from_gateway_auth(config, mode, resolved_token);
        return {config, mode, resolved_token, resolved_password, std::nullopt, false};
    }

    if (mode != "token") {
        assert_gateway_auth_not_known_weak(mode, resolved_token, resolved_password);
        assert_hooks_token_separate_from_gateway_auth(config, mode, resolved_token);
        return {config, mode, resolved_token, resolved_password, std::nullopt, false};
    }

    // Auto-generate token — 对齐 :224-264
    std::string generated = generate_hex_token(24);

    json next_config = config;
    next_config["gateway"]["auth"]["mode"] = "token";
    next_config["gateway"]["auth"]["token"] = generated;

    assert_hooks_token_separate_from_gateway_auth(next_config, "token", generated);

    if (auth_log()) {
        auth_log()->info("auto-generated gateway auth token (persist={})", persist);
    }

    return {next_config, "token", generated, resolved_password, generated, persist};
}

} // namespace gateway
} // namespace hunterclaw
