#include "hunterclaw/mcp/mcp_oauth_token_exchange.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/mcp/json_converter.h"

#include <chrono>
#include <cstdio>

// 🎯 对齐 OpenClaw MCP SDK auth() 的 refresh_token 交换。

namespace hunterclaw {
namespace mcp {

namespace {
using json = nlohmann::json;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 读取 store.tokens.refresh_token。
std::optional<std::string> refresh_token_of(const McpOAuthStore& store) {
    if (!store.tokens.has_value() || !store.tokens->is_object()) return std::nullopt;
    const json& t = *store.tokens;
    if (t.contains("refresh_token") && t["refresh_token"].is_string()) {
        std::string rt = t["refresh_token"].get<std::string>();
        if (!rt.empty()) return rt;
    }
    return std::nullopt;
}

// 读取 store.clientInformation.client_id。
std::optional<std::string> client_id_of(const McpOAuthStore& store) {
    if (!store.client_information.has_value() || !store.client_information->is_object())
        return std::nullopt;
    const json& c = *store.client_information;
    if (c.contains("client_id") && c["client_id"].is_string()) {
        return c["client_id"].get<std::string>();
    }
    return std::nullopt;
}

// 从 store.discoveryState 取授权服务器基址。
std::optional<std::string> auth_server_from_store(const McpOAuthStore& store) {
    if (!store.discovery_state.has_value() || !store.discovery_state->is_object())
        return std::nullopt;
    const json& d = *store.discovery_state;
    if (d.contains("authorizationServerUrl") && d["authorizationServerUrl"].is_string()) {
        return d["authorizationServerUrl"].get<std::string>();
    }
    return std::nullopt;
}

// 从 store.discoveryState 取已缓存的 token_endpoint。
std::optional<std::string> token_endpoint_from_store(const McpOAuthStore& store) {
    if (!store.discovery_state.has_value() || !store.discovery_state->is_object())
        return std::nullopt;
    const json& d = *store.discovery_state;
    if (d.contains("tokenEndpoint") && d["tokenEndpoint"].is_string()) {
        return d["tokenEndpoint"].get<std::string>();
    }
    // 部分实现把 SDK metadata 存于 discoveryState.metadata.token_endpoint
    if (d.contains("metadata") && d["metadata"].is_object()) {
        const json& m = d["metadata"];
        if (m.contains("token_endpoint") && m["token_endpoint"].is_string()) {
            return m["token_endpoint"].get<std::string>();
        }
    }
    return std::nullopt;
}

// 拼接 well-known metadata URL（RFC 8414）。
std::string well_known_metadata_url(const std::string& auth_server_url) {
    std::string base = auth_server_url;
    // 去尾部 '/'
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/.well-known/oauth-authorization-server";
}

} // namespace

std::string form_url_encode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        // RFC 3986 unreserved: A-Z a-z 0-9 - _ . ~
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

core::Result<McpOAuthStore> apply_refreshed_tokens(
    const McpOAuthStore& current, const nlohmann::json& token_response, int64_t now_ms_val) {
    using Ret = core::Result<McpOAuthStore>;
    if (!token_response.is_object() || !token_response.contains("access_token") ||
        !token_response["access_token"].is_string()) {
        return Ret::Err("token response missing access_token");
    }

    McpOAuthStore next = current;

    // 对齐 saveTokens: tokens =响应；缺 refresh_token 时保留旧 refresh_token（多数
    //   授权服务器 refresh 响应不回传 refresh_token →沿用旧值，符合 RFC 6749 §6）。
    json new_tokens = token_response;
    if (!new_tokens.contains("refresh_token") || !new_tokens["refresh_token"].is_string()) {
        auto old_rt = refresh_token_of(current);
        if (old_rt.has_value()) new_tokens["refresh_token"] = *old_rt;
    }
    next.tokens = new_tokens;

    // 对齐: delete credentialState; delete pendingAuthorizationChallenge。
    next.credential_state = std::nullopt;
    next.pending_authorization_challenge = std::nullopt;

    // 对齐 resolveTokenExpiresAt: expires_in(number) → now + expires_in*1000；否则删。
    if (token_response.contains("expires_in") && token_response["expires_in"].is_number()) {
        double expires_in = token_response["expires_in"].get<double>();
        next.token_expires_at = static_cast<double>(now_ms_val) + expires_in * 1000.0;
    } else {
        next.token_expires_at = std::nullopt;
    }

    // 写前校验（对齐 replaceMcpOAuthStore 的 parse）。
    json validated_value = next.to_json();
    auto validated = parse_mcp_oauth_store("<refresh>", validated_value);
    if (validated.is_err()) {
        return Ret::Err(validated.error);
    }
    return Ret::Ok(std::move(next));
}

core::Result<McpOAuthStore> perform_mcp_oauth_token_refresh(
    const McpOAuthStore& current, const McpOAuthRefreshConfig& config) {
    using Ret = core::Result<McpOAuthStore>;

    auto refresh_token = refresh_token_of(current);
    if (!refresh_token.has_value()) {
        return Ret::Err(mcp_oauth_refresh_failed_error(config.server_name));
    }

    std::shared_ptr<infra::HttpClient> http = config.http;
    if (!http) http = infra::HttpClient::create();

    // 1. 解析 token endpoint：显式 config > store缓存 > discovery拉取。
    std::optional<std::string> token_endpoint = config.token_endpoint;
    if (!token_endpoint.has_value()) {
        token_endpoint = token_endpoint_from_store(current);
    }
    if (!token_endpoint.has_value()) {
        std::optional<std::string> auth_server =
            config.authorization_server_url.has_value()
                ? config.authorization_server_url
                : auth_server_from_store(current);
        if (!auth_server.has_value()) {
            return Ret::Err("cannot resolve token endpoint: no authorization server url");
        }
        // RFC 8414 metadata discovery。
        auto md = http->get(well_known_metadata_url(*auth_server), config.timeout_ms);
        if (md.is_err() || !md.get().ok) {
            return Ret::Err("authorization server metadata discovery failed");
        }
        auto md_json = md.get().json();
        if (md_json.is_err()) {
            return Ret::Err("authorization server metadata is not valid JSON");
        }
        json meta = converters::JsonConverter::to_nlohmann(md_json.get());
        if (!meta.contains("token_endpoint") || !meta["token_endpoint"].is_string()) {
            return Ret::Err("authorization server metadata missing token_endpoint");
        }
        token_endpoint = meta["token_endpoint"].get<std::string>();
    }

    // 2. 构造 refresh_token grant 请求体（application/x-www-form-urlencoded）。
    std::string body = "grant_type=refresh_token";
    body += "&refresh_token=" + form_url_encode(*refresh_token);
    // token_endpoint_auth_method="none" → client_id 入 body（若有）。
    auto client_id = client_id_of(current);
    if (client_id.has_value()) {
        body += "&client_id=" + form_url_encode(*client_id);
    }

    infra::HttpRequestOptions opts;
    opts.headers["Content-Type"] = "application/x-www-form-urlencoded";
    opts.headers["Accept"] = "application/json";
    opts.body = body;
    opts.timeout_ms = config.timeout_ms;

    auto resp = http->request("POST", *token_endpoint, opts);
    if (resp.is_err()) {
        return Ret::Err(mcp_oauth_refresh_failed_error(config.server_name));
    }
    const infra::HttpResponse& r = resp.get();
    if (!r.ok) {
        return Ret::Err(mcp_oauth_refresh_failed_error(config.server_name) +
                        " (HTTP " + std::to_string(r.status_code) + ")");
    }
    auto body_json = r.json();
    if (body_json.is_err()) {
        return Ret::Err("token response is not valid JSON");
    }

    // 3. 应用刷新结果。
    auto next = apply_refreshed_tokens(
        current, converters::JsonConverter::to_nlohmann(body_json.get()), now_ms());
    if (next.is_err()) {
        return Ret::Err(next.error);
    }
    // discovery 若刚拉取，回写 tokenEndpoint 缓存（便于下次直用）。
    McpOAuthStore result = next.get();
    if (token_endpoint.has_value()) {
        json ds = result.discovery_state.has_value() && result.discovery_state->is_object()
                      ? *result.discovery_state
                      : json::object();
        // 仅补充缺失的 authorizationServerUrl以满足 store 校验（discoveryState 需含它）。
        if (!ds.contains("authorizationServerUrl")) {
            std::optional<std::string> auth_server =
                config.authorization_server_url.has_value()
                    ? config.authorization_server_url
                    : auth_server_from_store(current);
            if (auth_server.has_value()) {
                ds["authorizationServerUrl"] = *auth_server;
            }
        }
        if (ds.contains("authorizationServerUrl")) {
            ds["tokenEndpoint"] = *token_endpoint;
            result.discovery_state = ds;
        }
    }
    return Ret::Ok(std::move(result));
}

TokenRefreshFn make_mcp_oauth_token_refresh_fn(const McpOAuthRefreshConfig& config) {
    return [config](const McpOAuthStore& current) -> core::Result<McpOAuthStore> {
        return perform_mcp_oauth_token_refresh(current, config);
    };
}

} // namespace mcp
} // namespace hunterclaw
