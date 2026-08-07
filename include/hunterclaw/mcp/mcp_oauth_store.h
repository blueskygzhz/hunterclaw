#pragma once

// 🎯 对齐 OpenClaw src/agents/mcp-oauth-store.ts + mcp-oauth.ts 的可移植纯算法核心：
//   OAuth 凭证 store 的数据契约、损坏校验、状态判定、authorization challenge 迁移。
//   （token 交换/refresh、sqlite 持久化、状态租约锁属于宿主基础设施，不在此移植。）

#include "hunterclaw/core/result.h"
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace mcp {

// 对齐 McpOAuthStore（以 json 承载 clientInformation/tokens/discoveryState 等
//   SDK 结构，保真且便于持久化）。
struct McpOAuthStore {
    // "uninitialized" | "cleared" | (absent)
    std::optional<std::string> credential_state;
    std::optional<nlohmann::json> client_information;
    std::optional<nlohmann::json> tokens;
    std::optional<double> token_expires_at;
    std::optional<std::string> code_verifier;
    std::optional<nlohmann::json> discovery_state;
    std::optional<std::string> last_authorization_url;
    std::optional<std::string> redirect_url;
    std::optional<nlohmann::json> pending_authorization_challenge;

    nlohmann::json to_json() const;
};

// 对齐 credentialsStatus (mcp-oauth.ts:279-287)。
struct McpOAuthCredentialsStatus {
    bool has_tokens = false;
    bool requires_authorization = false;
    bool has_client_information = false;
    bool has_code_verifier = false;
    bool has_discovery_state = false;
    bool has_last_authorization_url = false;
};

McpOAuthCredentialsStatus mcp_oauth_credentials_status(const McpOAuthStore& store);

// 对齐 parseMcpOAuthStore 的损坏校验（返回 Err 表示 store 损坏，附文案）。
//   校验规则逐条对齐：credentialState 取值、与 tokens 互斥、uninitialized 字段白名单、
//   tokenExpiresAt 依赖 tokens、可选字符串非空、discoveryState/challenge 结构。
core::Result<McpOAuthStore> parse_mcp_oauth_store(
    const std::string& store_key, const nlohmann::json& value);

// 对齐 applyMcpOAuthAuthorizationChallenge 的状态迁移（纯函数，返回新 store）。
struct McpOAuthChallengeParams {
    std::optional<std::string> resource_metadata_url;
    std::optional<std::string> scope;
    bool requires_authorization = false;
};
McpOAuthStore apply_mcp_oauth_authorization_challenge(
    const McpOAuthStore& current, const McpOAuthChallengeParams& params);

// 对齐 resolveMcpOAuthStoreKey (mcp-oauth-store.ts:197-201):
//   `${sanitizeServerName(serverName)}-${sha256(serverName + "\0" + serverUrl)[:16]}`
std::string resolve_mcp_oauth_store_key(
    const std::string& server_name, const std::string& server_url);

} // namespace mcp
} // namespace hunterclaw
