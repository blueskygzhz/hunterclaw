#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/result.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/gateway/auth_rate_limiter.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;

// ============================================================================
// 🎯 100% 对齐 OpenClaw: Gateway Authentication
// ============================================================================

/**
 * 鉴权结果
 * 
 * OpenClaw 对齐:
 * - ok: 鉴权是否成功
 * - device_id: 设备标识
 * - scopes: 权限列表
 * - reason: 失败原因 (对齐 OpenClaw 错误码)
 * - method: 鉴权方法 (对齐 OpenClaw)
 * - user: 用户标识 (Trusted Proxy 模式)
 * - rate_limited: 是否因速率限制被拒绝
 * - retry_after_ms: 重试等待时间
 */
struct AuthResult {
    bool ok = false;
    std::string device_id;
    std::vector<std::string> scopes;
    std::string reason;  // "token_missing", "token_mismatch", "password_mismatch", "rate_limited", etc.
    std::string method;  // "none", "token", "password", "device-token", "tailscale", "trusted-proxy"
    std::string user;    // 用户标识 (Trusted Proxy 模式)
    bool rate_limited = false;
    int64_t retry_after_ms = 0;
};

/**
 * 设备信息
 * 
 * OpenClaw 对齐:
 * - device_id: 设备唯一标识
 * - device_token: 设备认证 Token
 * - device_name: 设备名称 (用户可读)
 * - platform: 平台 (macos, windows, linux, ios, android, web)
 * - scopes: 设备权限列表
 * - created_at: 创建时间
 * - expires_at: 过期时间
 * - revoked: 是否已撤销
 */
struct DeviceInfo {
    std::string device_id;
    std::string device_token;
    std::string device_name;
    std::string platform;
    std::vector<std::string> scopes;
    core::Timestamp created_at;
    core::Timestamp expires_at;
    bool revoked = false;
    
    // 可选: 公钥 (用于挑战-响应鉴权)
    std::string public_key;
    
    // 元数据
    std::map<std::string, std::string> metadata;
};

/**
 * 设备挑战 (Challenge-Response Authentication)
 * 
 * OpenClaw 对齐:
 * - nonce: 随机挑战字符串
 * - created_at: 创建时间
 * - expires_in_ms: 过期时间 (毫秒)
 */
struct DeviceChallenge {
    std::string nonce;
    core::Timestamp created_at;
    int64_t expires_in_ms = 300000;  // 5 分钟
    
    bool is_expired() const {
        auto now = core::utils::now();
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - created_at
        ).count();
        return age_ms > expires_in_ms;
    }
};

// ============================================================================
// AuthManager - 鉴权管理器
// ============================================================================

/**
 * 🎯 100% 对齐 OpenClaw: Gateway Authentication Manager
 * 
 * 支持的鉴权模式:
 * 1. **无鉴权** (mode: "none")
 *    - 用于本地开发/内网部署
 *    - 所有连接自动通过
 * 
 * 2. **Bearer Token** (mode: "bearer")
 *    - 统一 API Token
 *    - 适用于单用户/简单场景
 *    - Token 来源: HTTP Header 或 connect() 参数
 * 
 * 3. **设备认证** (mode: "device")
 *    - 每个设备独立的 device_id + device_token
 *    - 适用于多设备/多用户场景
 *    - 支持设备注册、撤销、权限管理
 * 
 * 4. **挑战-响应** (mode: "challenge")
 *    - 基于公钥加密的签名验证
 *    - 适用于高安全场景
 *    - 需要客户端持有私钥
 * 
 * 5. **Trusted Proxy** (mode: "trusted-proxy") 🎯 新增
 *    - 委托给反向代理（Pomerium, Caddy, nginx + OAuth）
 *    - 适用于企业级部署
 *    - 通过 HTTP Header 传递用户身份
 * 
 * 使用示例:
 * 
 * ```cpp
 * // 创建 AuthManager
 * auto auth_mgr = std::make_shared<AuthManager>(config);
 * 
 * // 鉴权 connect() 调用
 * auto result = auth_mgr->authenticate_connect(params, nonce);
 * if (!result.ok) {
 *     // 鉴权失败
 *     send_error(result.reason);
 *     return;
 * }
 * 
 * // 鉴权成功,创建 Session
 * auto session_id = create_session(result.device_id, result.scopes);
 * ```
 * 
 * OpenClaw 参考:
 * - src/gateway/server-methods/connect.ts
 * - src/gateway/auth.ts
 */
class AuthManager {
public:
    /**
     * 构造函数
     * @param config 配置对象 (包含 gateway.auth 配置)
     */
    explicit AuthManager(std::shared_ptr<core::Config> config);
    ~AuthManager();
    
    // ========== 连接鉴权 ==========
    
    /**
     * 🎯 OpenClaw 核心 API: 鉴权 connect() 调用
     * 
     * 根据配置的鉴权模式进行验证:
     * - mode: "none" → 自动通过
     * - mode: "bearer" → 验证 Bearer Token
     * - mode: "device" → 验证 device_id + device_token
     * - mode: "challenge" → 验证签名
     * - mode: "trusted-proxy" → 验证反向代理传递的用户身份
     * 
     * @param params connect() RPC 参数
     *               - token: Bearer Token (可选)
     *               - deviceId: 设备 ID (可选)
     *               - deviceToken: 设备 Token (可选)
     *               - nonce: 挑战 nonce (可选)
     *               - signature: 签名 (可选)
     * @param nonce 连接的 nonce (用于挑战-响应)
     * @param headers HTTP 请求头 (用于 Trusted Proxy)
     * @param client_ip 客户端 IP 地址 (用于速率限制和 Trusted Proxy 验证)
     * @return AuthResult 鉴权结果
     * 
     * OpenClaw 对齐:
     * ```typescript
     * async function authenticate(params: ConnectParams): Promise<AuthResult> {
     *   // 1. 速率限制检查
     *   const limiter = getRateLimiter();
     *   const check = limiter.check(clientIp);
     *   if (!check.allowed) return { ok: false, reason: 'RATE_LIMITED' };
     *   
     *   // 2. 鉴权模式检查
     *   if (config.auth.mode === 'none') return { ok: true };
     *   if (config.auth.mode === 'trusted-proxy') return validateTrustedProxy(headers, clientIp);
     *   if (config.auth.mode === 'bearer') return validateBearerToken(params.token);
     *   if (config.auth.mode === 'device') return validateDevice(params);
     *   return { ok: false, reason: 'INVALID_AUTH_MODE' };
     * }
     * ```
     */
    AuthResult authenticate_connect(
        const json& params,
        const std::string& nonce = "",
        const std::map<std::string, std::string>& headers = {},
        const std::string& client_ip = ""
    );
    
    // ========== Token 验证 ==========
    
    /**
     * 验证 Bearer Token
     * 
     * 支持两种格式:
     * - "sk-hunterclaw-abc123"
     * - "Bearer sk-hunterclaw-abc123"
     * 
     * @param token Token 字符串
     * @return 是否有效
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function validateBearerToken(token: string): boolean {
     *   const expected = config.auth.bearerToken;
     *   return token === expected || token === `Bearer ${expected}`;
     * }
     * ```
     */
    bool validate_bearer_token(const std::string& token) const;
    
    /**
     * 验证密码 (简单鉴权)
     * @param password 密码
     * @return 是否有效
     */
    bool validate_password(const std::string& password) const;
    
    // ========== 设备管理 ==========
    
    /**
     * 注册设备
     * 
     * 生成新的 device_id 和 device_token,并分配权限。
     * 
     * @param device_name 设备名称 (e.g., "My MacBook Pro")
     * @param platform 平台 (e.g., "macos", "windows", "linux")
     * @param scopes 权限列表 (可选,默认使用配置的权限)
     * @return 设备信息 (包含 device_id 和 device_token)
     * 
     * 使用示例:
     * ```cpp
     * auto result = auth_mgr->register_device("My MacBook Pro", "macos");
     * if (result.is_ok()) {
     *     auto device = result.value();
     *     std::cout << "Device ID: " << device.device_id << std::endl;
     *     std::cout << "Device Token: " << device.device_token << std::endl;
     *     // 保存 device_id 和 device_token 到客户端本地
     * }
     * ```
     * 
     * OpenClaw 对齐:
     * ```typescript
     * async function registerDevice(
     *   deviceName: string,
     *   platform: string
     * ): Promise<DeviceInfo> {
     *   const deviceId = generateUUID();
     *   const deviceToken = generateRandomToken();
     *   return { deviceId, deviceToken, deviceName, platform };
     * }
     * ```
     */
    core::Result<DeviceInfo> register_device(
        const std::string& device_name,
        const std::string& platform,
        const std::vector<std::string>& scopes = {}
    );
    
    /**
     * 验证设备
     * 
     * 检查 device_id 和 device_token 是否匹配,以及设备是否被撤销或过期。
     * 
     * @param device_id 设备 ID
     * @param device_token 设备 Token
     * @return 是否有效
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function verifyDevice(deviceId: string, deviceToken: string): boolean {
     *   const device = devices.get(deviceId);
     *   if (!device) return false;
     *   if (device.token !== deviceToken) return false;
     *   if (device.revoked) return false;
     *   if (device.expiresAt < Date.now()) return false;
     *   return true;
     * }
     * ```
     */
    bool verify_device(
        const std::string& device_id,
        const std::string& device_token
    );
    
    /**
     * 获取设备信息
     * @param device_id 设备 ID
     * @return 设备信息 (如果存在)
     */
    std::optional<DeviceInfo> get_device(const std::string& device_id) const;
    
    /**
     * 列出所有设备
     * @return 设备列表
     */
    std::vector<DeviceInfo> list_devices() const;
    
    /**
     * 撤销设备
     * 
     * 撤销后,设备将无法再通过鉴权。
     * 
     * @param device_id 设备 ID
     * @return 是否成功
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function revokeDevice(deviceId: string): boolean {
     *   const device = devices.get(deviceId);
     *   if (!device) return false;
     *   device.revoked = true;
     *   return true;
     * }
     * ```
     */
    bool revoke_device(const std::string& device_id);
    
    /**
     * 删除设备
     * @param device_id 设备 ID
     * @return 是否成功
     */
    bool delete_device(const std::string& device_id);
    
    /**
     * 更新设备信息
     * @param device_id 设备 ID
     * @param updates 更新的字段
     * @return 是否成功
     */
    bool update_device(
        const std::string& device_id,
        const std::map<std::string, std::string>& updates
    );
    
    // ========== 挑战-响应鉴权 ==========
    
    /**
     * 创建挑战
     * 
     * 生成一个随机 nonce,用于挑战-响应鉴权。
     * 
     * @return 挑战信息
     * 
     * 使用示例:
     * ```cpp
     * // 服务端: 创建挑战
     * auto challenge = auth_mgr->create_challenge();
     * send_to_client(challenge.nonce);
     * 
     * // 客户端: 签名挑战
     * std::string signature = sign(challenge.nonce, private_key);
     * 
     * // 客户端: 发送签名
     * connect({ deviceId, nonce: challenge.nonce, signature });
     * ```
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function createChallenge(): DeviceChallenge {
     *   const nonce = crypto.randomBytes(32).toString('hex');
     *   return { nonce, createdAt: Date.now() };
     * }
     * ```
     */
    DeviceChallenge create_challenge();
    
    /**
     * 验证设备签名
     * 
     * 使用设备的公钥验证签名。
     * 
     * @param device_id 设备 ID
     * @param nonce 挑战 nonce
     * @param signature 签名 (Base64 编码)
     * @return 是否有效
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function verifySignature(
     *   deviceId: string,
     *   nonce: string,
     *   signature: string
     * ): boolean {
     *   const device = devices.get(deviceId);
     *   if (!device || !device.publicKey) return false;
     *   return crypto.verify(nonce, signature, device.publicKey);
     * }
     * ```
     */
    bool verify_device_signature(
        const std::string& device_id,
        const std::string& nonce,
        const std::string& signature
    );
    
    // ========== 权限检查 ==========
    
    /**
     * 检查是否有指定权限
     * 
     * @param scopes 客户端权限列表
     * @param required_scope 所需权限
     * @return 是否有权限
     * 
     * 支持通配符:
     * - "admin.full" 匹配所有权限
     * - "agent.*" 匹配所有 agent 开头的权限
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function hasScope(
     *   scopes: string[],
     *   requiredScope: string
     * ): boolean {
     *   if (scopes.includes('admin.full')) return true;
     *   if (scopes.includes(requiredScope)) return true;
     *   // 检查通配符
     *   const prefix = requiredScope.split('.')[0];
     *   return scopes.includes(`${prefix}.*`);
     * }
     * ```
     */
    static bool has_scope(
        const std::vector<std::string>& scopes,
        const std::string& required_scope
    );
    
    // ========== Trusted Proxy 鉴权 (🎯 新增) ==========
    
    /**
     * 验证 Trusted Proxy 请求
     * 
     * @param headers HTTP 请求头
     * @param client_ip 客户端 IP 地址
     * @return 鉴权结果 (包含用户标识)
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function authorizeTrustedProxy(params: {
     *   req?: IncomingMessage;
     *   trustedProxies?: string[];
     *   trustedProxyConfig: GatewayTrustedProxyConfig;
     * }): { user: string } | { reason: string } {
     *   // 1. 验证请求来自受信任的代理 IP
     *   if (!isTrustedProxyAddress(remoteAddr, trustedProxies)) {
     *     return { reason: "trusted_proxy_untrusted_source" };
     *   }
     *   
     *   // 2. 验证必需的 HTTP 头存在
     *   for (const header of requiredHeaders) {
     *     if (!req.headers[header]) {
     *       return { reason: `trusted_proxy_missing_header_${header}` };
     *     }
     *   }
     *   
     *   // 3. 提取用户身份
     *   const user = req.headers[userHeader];
     *   if (!user) {
     *     return { reason: "trusted_proxy_user_missing" };
     *   }
     *   
     *   // 4. 检查用户白名单
     *   if (allowUsers.length > 0 && !allowUsers.includes(user)) {
     *     return { reason: "trusted_proxy_user_not_allowed" };
     *   }
     *   
     *   return { user };
     * }
     * ```
     */
    AuthResult authenticate_trusted_proxy(
        const std::map<std::string, std::string>& headers,
        const std::string& client_ip
    );
    
    // ========== 速率限制器 (🎯 新增) ==========
    
    /**
     * 获取速率限制器实例
     * @return 速率限制器 (可能为空)
     */
    std::shared_ptr<AuthRateLimiter> get_rate_limiter() const;
    
    /**
     * 设置速率限制器
     * @param limiter 速率限制器实例
     */
    void set_rate_limiter(std::shared_ptr<AuthRateLimiter> limiter);
    
    // ========== 持久化 ==========
    
    /**
     * 保存设备数据库到磁盘
     * @return 是否成功
     */
    core::Result<bool> save_devices();
    
    /**
     * 从磁盘加载设备数据库
     * @return 是否成功
     */
    core::Result<bool> load_devices();
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// 权限常量 (对齐 OpenClaw)
// ============================================================================

// ============================================================================
// 🎯 严格对齐 OpenClaw method-scopes.ts: 5 个 OperatorScope
// ============================================================================

namespace operator_scope {
    constexpr const char* ADMIN      = "operator.admin";
    constexpr const char* READ       = "operator.read";
    constexpr const char* WRITE      = "operator.write";
    constexpr const char* APPROVALS  = "operator.approvals";
    constexpr const char* PAIRING    = "operator.pairing";
}

// CLI 默认拥有所有 5 个 scope (对齐 OpenClaw CLI_DEFAULT_OPERATOR_SCOPES)
inline std::vector<std::string> cli_default_operator_scopes() {
    return {
        operator_scope::ADMIN,
        operator_scope::READ,
        operator_scope::WRITE,
        operator_scope::APPROVALS,
        operator_scope::PAIRING
    };
}

// 向后兼容别名 — 旧代码中引用 scopes::ADMIN_FULL 等的地方可平滑迁移
namespace scopes {
    // 映射到 OperatorScope
    constexpr const char* ADMIN_FULL     = "operator.admin";
    constexpr const char* AGENT_RUN      = "operator.write";    // agent → write scope
    constexpr const char* AGENT_CANCEL   = "operator.write";
    constexpr const char* AGENT_DEBUG    = "operator.read";
    constexpr const char* SESSION_READ   = "operator.read";
    constexpr const char* SESSION_WRITE  = "operator.admin";
    constexpr const char* SESSION_DELETE = "operator.admin";
    constexpr const char* SKILL_INSTALL  = "operator.admin";
    constexpr const char* SKILL_UNINSTALL = "operator.admin";
    constexpr const char* SKILL_LIST     = "operator.read";
    constexpr const char* PLUGIN_INSTALL  = "operator.admin";
    constexpr const char* PLUGIN_UNINSTALL = "operator.admin";
    constexpr const char* PLUGIN_LIST    = "operator.read";
    constexpr const char* CHANNEL_MANAGE = "operator.admin";
}

// ============================================================================
// 错误码常量 (对齐 OpenClaw)
// ============================================================================

namespace auth_errors {
    constexpr const char* NOT_AUTHENTICATED = "NOT_AUTHENTICATED";
    constexpr const char* DEVICE_TOKEN_MISMATCH = "DEVICE_TOKEN_MISMATCH";
    constexpr const char* INVALID_SIGNATURE = "INVALID_SIGNATURE";
    constexpr const char* PERMISSION_DENIED = "PERMISSION_DENIED";
    constexpr const char* TOKEN_EXPIRED = "TOKEN_EXPIRED";
    constexpr const char* DEVICE_REVOKED = "DEVICE_REVOKED";
    constexpr const char* INVALID_AUTH_MODE = "INVALID_AUTH_MODE";
    
    // 🎯 新增: Trusted Proxy 错误码 (对齐 OpenClaw)
    constexpr const char* TRUSTED_PROXY_UNTRUSTED_SOURCE = "TRUSTED_PROXY_UNTRUSTED_SOURCE";
    constexpr const char* TRUSTED_PROXY_USER_MISSING = "TRUSTED_PROXY_USER_MISSING";
    constexpr const char* TRUSTED_PROXY_USER_NOT_ALLOWED = "TRUSTED_PROXY_USER_NOT_ALLOWED";
    constexpr const char* TRUSTED_PROXY_MISSING_HEADER = "TRUSTED_PROXY_MISSING_HEADER";
    
    // 🎯 新增: 速率限制错误码
    constexpr const char* RATE_LIMITED = "RATE_LIMITED";
}

} // namespace gateway
} // namespace hunterclaw
