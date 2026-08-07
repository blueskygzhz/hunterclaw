#include "hunterclaw/mcp/mcp_oauth_store.h"
#include "hunterclaw/mcp/bundle_mcp_types.h"
#include "hunterclaw/infra/utilities.h"

#include <set>

// 🎯 对齐 OpenClaw src/agents/mcp-oauth-store.ts + mcp-oauth.ts（可移植纯算法核心）。

namespace hunterclaw {
namespace mcp {

namespace {
using json = nlohmann::json;

// 对齐 UNINITIALIZED_STORE_FIELDS。
const std::set<std::string>& uninitialized_store_fields() {
    static const std::set<std::string> f = {"credentialState", "pendingAuthorizationChallenge"};
    return f;
}

bool is_nonempty_string(const json& v) {
    return v.is_string() && !v.get<std::string>().empty();
}

// URL.canParse 的近似：非空且含 "://" 或以已知 scheme 开头。够用于损坏判定。
bool url_can_parse(const json& v) {
    if (!v.is_string()) return false;
    const std::string& s = v.get<std::string>();
    if (s.empty()) return false;
    return s.find("://") != std::string::npos;
}

template <typename T>
void assign_if(json& obj, const char* key, const std::optional<T>& val) {
    if (val.has_value()) obj[key] = *val;
}
void assign_json_if(json& obj, const char* key, const std::optional<json>& val) {
    if (val.has_value()) obj[key] = *val;
}

} // namespace

nlohmann::json McpOAuthStore::to_json() const {
    json obj = json::object();
    assign_if(obj, "credentialState", credential_state);
    assign_json_if(obj, "clientInformation", client_information);
    assign_json_if(obj, "tokens", tokens);
    assign_if(obj, "tokenExpiresAt", token_expires_at);
    assign_if(obj, "codeVerifier", code_verifier);
    assign_json_if(obj, "discoveryState", discovery_state);
    assign_if(obj, "lastAuthorizationUrl", last_authorization_url);
    assign_if(obj, "redirectUrl", redirect_url);
    assign_json_if(obj, "pendingAuthorizationChallenge", pending_authorization_challenge);
    return obj;
}

McpOAuthCredentialsStatus mcp_oauth_credentials_status(const McpOAuthStore& store) {
    McpOAuthCredentialsStatus s;
    s.has_tokens = store.tokens.has_value();
    // requiresAuthorization: pendingAuthorizationChallenge?.requiresAuthorization === true
    if (store.pending_authorization_challenge.has_value()) {
        const json& c = *store.pending_authorization_challenge;
        s.requires_authorization =
            c.is_object() && c.contains("requiresAuthorization") &&
            c["requiresAuthorization"].is_boolean() && c["requiresAuthorization"].get<bool>();
    }
    s.has_client_information = store.client_information.has_value();
    s.has_code_verifier = store.code_verifier.has_value();
    s.has_discovery_state = store.discovery_state.has_value();
    s.has_last_authorization_url = store.last_authorization_url.has_value();
    return s;
}

core::Result<McpOAuthStore> parse_mcp_oauth_store(
    const std::string& store_key, const nlohmann::json& value) {
    using Ret = core::Result<McpOAuthStore>;
    auto corrupt = [&](const std::string& msg) {
        return Ret::Err("mcp-oauth store \"" + store_key + "\" corrupt: " + msg);
    };

    if (!value.is_object()) {
        return corrupt("store_json must contain an object");
    }
    // clientInformation / tokens 结构此处不做 SDK schema 深校验（宿主 SDK 负责），
    // 仅做与 OC 一致的关系/取值/类型校验。
    // credentialState 取值。
    if (value.contains("credentialState")) {
        const json& cs = value["credentialState"];
        if (!(cs.is_string() &&
              (cs.get<std::string>() == "uninitialized" || cs.get<std::string>() == "cleared"))) {
            return corrupt("credentialState is invalid");
        }
    }
    // credentialState 与 tokens 互斥。
    if (value.contains("credentialState") && value.contains("tokens")) {
        return corrupt("credentialState cannot coexist with tokens");
    }
    // uninitialized 时字段白名单。
    if (value.contains("credentialState") &&
        value["credentialState"].is_string() &&
        value["credentialState"].get<std::string>() == "uninitialized") {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!uninitialized_store_fields().count(it.key())) {
                return corrupt("uninitialized store must not contain \"" + it.key() + "\"");
            }
        }
    }
    // tokenExpiresAt 依赖 tokens。
    if (value.contains("tokenExpiresAt") && !value.contains("tokens")) {
        return corrupt("tokenExpiresAt requires tokens");
    }
    // 可选字符串非空。
    for (const char* field : {"codeVerifier", "lastAuthorizationUrl", "redirectUrl"}) {
        if (value.contains(field)) {
            const json& v = value[field];
            if (!is_nonempty_string(v)) {
                return corrupt(std::string(field) + " must be a non-empty string");
            }
        }
    }
    // discoveryState 结构。
    if (value.contains("discoveryState")) {
        const json& ds = value["discoveryState"];
        if (!ds.is_object() || !ds.contains("authorizationServerUrl") ||
            !ds["authorizationServerUrl"].is_string()) {
            return corrupt("discoveryState is invalid");
        }
        if (!url_can_parse(ds["authorizationServerUrl"])) {
            return corrupt("discoveryState URLs are invalid");
        }
        if (ds.contains("resourceMetadataUrl") &&
            !url_can_parse(ds["resourceMetadataUrl"])) {
            return corrupt("discoveryState URLs are invalid");
        }
    }
    // pendingAuthorizationChallenge 结构。
    if (value.contains("pendingAuthorizationChallenge")) {
        const json& c = value["pendingAuthorizationChallenge"];
        if (!c.is_object()) {
            return corrupt("pendingAuthorizationChallenge is invalid");
        }
        if (c.contains("resourceMetadataUrl") && !url_can_parse(c["resourceMetadataUrl"])) {
            return corrupt("pendingAuthorizationChallenge URL is invalid");
        }
        if (c.contains("scope") && !is_nonempty_string(c["scope"])) {
            return corrupt("pendingAuthorizationChallenge scope is invalid");
        }
        if (c.contains("requiresAuthorization") &&
            !(c["requiresAuthorization"].is_boolean() &&
              c["requiresAuthorization"].get<bool>())) {
            return corrupt("pendingAuthorizationChallenge requiresAuthorization must be true");
        }
    }

    // 通过校验 → 填充结构体。
    McpOAuthStore store;
    if (value.contains("credentialState"))
        store.credential_state = value["credentialState"].get<std::string>();
    if (value.contains("clientInformation")) store.client_information = value["clientInformation"];
    if (value.contains("tokens")) store.tokens = value["tokens"];
    if (value.contains("tokenExpiresAt") && value["tokenExpiresAt"].is_number())
        store.token_expires_at = value["tokenExpiresAt"].get<double>();
    if (value.contains("codeVerifier"))
        store.code_verifier = value["codeVerifier"].get<std::string>();
    if (value.contains("discoveryState")) store.discovery_state = value["discoveryState"];
    if (value.contains("lastAuthorizationUrl"))
        store.last_authorization_url = value["lastAuthorizationUrl"].get<std::string>();
    if (value.contains("redirectUrl"))
        store.redirect_url = value["redirectUrl"].get<std::string>();
    if (value.contains("pendingAuthorizationChallenge"))
        store.pending_authorization_challenge = value["pendingAuthorizationChallenge"];
    return Ret::Ok(std::move(store));
}

McpOAuthStore apply_mcp_oauth_authorization_challenge(
    const McpOAuthStore& current, const McpOAuthChallengeParams& params) {
    McpOAuthStore next = current;

    // 合并 pendingAuthorizationChallenge（对齐 {...current, ...params}）。
    json challenge = current.pending_authorization_challenge.value_or(json::object());
    if (!challenge.is_object()) challenge = json::object();
    if (params.resource_metadata_url.has_value())
        challenge["resourceMetadataUrl"] = *params.resource_metadata_url;
    if (params.scope.has_value())
        challenge["scope"] = *params.scope;
    if (params.requires_authorization)
        challenge["requiresAuthorization"] = true;
    next.pending_authorization_challenge = challenge;

    // 对齐：当 current 完全空白时，credentialState 置 "uninitialized"。
    if (!current.credential_state.has_value() && !current.tokens.has_value() &&
        !current.client_information.has_value() && !current.code_verifier.has_value() &&
        !current.discovery_state.has_value() && !current.last_authorization_url.has_value() &&
        !current.redirect_url.has_value()) {
        next.credential_state = "uninitialized";
    }

    // 对齐：resourceMetadataUrl 变更时清除 discoveryState。
    if (params.resource_metadata_url.has_value()) {
        bool differs = true;
        if (current.discovery_state.has_value() &&
            current.discovery_state->is_object() &&
            current.discovery_state->contains("resourceMetadataUrl") &&
            (*current.discovery_state)["resourceMetadataUrl"].is_string()) {
            differs = (*current.discovery_state)["resourceMetadataUrl"].get<std::string>() !=
                      *params.resource_metadata_url;
        }
        if (differs) {
            next.discovery_state = std::nullopt;
        }
    }
    return next;
}

std::string resolve_mcp_oauth_store_key(
    const std::string& server_name, const std::string& server_url) {
    // 对齐 sanitizeServerName(serverName, new Set())
    std::set<std::string> reserved;
    std::string safe_server_name = sanitize_server_name(server_name, reserved);
    // 对齐 createHash("sha256").update(serverName).update("\0").update(serverUrl)
    std::string material = server_name;
    material.push_back('\0');
    material += server_url;
    std::string hash = infra::sha256_hash(material);
    // hash.slice(0, 16)
    std::string short_hash = hash.substr(0, 16);
    return safe_server_name + "-" + short_hash;
}

} // namespace mcp
} // namespace hunterclaw
