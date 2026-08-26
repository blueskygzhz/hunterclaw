// ============================================================================
// remote_a2a/auth.cc
// 鉴权配置解析。★ 明文凭据一律拒绝；apiKey in!=header 显式失败。
// ============================================================================

#include "hunterclaw/remote_a2a/auth.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace hunterclaw {
namespace remote_a2a {

namespace {

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string jstr(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

bool jbool(const nlohmann::json& j, const char* key, bool def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

/// 从 cfg 里挑出 (source, env, file, ref) 三选一
/// prefix 为空时读 token_env/token_file/secret_ref，
/// 为 "password" 时读 password_env/password_file/password_ref
bool pick_source(const nlohmann::json& cfg, const char* prefix, SecretSource* out_src,
                 std::string* out_env, std::string* out_file, std::string* out_ref) {
    const std::string p = (prefix && *prefix) ? std::string(prefix) : std::string("token");
    const std::string k_env = p + "_env";
    const std::string k_file = p + "_file";
    const std::string k_ref = (p == "token") ? std::string("secret_ref") : (p + "_ref");

    std::string v = jstr(cfg, k_env.c_str());
    if (!v.empty()) { *out_src = SecretSource::Env; *out_env = v; return true; }
    v = jstr(cfg, k_file.c_str());
    if (!v.empty()) { *out_src = SecretSource::File; *out_file = v; return true; }
    v = jstr(cfg, k_ref.c_str());
    if (!v.empty()) { *out_src = SecretSource::SecretRef; *out_ref = v; return true; }
    return false;
}

}  // namespace

const char* auth_kind_text(AuthKind k) {
    switch (k) {
        case AuthKind::None:   return "none";
        case AuthKind::Bearer: return "bearer";
        case AuthKind::ApiKey: return "apikey";
        case AuthKind::Basic:  return "basic";
        case AuthKind::Custom: return "custom";
        case AuthKind::OAuth2: return "oauth2";
        case AuthKind::Mtls:   return "mtls";
    }
    return "unknown";
}

core::Result<AuthSpec> parse_auth_spec(const nlohmann::json& cfg) {
    AuthSpec s;

    if (cfg.is_null()) {
        s.kind = AuthKind::None;
        return core::Result<AuthSpec>::Ok(std::move(s));
    }
    if (!cfg.is_object()) {
        return core::Result<AuthSpec>::Err("auth 必须是对象");
    }

    // ★★ 明文凭据一律拒绝，错误信息指向正确做法 ★★
    static const char* const kForbidden[] = {
        "token", "password", "secret", "api_key", "apikey",
        "client_secret", "bearer_token", "access_token", "key",
    };
    for (const char* f : kForbidden) {
        auto it = cfg.find(f);
        if (it != cfg.end() && it->is_string()) {
            return core::Result<AuthSpec>::Err(
                std::string("禁止在配置中写明文凭据字段 '") + f +
                "'。请改用 token_env / token_file / secret_ref。");
        }
    }

    const std::string type = to_lower_copy(jstr(cfg, "type"));

    if (type.empty() || type == "none") {
        s.kind = AuthKind::None;
        s.authenticate_card_request = false;
        return core::Result<AuthSpec>::Ok(std::move(s));
    }

    if (type == "bearer") {
        s.kind = AuthKind::Bearer;
    } else if (type == "apikey" || type == "api_key") {
        s.kind = AuthKind::ApiKey;
        s.header_name = jstr(cfg, "header_name");
        if (s.header_name.empty()) {
            // ★ 不从卡推断 —— 卡是对端可控的
            return core::Result<AuthSpec>::Err(
                "apikey 必须显式指定 header_name（例如 X-API-Key）");
        }
    } else if (type == "basic") {
        s.kind = AuthKind::Basic;
        s.username = jstr(cfg, "username");
        if (s.username.empty()) {
            return core::Result<AuthSpec>::Err("basic 必须指定 username");
        }
        if (!pick_source(cfg, "password", &s.password_source, &s.password_env,
                         &s.password_file, &s.password_ref)) {
            return core::Result<AuthSpec>::Err(
                "basic 必须指定 password_env / password_file / password_ref 之一");
        }
    } else if (type == "custom") {
        s.kind = AuthKind::Custom;
        s.scheme_name = jstr(cfg, "scheme_name");
        if (s.scheme_name.empty()) {
            return core::Result<AuthSpec>::Err(
                "custom 必须指定 scheme_name（例如 Token）");
        }
        // scheme 名不能带空格（会破坏 Authorization 头结构）
        if (s.scheme_name.find(' ') != std::string::npos) {
            return core::Result<AuthSpec>::Err("scheme_name 不能包含空格");
        }
    } else if (type == "oauth2" || type == "openidconnect") {
        return core::Result<AuthSpec>::Err(
            "oauth2 一期不支持（二期复用 MCP OAuth 体系：mcp_oauth_*）");
    } else if (type == "mtls" || type == "mutualtls") {
        return core::Result<AuthSpec>::Err(
            "mtls 暂不支持（需扩展 infra::HttpClient 以传递客户端证书）");
    } else {
        return core::Result<AuthSpec>::Err("未知 auth.type: " + type);
    }

    // 凭据来源（Basic 用 password_* 已在上面处理，其 token 源可省）
    if (s.needs_secret()) {
        if (!pick_source(cfg, "token", &s.source, &s.env_name, &s.file_path,
                         &s.secret_ref)) {
            return core::Result<AuthSpec>::Err(
                std::string("auth.type=") + type +
                " 必须指定 token_env / token_file / secret_ref 之一");
        }
    }

    // additional_headers
    {
        auto it = cfg.find("additional_headers");
        if (it != cfg.end() && it->is_object()) {
            for (auto kv = it->begin(); kv != it->end(); ++kv) {
                if (!kv.value().is_string()) continue;
                const std::string hk = kv.key();
                // 不允许覆盖 Authorization（会绕过 kind 的语义）
                if (to_lower_copy(hk) == "authorization") {
                    return core::Result<AuthSpec>::Err(
                        "additional_headers 不允许覆盖 Authorization");
                }
                s.additional_headers[hk] = kv.value().get<std::string>();
            }
        }
    }

    s.authenticate_card_request = jbool(cfg, "authenticate_card_request", false);
    return core::Result<AuthSpec>::Ok(std::move(s));
}

core::Result<AuthKind> infer_kind_from_card_scheme(const nlohmann::json& scheme) {
    if (!scheme.is_object()) {
        return core::Result<AuthKind>::Err("securityScheme 必须是对象");
    }
    const std::string t = to_lower_copy(jstr(scheme, "type"));

    if (t == "apikey") {
        std::string loc = to_lower_copy(jstr(scheme, "in"));
        if (loc.empty()) loc = to_lower_copy(jstr(scheme, "location"));  // 1.x proto
        if (loc.empty()) loc = "header";
        // ★★ 显式失败，不像 ADK 只 warning ★★
        //   ADK: _auth_headers.py:97-104 只 logger.warning，凭据被静默丢弃，
        //   请求以无鉴权发出，远端返回 401 —— 运维看到的是 401 而非"配置不支持"。
        //   另外 in=query 本身是坏实践（凭据进 URL/日志/Referer），
        //   与现有 hooks 拒绝 query token 的做法一致（http_gateway.cc:1439-1451）。
        if (loc != "header") {
            return core::Result<AuthKind>::Err(
                "apiKey in='" + loc + "' 不受支持（仅支持 header）");
        }
        return core::Result<AuthKind>::Ok(AuthKind::ApiKey);
    }
    if (t == "http") {
        const std::string sc = to_lower_copy(jstr(scheme, "scheme"));
        if (sc == "bearer") return core::Result<AuthKind>::Ok(AuthKind::Bearer);
        if (sc == "basic") return core::Result<AuthKind>::Ok(AuthKind::Basic);
        if (sc.empty()) {
            return core::Result<AuthKind>::Err("http securityScheme 缺少 scheme 字段");
        }
        return core::Result<AuthKind>::Ok(AuthKind::Custom);
    }
    if (t == "oauth2" || t == "openidconnect") {
        return core::Result<AuthKind>::Ok(AuthKind::OAuth2);
    }
    if (t == "mutualtls") {
        return core::Result<AuthKind>::Ok(AuthKind::Mtls);
    }
    return core::Result<AuthKind>::Err("未知 securityScheme.type: " +
                                      (t.empty() ? "(空)" : t));
}

std::vector<std::string> check_auth_consistency(const nlohmann::json& security_schemes,
                                                const AuthSpec& local) {
    std::vector<std::string> warns;
    if (!security_schemes.is_object() || security_schemes.empty()) {
        if (local.kind != AuthKind::None) {
            warns.push_back(
                "卡未声明 securitySchemes，但本地配置了鉴权 —— "
                "可能是卡未按规范填写（如 Google ADK 的示例卡），"
                "凭据仍会照常发送");
        }
        return warns;
    }

    bool matched = false;
    std::vector<std::string> declared;
    for (auto it = security_schemes.begin(); it != security_schemes.end(); ++it) {
        auto k = infer_kind_from_card_scheme(it.value());
        if (!k.ok) {
            warns.push_back("卡内 securityScheme '" + it.key() + "' 无法识别: " +
                            k.error);
            continue;
        }
        declared.push_back(auth_kind_text(*k.value));
        if (*k.value == local.kind) matched = true;
    }

    if (!matched && local.kind != AuthKind::None && !declared.empty()) {
        std::string list;
        for (size_t i = 0; i < declared.size(); ++i) {
            if (i) list += ", ";
            list += declared[i];
        }
        warns.push_back(std::string("本地鉴权方式 '") + auth_kind_text(local.kind) +
                        "' 不在卡声明的 [" + list + "] 之中 —— 可能会被远端拒绝");
    }
    if (local.kind == AuthKind::None && !declared.empty()) {
        warns.push_back("卡声明了鉴权要求但本地未配置凭据 —— 调用可能返回 401");
    }
    return warns;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
