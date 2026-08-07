#pragma once

// 🎯 对齐 OpenClaw src/agents/mcp-oauth.ts (runMcpOAuthLogin / runMcpOAuthLoginAttempt)
//   + mcp-oauth-provider.ts (redirectToAuthorization / saveCodeVerifier / saveTokens)
//   + MCP SDK auth() 的 Authorization Code + PKCE(S256) 流程（RFC 6749 §4.1 / RFC 7636
//   / RFC 8414 metadata discovery）。
//
// 方案 B（无回调 server的两步式 CLI 登录，对齐 openclaw mcp login）：
//   第一步 `mcp login <name>`：
//     1. discovery: 拉 protected-resource / authorization-server metadata（RFC 8414/9728）；
//     2. 若无 client_information → 动态注册（RFC 7591，可选）或用配置 client_id；
//     3. 生成 PKCE(code_verifier + code_challenge=S256)，存codeVerifier；
//     4. 构造 authorization_url（response_type=code、code_challenge、redirect_uri、state、
//        scope），持久化 lastAuthorizationUrl，回调 on_authorization_url 打印给用户；
//     → 返回 Redirect（等待用户浏览器授权、把code 粘回来）。
//   第二步 `mcp login <name> --code <code>`：
//     1. 读回codeVerifier + client_information + tokenEndpoint；
//     2. POST grant_type=authorization_code（code + code_verifier + redirect_uri
//        + client_id）→ tokens；
//     3. saveTokens（写 tokens/tokenExpiresAt，删 codeVerifier/credentialState/
//        pendingAuthorizationChallenge）。
//     → 返回 Authorized。

#include "hunterclaw/core/result.h"
#include "hunterclaw/mcp/mcp_oauth_store.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace hunterclaw {
namespace infra { class HttpClient; }

namespace mcp {

// 对齐 McpOAuthConfig（server.oauth 配置片段）。
struct McpOAuthLoginConfig {
    std::string server_name;
    std::string server_url;         // MCP resource URL（用于 store key + resource metadata）
    std::optional<std::string> scope;
    // 显式 redirect_uri；缺省用 LEGACY_DEFAULT_REDIRECT_URL / store.redirectUrl。
    std::optional<std::string> redirect_url;
    // 显式授权服务器基址；缺省从 discovery / store 取。
    std::optional<std::string> authorization_server_url;
    // 显式 client_id（token_endpoint_auth_method="none"）；缺省从 store.clientInformation 取。
    std::optional<std::string> client_id;
    // 显式动态注册端点（RFC 7591）；缺省从 discovery metadata.registration_endpoint 取。
    std::optional<std::string> registration_endpoint;
    // client_name（动态注册用）；缺省 "HunterClaw MCP"。
    std::optional<std::string> client_name;
    int timeout_ms = 15000;
    // 注入 HttpClient（测试/复用）；空则内部 create()。
    std::shared_ptr<infra::HttpClient> http;
};

// 对齐 runMcpOAuthLogin 的返回："authorized" | "redirect"。
enum class McpOAuthLoginResult {
    Authorized,  // 已换到 tokens
    Redirect,    // 打印了授权 URL，等待用户回填 --code
};

// 第一步：生成并持久化 PKCE + authorization URL，回调打印 URL。
//   on_authorization_url 对齐 provider.redirectToAuthorization 的宿主副作用。
core::Result<McpOAuthLoginResult> begin_mcp_oauth_login(
    const McpOAuthLoginConfig& config,
    const std::function<void(const std::string& authorization_url)>& on_authorization_url);

// 第二步：用 authorization_code 换tokens 并 saveTokens。
core::Result<McpOAuthLoginResult> complete_mcp_oauth_login(
    const McpOAuthLoginConfig& config, const std::string& authorization_code);

// 统一入口（对齐 runMcpOAuthLogin）：有 code → complete；否则 begin。
core::Result<McpOAuthLoginResult> run_mcp_oauth_login(
    const McpOAuthLoginConfig& config,
    const std::optional<std::string>& authorization_code,
    const std::function<void(const std::string& authorization_url)>& on_authorization_url);

// ============================================================================
// 以下为可独立测试的纯函数（PKCE / URL 构造 / code 交换应用）。
// ============================================================================

// PKCE 材料（对齐 SDK generatePKCEChallenge）。
struct PkceChallenge {
    std::string code_verifier;   // 43~128 字符，[A-Za-z0-9-._~]
    std::string code_challenge;  // BASE64URL(SHA256(code_verifier))
    std::string method = "S256";
};

// 生成 PKCE（code_verifier 为 32 字节随机的 base64url，code_challenge=S256）。
PkceChallenge generate_pkce_challenge();

// base64url(no padding) 编码。
std::string base64url_encode_no_pad(const std::string& raw_bytes);

// SHA256 原始字节（32 字节）。
std::string sha256_raw(const std::string& data);

// 构造 authorization URL（response_type=code + PKCE + state + redirect_uri + scope）。
std::string build_authorization_url(
    const std::string& authorization_endpoint, const std::string& client_id,
    const std::string& redirect_uri, const std::string& code_challenge,
    const std::string& state, const std::optional<std::string>& scope);

// 对齐 RFC 7591 动态客户端注册请求体（SDK registerClient 的 client metadata）。
//   redirect_uris + token_endpoint_auth_method:"none" + grant_types + response_types + scope。
nlohmann::json build_dynamic_client_registration_body(
    const std::string& client_name, const std::string& redirect_uri,
    const std::optional<std::string>& scope);

// 从 authorization_code token响应构造 saveTokens 后的 store（对齐 provider.saveTokens）：
//   写 tokens/tokenExpiresAt，删 codeVerifier/credentialState/pendingAuthorizationChallenge。
core::Result<McpOAuthStore> apply_authorized_tokens(
    const McpOAuthStore& current, const nlohmann::json& token_response, int64_t now_ms);

} // namespace mcp
} // namespace hunterclaw
