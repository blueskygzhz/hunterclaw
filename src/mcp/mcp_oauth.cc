#include "hunterclaw/mcp/mcp_oauth.h"
#include "hunterclaw/mcp/mcp_oauth_persistence.h"

#include <algorithm>
#include <cctype>
#include <chrono>

// 🎯 对齐 OpenClaw src/agents/mcp-oauth.ts + mcp-oauth-fetch.ts 的决策纯算法。

namespace hunterclaw {
namespace mcp {

namespace {
using json = nlohmann::json;

// 读取 store.tokens.access_token（不存在→nullopt）。
std::optional<std::string> access_token_of(const McpOAuthStore& store) {
    if (!store.tokens.has_value() || !store.tokens->is_object()) return std::nullopt;
    const json& t = *store.tokens;
    if (t.contains("access_token") && t["access_token"].is_string()) {
        return t["access_token"].get<std::string>();
    }
    return std::nullopt;
}

// store.tokens.refresh_token 是否存在且非空。
bool has_refresh_token(const McpOAuthStore& store) {
    if (!store.tokens.has_value() || !store.tokens->is_object()) return false;
    const json& t = *store.tokens;
    return t.contains("refresh_token") && t["refresh_token"].is_string() &&
           !t["refresh_token"].get<std::string>().empty();
}

// pendingAuthorizationChallenge.requiresAuthorization === true。
bool pending_requires_authorization(const McpOAuthStore& store) {
    if (!store.pending_authorization_challenge.has_value()) return false;
    const json& c = *store.pending_authorization_challenge;
    return c.is_object() && c.contains("requiresAuthorization") &&
           c["requiresAuthorization"].is_boolean() && c["requiresAuthorization"].get<bool>();
}

// normalizeOptionalString: trim 后非空则返回，否则 nullopt。
std::optional<std::string> normalize_optional_string(const std::optional<std::string>& v) {
    if (!v.has_value()) return std::nullopt;
    size_t b = v->find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::nullopt;
    size_t e = v->find_last_not_of(" \t\r\n");
    return v->substr(b, e - b + 1);
}

// 提取 URL 的 origin（scheme://host[:port]）。失败返回原串（对齐"无法解析即整串比较"）。
std::string origin_of(const std::string& url) {
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;
    size_t host_start = scheme_end + 3;
    size_t path_start = url.find('/', host_start);
    std::string origin =
        (path_start == std::string::npos) ? url : url.substr(0, path_start);
    // origin 大小写：scheme+host 忽略大小写，此处按OC URL.origin 语义小写化。
    for (char& c : origin) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return origin;
}

} // namespace

std::string mcp_oauth_requires_authorization_error(const std::string& server_name) {
    return "MCP server \"" + server_name +
           "\" requires OAuth authorization. Run openclaw mcp login " + server_name + ".";
}
std::string mcp_oauth_additional_authorization_error(const std::string& server_name) {
    return "MCP server \"" + server_name +
           "\" requires additional OAuth authorization. Run openclaw mcp login " +
           server_name + ".";
}
std::string mcp_oauth_expired_error(const std::string& server_name) {
    return "MCP server \"" + server_name +
           "\" has expired OAuth credentials. Run openclaw mcp login " + server_name + ".";
}
std::string mcp_oauth_refresh_failed_error(const std::string& server_name) {
    return "MCP server \"" + server_name +
           "\" could not refresh OAuth credentials. Run openclaw mcp login " +
           server_name + ".";
}

McpOAuthTokenDecision decide_mcp_oauth_token(
    const McpOAuthTokenDecisionInput& input,
    const std::optional<std::string>& challenge_resource_metadata_url,
    const std::optional<std::string>& challenge_scope) {
    const McpOAuthStore& store = input.store;
    std::optional<std::string> access = access_token_of(store);

    // rejectedCurrentToken = params.rejectedAccessToken === tokens?.access_token
    bool rejected_current_token =
        input.rejected_access_token.has_value() && access.has_value() &&
        *input.rejected_access_token == *access;
    // challengeAppliesToCurrentState = !tokens?.access_token || rejectedCurrentToken
    bool challenge_applies = !access.has_value() || rejected_current_token;

    McpOAuthTokenDecision decision;

    // (1) authorizationChallenge && challengeAppliesToCurrentState → 可能写challenge。
    if (input.authorization_challenge && challenge_applies) {
        std::optional<std::string> rmu = challenge_resource_metadata_url;  // 已是 string化
        std::optional<std::string> scope = normalize_optional_string(challenge_scope);
        if (rmu.has_value() || scope.has_value() || input.interactive_authorization_required) {
            McpOAuthChallengeParams cp;
            cp.resource_metadata_url = rmu;
            cp.scope = scope;
            cp.requires_authorization = input.interactive_authorization_required;
            decision.apply_challenge = cp;
        }
    }

    // (2) authorizationChallenge && interactive && challengeApplies → additional auth error。
    if (input.authorization_challenge && input.interactive_authorization_required &&
        challenge_applies) {
        decision.action = McpOAuthTokenAction::ErrorAdditionalAuth;
        decision.error_message = mcp_oauth_additional_authorization_error(input.server_name);
        return decision;
    }

    // (3) pending.requiresAuthorization → additional auth error。
    if (pending_requires_authorization(store)) {
        decision.action = McpOAuthTokenAction::ErrorAdditionalAuth;
        decision.error_message = mcp_oauth_additional_authorization_error(input.server_name);
        return decision;
    }

    // (4) 无 access_token。
    if (!access.has_value()) {
        if (input.allow_missing_token) {
            decision.action = McpOAuthTokenAction::ReturnMissing;
            return decision;
        }
        decision.action = McpOAuthTokenAction::ErrorRequiresAuth;
        decision.error_message = mcp_oauth_requires_authorization_error(input.server_name);
        return decision;
    }

    // (5) 新鲜度判定。
    bool token_is_fresh =
        store.token_expires_at.has_value() &&
        static_cast<int64_t>(*store.token_expires_at) >
            input.now_ms + MCP_OAUTH_TOKEN_EXPIRY_SKEW_MS;
    bool unknown_expiry_acceptable =
        !store.token_expires_at.has_value() &&
        (input.accept_unknown_expiry || !has_refresh_token(store));
    if (!rejected_current_token && (token_is_fresh || unknown_expiry_acceptable)) {
        decision.action = McpOAuthTokenAction::ReturnToken;
        decision.access_token = access;
        return decision;
    }

    // (6) 需刷新但无 refresh_token → 过期错误。
    if (!has_refresh_token(store)) {
        decision.action = McpOAuthTokenAction::ErrorExpiredNoRefresh;
        decision.error_message = mcp_oauth_expired_error(input.server_name);
        return decision;
    }

    // (7) 触发 refresh（调用方走 auth()；失败时用 refresh_failed_error）。
    decision.action = McpOAuthTokenAction::Refresh;
    return decision;
}

bool mcp_oauth_should_retry(int status, const WwwAuthenticateChallenge& challenge) {
    if (status == 401) return true;
    if (status == 403 && challenge.error.has_value() &&
        *challenge.error == "insufficient_scope") {
        return true;
    }
    return false;
}

namespace {
// 从 auth-param 列表里取某个键的值（形如 key="value" 或 key=value）。
std::optional<std::string> extract_auth_param(const std::string& params, const std::string& key) {
    // 大小写不敏感查找 key= 。
    std::string lower_params = params;
    std::string lower_key = key;
    for (char& c : lower_params) c = static_cast<char>(std::tolower((unsigned char)c));
    for (char& c : lower_key) c = static_cast<char>(std::tolower((unsigned char)c));
    size_t search_from = 0;
    while (true) {
        size_t pos = lower_params.find(lower_key, search_from);
        if (pos == std::string::npos) return std::nullopt;
        // 要求 key 前是边界（起始/空白/逗号），后接可选空白+'='。
        bool boundary_ok = (pos == 0) || lower_params[pos - 1] == ' ' ||
                           lower_params[pos - 1] == ',' || lower_params[pos - 1] == '\t';
        size_t after = pos + lower_key.size();
        while (after < params.size() && (params[after] == ' ' || params[after] == '\t')) ++after;
        if (boundary_ok && after < params.size() && params[after] == '=') {
            size_t val_start = after + 1;
            while (val_start < params.size() &&
                   (params[val_start] == ' ' || params[val_start] == '\t')) ++val_start;
            if (val_start >= params.size()) return std::string("");
            if (params[val_start] == '"') {
                // 引号值：到下一个未转义引号。
                size_t end = params.find('"', val_start + 1);
                if (end == std::string::npos) return std::nullopt;
                return params.substr(val_start + 1, end - val_start - 1);
            }
            // 裸值：到逗号或空白。
            size_t end = val_start;
            while (end < params.size() && params[end] != ',' && params[end] != ' ' &&
                   params[end] != '\t') ++end;
            return params.substr(val_start, end - val_start);
        }
        search_from = pos + lower_key.size();
    }
}
} // namespace

WwwAuthenticateChallenge parse_www_authenticate(const std::string& header_value) {
    WwwAuthenticateChallenge challenge;
    // 找到 "Bearer" scheme 之后的 auth-param 部分。
    std::string lower = header_value;
    for (char& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
    size_t bearer = lower.find("bearer");
    if (bearer == std::string::npos) {
        return challenge;  // 非 Bearer → 全空
    }
    std::string params = header_value.substr(bearer + 6);  // "bearer" 长度 6

    challenge.error = extract_auth_param(params, "error");
    // SDK 用 resource_metadata 承载 metadata URL。
    challenge.resource_metadata_url = extract_auth_param(params, "resource_metadata");
    challenge.scope = extract_auth_param(params, "scope");
    return challenge;
}

bool mcp_oauth_same_origin(const std::string& request_url, const std::string& resource_url) {
    return origin_of(request_url) == origin_of(resource_url);
}

namespace {
int64_t orchestration_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

core::Result<std::optional<std::string>> resolve_mcp_oauth_access_token(
    const ResolveMcpOAuthAccessTokenParams& params) {
    using Ret = core::Result<std::optional<std::string>>;
    const std::string store_key =
        resolve_mcp_oauth_store_key(params.server_name, params.server_url);

    // 读取当前 store（对齐 readMcpOAuthStore）。
    auto store_res = read_mcp_oauth_store(store_key);
    if (store_res.is_err()) return Ret::Err(store_res.error);
    McpOAuthStore store = store_res.get();

    // 构造决策输入。
    McpOAuthTokenDecisionInput in;
    in.server_name = params.server_name;
    in.store = store;
    in.rejected_access_token = params.rejected_access_token;
    in.accept_unknown_expiry = params.accept_unknown_expiry;
    in.allow_missing_token = params.allow_missing_token;
    in.authorization_challenge = params.authorization_challenge;
    in.interactive_authorization_required = params.interactive_authorization_required;
    in.now_ms = orchestration_now_ms();

    McpOAuthTokenDecision decision = decide_mcp_oauth_token(
        in, params.challenge_resource_metadata_url, params.challenge_scope);

    // 若决策要求应用 challenge，先原子写入 store（对齐 OC 在抛错/返回前 update）。
    if (decision.apply_challenge.has_value()) {
        McpOAuthChallengeParams cp = *decision.apply_challenge;
        auto updated = update_mcp_oauth_store(
            store_key, [&cp](const McpOAuthStore& cur) {
                return apply_mcp_oauth_authorization_challenge(cur, cp);
            });
        if (updated.is_err()) return Ret::Err(updated.error);
        store = updated.get();
    }

    switch (decision.action) {
        case McpOAuthTokenAction::ReturnToken:
            return Ret::Ok(decision.access_token);
        case McpOAuthTokenAction::ReturnMissing:
            return Ret::Ok(std::nullopt);
        case McpOAuthTokenAction::ErrorRequiresAuth:
        case McpOAuthTokenAction::ErrorAdditionalAuth:
        case McpOAuthTokenAction::ErrorExpiredNoRefresh:
            return Ret::Err(decision.error_message);
        case McpOAuthTokenAction::Refresh: {
            // 对齐 auth() 刷新：注入式回调；无回调则等价刷新失败。
            if (!params.refresh) {
                return Ret::Err(mcp_oauth_refresh_failed_error(params.server_name));
            }
            auto refreshed = params.refresh(store);
            if (refreshed.is_err()) {
                return Ret::Err(mcp_oauth_refresh_failed_error(params.server_name));
            }
            McpOAuthStore next = refreshed.get();
            // 持久化刷新结果（对齐 auth() 后 store 写回）。
            auto written = update_mcp_oauth_store(
                store_key, [&next](const McpOAuthStore&) { return next; });
            if (written.is_err()) return Ret::Err(written.error);
            // 返回新 access_token；若刷新后仍无 token → 视为刷新失败。
            McpOAuthStore final_store = written.get();
            if (!final_store.tokens.has_value() || !final_store.tokens->is_object()) {
                return Ret::Err(mcp_oauth_refresh_failed_error(params.server_name));
            }
            const json& t = *final_store.tokens;
            if (t.contains("access_token") && t["access_token"].is_string()) {
                return Ret::Ok(std::optional<std::string>(t["access_token"].get<std::string>()));
            }
            return Ret::Err(mcp_oauth_refresh_failed_error(params.server_name));
        }
    }
    return Ret::Err(mcp_oauth_refresh_failed_error(params.server_name));
}

core::Result<void> record_mcp_oauth_authorization_required(
    const std::string& server_name, const std::string& server_url,
    const McpOAuthChallengeParams& challenge) {
    const std::string store_key = resolve_mcp_oauth_store_key(server_name, server_url);
    auto updated = update_mcp_oauth_store(
        store_key, [&challenge](const McpOAuthStore& cur) {
            return apply_mcp_oauth_authorization_challenge(cur, challenge);
        });
    if (updated.is_err()) return core::Result<void>::Err(updated.error);
    return core::Result<void>::Ok();
}

core::Result<void> clear_mcp_oauth_credentials(
    const std::string& server_name, const std::string& server_url) {
    const std::string store_key = resolve_mcp_oauth_store_key(server_name, server_url);
    return clear_mcp_oauth_store(store_key);
}

} // namespace mcp
} // namespace hunterclaw
