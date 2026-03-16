#pragma once

#include <string>
#include <optional>
#include <cstdlib>

namespace hunterclaw {
namespace gateway {

/**
 * 🎯 100% 对齐 OpenClaw: Credential Helper
 * 
 * 从环境变量读取 Gateway 凭证
 * 
 * OpenClaw 参考: src/gateway/credential-planner.ts
 * 
 * 支持的环境变量:
 * - HUNTERCLAW_GATEWAY_TOKEN: Bearer Token
 * - HUNTERCLAW_GATEWAY_PASSWORD: Password (legacy)
 * - OPENCLAW_GATEWAY_TOKEN: 兼容 OpenClaw
 * - OPENCLAW_GATEWAY_PASSWORD: 兼容 OpenClaw
 */

/**
 * 读取 Gateway Token 环境变量
 * 
 * 优先级:
 * 1. HUNTERCLAW_GATEWAY_TOKEN
 * 2. OPENCLAW_GATEWAY_TOKEN (兼容性)
 * 
 * @return Token 值 (如果存在)
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export function readGatewayTokenEnv(
 *   env: NodeJS.ProcessEnv = process.env,
 *   includeLegacyEnv = true,
 * ): string | undefined {
 *   return readGatewayEnv(
 *     env,
 *     ["OPENCLAW_GATEWAY_TOKEN", "CLAWDBOT_GATEWAY_TOKEN"],
 *     includeLegacyEnv,
 *   );
 * }
 * ```
 */
inline std::optional<std::string> read_gateway_token_env(bool include_legacy = true) {
    // 优先: HUNTERCLAW_GATEWAY_TOKEN
    const char* token = std::getenv("HUNTERCLAW_GATEWAY_TOKEN");
    if (token && token[0] != '\0') {
        return std::string(token);
    }
    
    // 兼容: OPENCLAW_GATEWAY_TOKEN
    if (include_legacy) {
        token = std::getenv("OPENCLAW_GATEWAY_TOKEN");
        if (token && token[0] != '\0') {
            return std::string(token);
        }
        
        // 兼容: CLAWDBOT_GATEWAY_TOKEN (旧名称)
        token = std::getenv("CLAWDBOT_GATEWAY_TOKEN");
        if (token && token[0] != '\0') {
            return std::string(token);
        }
    }
    
    return std::nullopt;
}

/**
 * 读取 Gateway Password 环境变量
 * 
 * 优先级:
 * 1. HUNTERCLAW_GATEWAY_PASSWORD
 * 2. OPENCLAW_GATEWAY_PASSWORD (兼容性)
 * 
 * @return Password 值 (如果存在)
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export function readGatewayPasswordEnv(
 *   env: NodeJS.ProcessEnv = process.env,
 *   includeLegacyEnv = true,
 * ): string | undefined {
 *   return readGatewayEnv(
 *     env,
 *     ["OPENCLAW_GATEWAY_PASSWORD", "CLAWDBOT_GATEWAY_PASSWORD"],
 *     includeLegacyEnv,
 *   );
 * }
 * ```
 */
inline std::optional<std::string> read_gateway_password_env(bool include_legacy = true) {
    // 优先: HUNTERCLAW_GATEWAY_PASSWORD
    const char* password = std::getenv("HUNTERCLAW_GATEWAY_PASSWORD");
    if (password && password[0] != '\0') {
        return std::string(password);
    }
    
    // 兼容: OPENCLAW_GATEWAY_PASSWORD
    if (include_legacy) {
        password = std::getenv("OPENCLAW_GATEWAY_PASSWORD");
        if (password && password[0] != '\0') {
            return std::string(password);
        }
        
        // 兼容: CLAWDBOT_GATEWAY_PASSWORD (旧名称)
        password = std::getenv("CLAWDBOT_GATEWAY_PASSWORD");
        if (password && password[0] != '\0') {
            return std::string(password);
        }
    }
    
    return std::nullopt;
}

/**
 * 应用环境变量凭证到配置
 * 
 * 如果配置中的凭证为空,则从环境变量读取。
 * 
 * @param token 配置的 Token (in/out)
 * @param password 配置的 Password (in/out)
 * 
 * 使用示例:
 * ```cpp
 * std::string token = config->gateway.auth.bearer_token;
 * std::string password = config->gateway.auth.password;
 * apply_env_credentials(token, password);
 * 
 * if (!token.empty()) {
 *     // 使用环境变量或配置的 Token
 * }
 * ```
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export function createGatewayCredentialPlan(params: {...}): GatewayCredentialPlan {
 *   const envToken = readGatewayTokenEnv(env, includeLegacyEnv);
 *   const envPassword = readGatewayPasswordEnv(env, includeLegacyEnv);
 *   
 *   // 优先级: 环境变量 > 配置文件
 *   const token = envToken ?? config.gateway.auth.token;
 *   const password = envPassword ?? config.gateway.auth.password;
 *   
 *   return { token, password };
 * }
 * ```
 */
inline void apply_env_credentials(std::string& token, std::string& password) {
    // Token: 环境变量优先
    if (token.empty()) {
        auto env_token = read_gateway_token_env();
        if (env_token.has_value()) {
            token = env_token.value();
        }
    }
    
    // Password: 环境变量优先
    if (password.empty()) {
        auto env_password = read_gateway_password_env();
        if (env_password.has_value()) {
            password = env_password.value();
        }
    }
}

} // namespace gateway
} // namespace hunterclaw
