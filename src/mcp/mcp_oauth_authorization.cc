// 🎯 对齐 OpenClaw MCP OAuth Authorization Code + PKCE 登录（方案 B：无回调 server）。
//   参见头文件说明。discovery/token 端点解析与 mcp_oauth_token_exchange.cc 保持一致口径。

#include "hunterclaw/mcp/mcp_oauth_authorization.h"
#include "hunterclaw/mcp/mcp_oauth.h"
#include "hunterclaw/mcp/mcp_oauth_persistence.h"
#include "hunterclaw/mcp/mcp_oauth_token_exchange.h"  // form_url_encode
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/infra/utilities.h"
#include "hunterclaw/mcp/json_converter.h"

#include <openssl/sha.h>
#include <openssl/rand.h>

#include <chrono>
#include <vector>

namespace hunterclaw {
namespace mcp {

namespace {
using json = nlohmann::json;

// 对齐 mcp-oauth-provider.ts LEGACY_DEFAULT_REDIRECT_URL。
constexpr const char* LEGACY_DEFAULT_REDIRECT_URL = "http://127.0.0.1:8989/oauth/callback";

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// application/x-www-form-urlencoded / query 单值编码由 mcp_oauth_token_exchange.h 提供
// （form_url_encode，RFC 3986 unreserved 之外转义）。

// 读 store.clientInformation.client_id。
std::optional<std::string> client_id_of(const McpOAuthStore& store) {
    if (!store.client_information.has_value() || !store.client_information->is_object())
        return std::nullopt;
    const json& c = *store.client_information;
    if (c.contains("client_id") && c["client_id"].is_string()) {
        std::string v = c["client_id"].get<std::string>();
        if (!v.empty()) return v;
    }
    return std::nullopt;
}

// 从 store.discoveryState 取授权服务器基址。
std::optional<std::string> auth_server_from_store(const McpOAuthStore& store) {
    if (!store.discovery_state.has_value() || !store.discovery_state->is_object())
        return std::nullopt;
    const json& d = *store.discovery_state;
    if (d.contains("authorizationServerUrl") && d["authorizationServerUrl"].is_string())
        return d["authorizationServerUrl"].get<std::string>();
    return std::nullopt;
}

std::optional<std::string> endpoint_from_store(const McpOAuthStore& store, const char* key,
                                               const char* meta_key) {
    if (!store.discovery_state.has_value() || !store.discovery_state->is_object())
        return std::nullopt;
    const json& d = *store.discovery_state;
    if (d.contains(key) && d[key].is_string()) return d[key].get<std::string>();
    if (d.contains("metadata") && d["metadata"].is_object()) {
        const json& m = d["metadata"];
        if (m.contains(meta_key) && m[meta_key].is_string()) return m[meta_key].get<std::string>();
    }
    return std::nullopt;
}

std::string well_known_metadata_url(const std::string& auth_server_url) {
    std::string base = auth_server_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/.well-known/oauth-authorization-server";
}

std::string resolve_redirect_uri(const McpOAuthLoginConfig& config, const McpOAuthStore& store) {
    if (config.redirect_url.has_value() && !config.redirect_url->empty())
        return *config.redirect_url;
    if (store.redirect_url.has_value() && !store.redirect_url->empty())
        return *store.redirect_url;
    return LEGACY_DEFAULT_REDIRECT_URL;
}

// 拉取授权服务器 metadata（返回含 authorization_endpoint/token_endpoint的 json）。
core::Result<json> fetch_auth_server_metadata(
    const std::shared_ptr<infra::HttpClient>& http, const std::string& auth_server, int timeout_ms) {
    using Ret = core::Result<json>;
    auto md = http->get(well_known_metadata_url(auth_server), timeout_ms);
    if (md.is_err() || !md.get().ok) return Ret::Err("authorization server metadata discovery failed");
    auto md_json = md.get().json();
    if (md_json.is_err()) return Ret::Err("authorization server metadata is not valid JSON");
    return Ret::Ok(converters::JsonConverter::to_nlohmann(md_json.get()));
}

} // namespace

std::string sha256_raw(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    return std::string(reinterpret_cast<const char*>(digest), SHA256_DIGEST_LENGTH);
}

std::string base64url_encode_no_pad(const std::string& raw_bytes) {
    std::vector<uint8_t> bytes(raw_bytes.begin(), raw_bytes.end());
    std::string b64 = infra::base64_encode(bytes);
    // base64 → base64url: '+'→'-', '/'→'_', 去除 '=' padding。
    std::string out;
    out.reserve(b64.size());
    for (char c : b64) {
        if (c == '+') out.push_back('-');
        else if (c == '/') out.push_back('_');
        else if (c == '=') continue;
        else out.push_back(c);
    }
    return out;
}

PkceChallenge generate_pkce_challenge() {
    // code_verifier: 32 字节随机 → base64url（43 字符，符合 RFC 7636 §4.1）。
    unsigned char rnd[32];
    if (RAND_bytes(rnd, sizeof(rnd)) != 1) {
        // 退化：用 uuid 拼接兜底（极少发生；仍满足熵与字符集要求）。
        std::string fallback = infra::generate_uuid() + infra::generate_uuid();
        std::string verifier = base64url_encode_no_pad(sha256_raw(fallback));
        PkceChallenge c;
        c.code_verifier = verifier;
        c.code_challenge = base64url_encode_no_pad(sha256_raw(verifier));
        return c;
    }
    std::string verifier = base64url_encode_no_pad(
        std::string(reinterpret_cast<const char*>(rnd), sizeof(rnd)));
    PkceChallenge c;
    c.code_verifier = verifier;
    c.code_challenge = base64url_encode_no_pad(sha256_raw(verifier));  // S256
    c.method = "S256";
    return c;
}

std::string build_authorization_url(
    const std::string& authorization_endpoint, const std::string& client_id,
    const std::string& redirect_uri, const std::string& code_challenge,
    const std::string& state, const std::optional<std::string>& scope) {
    std::string url = authorization_endpoint;
    url += (authorization_endpoint.find('?') == std::string::npos) ? "?" : "&";
    url += "response_type=code";
    url += "&client_id=" + form_url_encode(client_id);
    url += "&redirect_uri=" + form_url_encode(redirect_uri);
    url += "&code_challenge=" + form_url_encode(code_challenge);
    url += "&code_challenge_method=S256";
    url += "&state=" + form_url_encode(state);
    if (scope.has_value() && !scope->empty()) {
        url += "&scope=" + form_url_encode(*scope);
    }
    return url;
}

nlohmann::json build_dynamic_client_registration_body(
    const std::string& client_name, const std::string& redirect_uri,
    const std::optional<std::string>& scope) {
    // 对齐 SDK registerClient 的 client metadata（RFC 7591 §2）：
    //   token_endpoint_auth_method="none" →公开客户端（配合 PKCE），
    //   grant_types=[authorization_code, refresh_token]，response_types=[code]。
    json body = {
        {"client_name", client_name},
        {"redirect_uris", json::array({redirect_uri})},
        {"grant_types", json::array({"authorization_code", "refresh_token"})},
        {"response_types", json::array({"code"})},
        {"token_endpoint_auth_method", "none"},
    };
    if (scope.has_value() && !scope->empty()) body["scope"] = *scope;
    return body;
}

core::Result<McpOAuthStore> apply_authorized_tokens(
    const McpOAuthStore& current, const nlohmann::json& token_response, int64_t now_ms_val) {
    using Ret = core::Result<McpOAuthStore>;
    if (!token_response.is_object() || !token_response.contains("access_token") ||
        !token_response["access_token"].is_string()) {
        return Ret::Err("token response missing access_token");
    }
    McpOAuthStore next = current;
    // 对齐 saveTokens: tokens =响应；删 credentialState/pendingAuthorizationChallenge/codeVerifier。
    next.tokens = token_response;
    next.credential_state = std::nullopt;
    next.pending_authorization_challenge = std::nullopt;
    next.code_verifier = std::nullopt;
    next.last_authorization_url = std::nullopt;
    // resolveTokenExpiresAt。
    if (token_response.contains("expires_in") && token_response["expires_in"].is_number()) {
        double expires_in = token_response["expires_in"].get<double>();
        next.token_expires_at = static_cast<double>(now_ms_val) + expires_in * 1000.0;
    } else {
        next.token_expires_at = std::nullopt;
    }
    // 写前校验（对齐 replaceMcpOAuthStore 的 parse）。
    auto validated = parse_mcp_oauth_store("<login>", next.to_json());
    if (validated.is_err()) return Ret::Err(validated.error);
    return Ret::Ok(std::move(next));
}

core::Result<McpOAuthLoginResult> begin_mcp_oauth_login(
    const McpOAuthLoginConfig& config,
    const std::function<void(const std::string&)>& on_authorization_url) {
    using Ret = core::Result<McpOAuthLoginResult>;
    const std::string store_key =
        resolve_mcp_oauth_store_key(config.server_name, config.server_url);

    auto store_res = read_mcp_oauth_store(store_key);
    if (store_res.is_err()) return Ret::Err(store_res.error);
    McpOAuthStore store = store_res.get();

    std::shared_ptr<infra::HttpClient> http = config.http;
    if (!http) http = infra::HttpClient::create();

    // 1. 解析授权服务器+ authorization_endpoint（显式 > store缓存 > discovery）。
    std::optional<std::string> auth_endpoint =
        endpoint_from_store(store, "authorizationEndpoint", "authorization_endpoint");
    std::optional<std::string> token_endpoint =
        endpoint_from_store(store, "tokenEndpoint", "token_endpoint");
    std::optional<std::string> registration_endpoint =
        config.registration_endpoint.has_value()
            ? config.registration_endpoint
            : endpoint_from_store(store, "registrationEndpoint", "registration_endpoint");
    std::optional<std::string> auth_server =
        config.authorization_server_url.has_value() ? config.authorization_server_url
                                                    : auth_server_from_store(store);

    // discovery：任一端点缺失时拉一次 metadata（顺带拿 registration_endpoint）。
    if (!auth_endpoint.has_value() || !token_endpoint.has_value() ||
        !registration_endpoint.has_value()) {
        if (!auth_server.has_value()) {
            return Ret::Err("cannot resolve authorization endpoint: no authorization server url");
        }
        auto meta = fetch_auth_server_metadata(http, *auth_server, config.timeout_ms);
        if (meta.is_err()) return Ret::Err(meta.error);
        const json& m = meta.get();
        if (!auth_endpoint.has_value()) {
            if (!m.contains("authorization_endpoint") || !m["authorization_endpoint"].is_string())
                return Ret::Err("authorization server metadata missing authorization_endpoint");
            auth_endpoint = m["authorization_endpoint"].get<std::string>();
        }
        if (!token_endpoint.has_value() && m.contains("token_endpoint") &&
            m["token_endpoint"].is_string()) {
            token_endpoint = m["token_endpoint"].get<std::string>();
        }
        if (!registration_endpoint.has_value() && m.contains("registration_endpoint") &&
            m["registration_endpoint"].is_string()) {
            registration_endpoint = m["registration_endpoint"].get<std::string>();
        }
    }

    // 2. client_id（显式 > store > RFC 7591 动态注册）。对齐 SDK auth() 的 registerClient
    //    fallback：无 clientInformation 且服务器暴露 registration_endpoint → 动态注册。
    std::optional<std::string> client_id =
        config.client_id.has_value() ? config.client_id : client_id_of(store);
    std::optional<json> registered_client_information;
    std::string redirect_uri = resolve_redirect_uri(config, store);
    if (!client_id.has_value()) {
        if (!registration_endpoint.has_value()) {
            return Ret::Err(
                "MCP server \"" + config.server_name +
                "\" OAuth login requires a client_id (no registration_endpoint for dynamic "
                "registration; configure server.oauth.client_id).");
        }
        json reg_body = build_dynamic_client_registration_body(
            config.client_name.value_or("HunterClaw MCP"), redirect_uri, config.scope);
        infra::HttpRequestOptions reg_opts;
        reg_opts.headers["Content-Type"] = "application/json";
        reg_opts.headers["Accept"] = "application/json";
        reg_opts.body = reg_body.dump();
        reg_opts.timeout_ms = config.timeout_ms;
        auto reg_resp = http->request("POST", *registration_endpoint, reg_opts);
        if (reg_resp.is_err() || !reg_resp.get().ok) {
            return Ret::Err("MCP server \"" + config.server_name +
                            "\" dynamic client registration failed");
        }
        auto reg_json = reg_resp.get().json();
        if (reg_json.is_err())
            return Ret::Err("dynamic client registration response is not valid JSON");
        json ci = converters::JsonConverter::to_nlohmann(reg_json.get());
        if (!ci.is_object() || !ci.contains("client_id") || !ci["client_id"].is_string()) {
            return Ret::Err("dynamic client registration response missing client_id");
        }
        client_id = ci["client_id"].get<std::string>();
        registered_client_information = ci;  // 对齐 saveClientInformation：保存完整注册信息。
    }

    // 3. 生成 PKCE + state，构造 authorization URL。
    PkceChallenge pkce = generate_pkce_challenge();
    std::string state = infra::generate_uuid();
    std::string authorization_url = build_authorization_url(
        *auth_endpoint, *client_id, redirect_uri, pkce.code_challenge, state, config.scope);

    // 4. 持久化：codeVerifier + redirectUrl + lastAuthorizationUrl + clientInformation
    //    + discoveryState 端点缓存。
    auto written = update_mcp_oauth_store(
        store_key, [&](const McpOAuthStore& cur) {
            McpOAuthStore next = cur;
            // beginMcpOAuthAuthorization: uninitialized → 清除 credentialState。
            if (next.credential_state.has_value() && *next.credential_state == "uninitialized")
                next.credential_state = std::nullopt;
            next.code_verifier = pkce.code_verifier;
            next.redirect_url = redirect_uri;
            next.last_authorization_url = authorization_url;
            // clientInformation：动态注册结果整体保存；否则仅在缺失时写入 client_id。
            if (registered_client_information.has_value()) {
                next.client_information = *registered_client_information;
            } else if (!client_id_of(next).has_value() && client_id.has_value()) {
                json ci = next.client_information.value_or(json::object());
                if (!ci.is_object()) ci = json::object();
                ci["client_id"] = *client_id;
                next.client_information = ci;
            }
            // discoveryState 端点缓存（需含 authorizationServerUrl 以过校验）。
            if (auth_server.has_value()) {
                json ds = next.discovery_state.has_value() && next.discovery_state->is_object()
                              ? *next.discovery_state : json::object();
                ds["authorizationServerUrl"] = *auth_server;
                if (auth_endpoint.has_value()) ds["authorizationEndpoint"] = *auth_endpoint;
                if (token_endpoint.has_value()) ds["tokenEndpoint"] = *token_endpoint;
                if (registration_endpoint.has_value())
                    ds["registrationEndpoint"] = *registration_endpoint;
                next.discovery_state = ds;
            }
            return next;
        });
    if (written.is_err()) return Ret::Err(written.error);

    if (on_authorization_url) on_authorization_url(authorization_url);
    return Ret::Ok(McpOAuthLoginResult::Redirect);
}

core::Result<McpOAuthLoginResult> complete_mcp_oauth_login(
    const McpOAuthLoginConfig& config, const std::string& authorization_code) {
    using Ret = core::Result<McpOAuthLoginResult>;
    const std::string store_key =
        resolve_mcp_oauth_store_key(config.server_name, config.server_url);

    auto store_res = read_mcp_oauth_store(store_key);
    if (store_res.is_err()) return Ret::Err(store_res.error);
    McpOAuthStore store = store_res.get();

    if (!store.code_verifier.has_value()) {
        return Ret::Err("Missing MCP OAuth code verifier. Run the login flow again.");
    }
    std::string code_verifier = *store.code_verifier;

    std::shared_ptr<infra::HttpClient> http = config.http;
    if (!http) http = infra::HttpClient::create();

    // token endpoint（store缓存 > discovery）。
    std::optional<std::string> token_endpoint =
        endpoint_from_store(store, "tokenEndpoint", "token_endpoint");
    std::optional<std::string> auth_server =
        config.authorization_server_url.has_value() ? config.authorization_server_url
                                                    : auth_server_from_store(store);
    if (!token_endpoint.has_value()) {
        if (!auth_server.has_value())
            return Ret::Err("cannot resolve token endpoint: no authorization server url");
        auto meta = fetch_auth_server_metadata(http, *auth_server, config.timeout_ms);
        if (meta.is_err()) return Ret::Err(meta.error);
        const json& m = meta.get();
        if (!m.contains("token_endpoint") || !m["token_endpoint"].is_string())
            return Ret::Err("authorization server metadata missing token_endpoint");
        token_endpoint = m["token_endpoint"].get<std::string>();
    }

    std::optional<std::string> client_id =
        config.client_id.has_value() ? config.client_id : client_id_of(store);
    std::string redirect_uri = resolve_redirect_uri(config, store);

    // POST grant_type=authorization_code（PKCE code_verifier + redirect_uri + client_id）。
    std::string body = "grant_type=authorization_code";
    body += "&code=" + form_url_encode(authorization_code);
    body += "&redirect_uri=" + form_url_encode(redirect_uri);
    body += "&code_verifier=" + form_url_encode(code_verifier);
    if (client_id.has_value()) body += "&client_id=" + form_url_encode(*client_id);

    infra::HttpRequestOptions opts;
    opts.headers["Content-Type"] = "application/x-www-form-urlencoded";
    opts.headers["Accept"] = "application/json";
    opts.body = body;
    opts.timeout_ms = config.timeout_ms;

    auto resp = http->request("POST", *token_endpoint, opts);
    if (resp.is_err())
        return Ret::Err("MCP server \"" + config.server_name + "\" token exchange failed");
    const infra::HttpResponse& r = resp.get();
    if (!r.ok) {
        return Ret::Err("MCP server \"" + config.server_name + "\" token exchange failed (HTTP " +
                        std::to_string(r.status_code) + ")");
    }
    auto body_json = r.json();
    if (body_json.is_err()) return Ret::Err("token response is not valid JSON");

    auto next = apply_authorized_tokens(
        store, converters::JsonConverter::to_nlohmann(body_json.get()), now_ms());
    if (next.is_err()) return Ret::Err(next.error);

    // 回写 token endpoint 缓存（若刚 discovery）。
    McpOAuthStore result = next.get();
    if (token_endpoint.has_value() && auth_server.has_value()) {
        json ds = result.discovery_state.has_value() && result.discovery_state->is_object()
                      ? *result.discovery_state : json::object();
        ds["authorizationServerUrl"] = *auth_server;
        ds["tokenEndpoint"] = *token_endpoint;
        result.discovery_state = ds;
    }

    auto written = update_mcp_oauth_store(store_key, [&result](const McpOAuthStore&) { return result; });
    if (written.is_err()) return Ret::Err(written.error);
    return Ret::Ok(McpOAuthLoginResult::Authorized);
}

core::Result<McpOAuthLoginResult> run_mcp_oauth_login(
    const McpOAuthLoginConfig& config,
    const std::optional<std::string>& authorization_code,
    const std::function<void(const std::string&)>& on_authorization_url) {
    if (authorization_code.has_value() && !authorization_code->empty()) {
        return complete_mcp_oauth_login(config, *authorization_code);
    }
    return begin_mcp_oauth_login(config, on_authorization_url);
}

} // namespace mcp
} // namespace hunterclaw
