#pragma once

// 🎯 对齐 OpenClaw src/agents/mcp-oauth.ts + mcp-oauth-fetch.ts 的决策纯算法。
//   （lease 跨进程锁、auth() SDK 网络刷新、store 持久化、fetch 分发属于宿主；
//    此处移植可独立编译验证、逐行对齐的"决策"逻辑：token 复用/刷新判定、
//    challenge 应用条件、401/403 重试判定、错误文案。）

#include "hunterclaw/mcp/mcp_oauth_store.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace hunterclaw {
namespace mcp {

// 对齐 TOKEN_EXPIRY_SKEW_MS。
constexpr int64_t MCP_OAUTH_TOKEN_EXPIRY_SKEW_MS = 30000;

// resolveMcpOAuthAccessToken 的输入（去除 fetch/signal/lease 等宿主项）。
struct McpOAuthTokenDecisionInput {
    std::string server_name;
    // 当前 store 快照。
    McpOAuthStore store;
    // 已被资源端拒绝的 access token（若与当前一致，则不复用）。
    std::optional<std::string> rejected_access_token;
    bool accept_unknown_expiry = false;
    bool allow_missing_token = false;
    bool authorization_challenge = false;
    bool interactive_authorization_required = false;
    // 当前时刻（epoch ms），便于测试注入。
    int64_t now_ms = 0;
};

// token 决策结果（对齐 resolveMcpOAuthAccessToken 的控制流分支）。
enum class McpOAuthTokenAction {
    ReturnToken,          // 直接复用现有 access_token（fresh 或可接受未知过期）
    ReturnMissing,        // 无 token 且 allowMissingToken → 返回 undefined
    Refresh,              // 需用 refresh_token 走 auth() 刷新
    ErrorRequiresAuth,    // 需重新授权（无 token / pending requiresAuthorization）
    ErrorAdditionalAuth,  // 需额外授权（交互式 challenge）
    ErrorExpiredNoRefresh,// 过期且无 refresh_token
};

struct McpOAuthTokenDecision {
    McpOAuthTokenAction action;
    //ReturnToken 时的 access_token；其它为空。
    std::optional<std::string> access_token;
    //需在决策前对 store 应用的 challenge（用于 authorizationChallenge 分支）。
    std::optional<McpOAuthChallengeParams> apply_challenge;
    // Error* 时的用户可读文案（对齐 OC 错误消息）。
    std::string error_message;
};

// 对齐 resolveMcpOAuthAccessToken 主体的纯决策（不做IO；调用方据此执行 store
//   写入 / auth() 刷新 / 抛错）。challenge 应用条件与抛错顺序严格对齐。
McpOAuthTokenDecision decide_mcp_oauth_token(
    const McpOAuthTokenDecisionInput& input,
    const std::optional<std::string>& challenge_resource_metadata_url = std::nullopt,
    const std::optional<std::string>& challenge_scope = std::nullopt);

// ============================================================================
// withMcpOAuthBearer 的重试决策（对齐 mcp-oauth-fetch.ts）。
// ============================================================================

// 对齐 extractWWWAuthenticateParams 的相关字段。
struct WwwAuthenticateChallenge {
    std::optional<std::string> error;                 // e.g. "insufficient_scope"
    std::optional<std::string> resource_metadata_url;
    std::optional<std::string> scope;
};

// 对齐 shouldRetry: status===401 || (status===403 && error==="insufficient_scope")。
bool mcp_oauth_should_retry(int status, const WwwAuthenticateChallenge& challenge);

// 对齐 SDK extractWWWAuthenticateParams: 解析 WWW-Authenticate: Bearer 挑战头，
//   提取 error / resource_metadata(→resource_metadata_url) / scope。
//   非 Bearer 或空 header → 全空。
WwwAuthenticateChallenge parse_www_authenticate(const std::string& header_value);

// 对齐 withMcpOAuthBearer 中"是否只对 resource origin 注入 Bearer"的判定：
//   request origin === resource origin。
bool mcp_oauth_same_origin(const std::string& request_url, const std::string& resource_url);

// 错误文案（对齐 OC）。
std::string mcp_oauth_requires_authorization_error(const std::string& server_name);
std::string mcp_oauth_additional_authorization_error(const std::string& server_name);
std::string mcp_oauth_expired_error(const std::string& server_name);
std::string mcp_oauth_refresh_failed_error(const std::string& server_name);

// ============================================================================
// 编排层（对齐 resolveMcpOAuthAccessToken / recordMcpOAuthAuthorizationRequired /
//   clearMcpOAuthCredentials）。串联决策 + 持久化 + 注入式 auth() 刷新。
// ============================================================================

// 注入式 token 刷新（对齐 SDK auth()）：给定当前 store，返回刷新后的 store
//   （含新 tokens/tokenExpiresAt/discoveryState）。失败以 Result::Err 返回。
//   宿主用 HTTP 打 token endpoint 实现；决策/持久化编排与之解耦。
using TokenRefreshFn =
    std::function<core::Result<McpOAuthStore>(const McpOAuthStore& current)>;

struct ResolveMcpOAuthAccessTokenParams {
    std::string server_name;
    std::string server_url;   // 用于 store key
    std::optional<std::string> rejected_access_token;
    bool accept_unknown_expiry = false;
    bool allow_missing_token = false;
    bool authorization_challenge = false;
    bool interactive_authorization_required = false;
    std::optional<std::string> challenge_resource_metadata_url;
    std::optional<std::string> challenge_scope;
    TokenRefreshFn refresh;   // 可空；为空时 Refresh 分支报refresh_failed
};

// 返回可用 access_token（allow_missing_token 且无 token 时为 nullopt）。
core::Result<std::optional<std::string>> resolve_mcp_oauth_access_token(
    const ResolveMcpOAuthAccessTokenParams& params);

// 对齐 recordMcpOAuthAuthorizationRequired: 记录 challenge（写 store）。
core::Result<void> record_mcp_oauth_authorization_required(
    const std::string& server_name, const std::string& server_url,
    const McpOAuthChallengeParams& challenge);

// 对齐 clearMcpOAuthCredentials: 清除凭证（写 {credentialState:"cleared"}）。
core::Result<void> clear_mcp_oauth_credentials(
    const std::string& server_name, const std::string& server_url);

} // namespace mcp
} // namespace hunterclaw
