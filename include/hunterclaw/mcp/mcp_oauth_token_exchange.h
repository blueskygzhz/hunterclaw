#pragma once

// 🎯 对齐 OpenClaw MCP SDK auth() 的 refresh_token 交换（RFC 6749 §6 + RFC 8414）：
//   实现一个可作为 TokenRefreshFn 的 HTTP token刷新器：
//     1. 发现 token endpoint（discoveryState.authorizationServerUrl 或
//        {authServer}/.well-known/oauth-authorization-server的 token_endpoint）；
//     2. POST grant_type=refresh_token（token_endpoint_auth_method="none" →
//        client_id 入 body），Content-Type: application/x-www-form-urlencoded；
//     3. 解析 access_token/refresh_token/expires_in → 新 McpOAuthStore
//        （对齐 provider.saveTokens：写 tokens+tokenExpiresAt，删 credentialState/
//         pendingAuthorizationChallenge，缺 expires_in 则不写 tokenExpiresAt）。

#include "hunterclaw/core/result.h"
#include "hunterclaw/mcp/mcp_oauth.h"
#include "hunterclaw/mcp/mcp_oauth_store.h"
#include <memory>
#include <optional>
#include <string>

namespace hunterclaw {
namespace infra { class HttpClient; }

namespace mcp {

// 刷新器配置。
struct McpOAuthRefreshConfig {
    std::string server_name;
    // 授权服务器基址（用于 metadata discovery）；缺省时从 store.discoveryState 取。
    std::optional<std::string> authorization_server_url;
    // 显式 token endpoint（优先于 discovery）。
    std::optional<std::string> token_endpoint;
    int timeout_ms = 15000;
    // 注入 HttpClient（便于测试/复用）；为空则内部 create()。
    std::shared_ptr<infra::HttpClient> http;
};

// 执行一次 refresh_token 交换，返回刷新后的 store（对齐 saveTokens 语义）。
core::Result<McpOAuthStore> perform_mcp_oauth_token_refresh(
    const McpOAuthStore& current, const McpOAuthRefreshConfig& config);

// 便捷工厂：产出绑定 config 的 TokenRefreshFn（供 resolve_mcp_oauth_access_token）。
TokenRefreshFn make_mcp_oauth_token_refresh_fn(const McpOAuthRefreshConfig& config);

// —— 以下为可独立测试的纯函数 ——

// application/x-www-form-urlencoded 编码单个值（RFC 3986 unreserved 之外均转义）。
std::string form_url_encode(const std::string& value);

// 从 token 响应 JSON 构造刷新后的 store（对齐 saveTokens + resolveTokenExpiresAt）。
//   now_ms 用于 expires_in → tokenExpiresAt 计算。
core::Result<McpOAuthStore> apply_refreshed_tokens(
    const McpOAuthStore& current, const nlohmann::json& token_response, int64_t now_ms);

} // namespace mcp
} // namespace hunterclaw
