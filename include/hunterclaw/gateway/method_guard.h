#pragma once

#include "hunterclaw/gateway/auth_manager.h"
#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/core/result.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;

// ============================================================================
// 🔐 100% 对齐 OpenClaw: 方法权限守卫
// ============================================================================

/**
 * 权限检查结果
 */
struct PermissionCheckResult {
    bool allowed = false;
    std::string reason;              // 拒绝原因
    std::string missing_scope;       // 缺失的权限
};

/**
 * 资源访问策略
 * 
 * 定义资源的访问控制策略:
 * - OWNER_ONLY: 仅所有者可访问
 * - SESSION_SCOPED: Session 级别隔离
 * - DEVICE_SCOPED: 设备级别隔离
 * - GLOBAL: 全局可访问
 */
enum class ResourceAccessPolicy {
    OWNER_ONLY,        // 仅资源所有者
    SESSION_SCOPED,    // Session 级别隔离
    DEVICE_SCOPED,     // 设备级别隔离
    GLOBAL             // 全局可访问 (需要权限)
};

/**
 * 资源所有权信息
 */
struct ResourceOwnership {
    std::string owner_session_id;
    std::string owner_device_id;
    std::string owner_account_id;
    core::Timestamp created_at;
    ResourceAccessPolicy policy = ResourceAccessPolicy::SESSION_SCOPED;
};

// ============================================================================
// MethodGuard - 方法权限守卫
// ============================================================================

/**
 * 🎯 100% 对齐 OpenClaw: 方法权限守卫
 * 
 * 核心功能:
 * 1. **权限验证**: 检查客户端是否有调用方法的权限
 * 2. **资源隔离**: Session/Device 级别的资源访问控制
 * 3. **审计日志**: 记录敏感操作
 * 4. **速率限制**: 防止滥用 (TODO)
 * 
 * 使用示例:
 * 
 * ```cpp
 * // 创建守卫
 * auto guard = std::make_shared<MethodGuard>(auth_mgr);
 * 
 * // 在 RPC 处理前检查权限
 * auto check = guard->check_permission(client_ctx, "agent.run");
 * if (!check.allowed) {
 *     return error(check.reason);
 * }
 * 
 * // 检查资源访问
 * auto access = guard->check_resource_access(
 *     client_ctx, "session", target_session_id
 * );
 * if (!access.allowed) {
 *     return error("RESOURCE_ACCESS_DENIED");
 * }
 * ```
 * 
 * OpenClaw 参考:
 * - src/gateway/middleware/auth.ts
 * - src/gateway/middleware/permissions.ts
 */
class MethodGuard {
public:
    /**
     * 构造函数
     * @param auth_mgr AuthManager 实例
     */
    explicit MethodGuard(std::shared_ptr<AuthManager> auth_mgr);
    ~MethodGuard();
    
    // ========== 权限检查 ==========
    
    /**
     * 🎯 核心 API: 检查方法调用权限
     * 
     * 根据方法名查找所需权限,并检查客户端是否拥有。
     * 
     * @param client_ctx 客户端上下文
     * @param method RPC 方法名
     * @return 权限检查结果
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function checkPermission(
     *   ctx: ClientContext,
     *   method: string
     * ): PermissionCheckResult {
     *   const requiredScope = methodScopes[method];
     *   if (!requiredScope) return { allowed: true };
     *   
     *   if (hasScope(ctx.scopes, requiredScope)) {
     *     return { allowed: true };
     *   }
     *   
     *   return {
     *     allowed: false,
     *     reason: 'PERMISSION_DENIED',
     *     missingScope: requiredScope
     *   };
     * }
     * ```
     */
    PermissionCheckResult check_permission(
        const ClientContext& client_ctx,
        const std::string& method
    ) const;
    
    /**
     * 检查是否有特定权限
     * @param client_ctx 客户端上下文
     * @param required_scope 所需权限
     * @return 权限检查结果
     */
    PermissionCheckResult check_scope(
        const ClientContext& client_ctx,
        const std::string& required_scope
    ) const;
    
    // ========== 资源访问控制 ==========
    
    /**
     * 🎯 核心 API: 检查资源访问权限
     * 
     * 实现 Session/Device 级别的资源隔离。
     * 
     * @param client_ctx 客户端上下文
     * @param resource_type 资源类型 (session, agent, skill, etc.)
     * @param resource_id 资源 ID
     * @return 权限检查结果
     * 
     * 策略:
     * - Session: 仅同一 Session 的客户端可访问
     * - Agent Run: 仅创建者的 Session 可访问
     * - Skill/Plugin: 仅安装者的 Device 可访问
     * - Channel: 需要 channel.manage 权限
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function checkResourceAccess(
     *   ctx: ClientContext,
     *   resourceType: string,
     *   resourceId: string
     * ): PermissionCheckResult {
     *   const resource = getResource(resourceType, resourceId);
     *   if (!resource) return { allowed: false, reason: 'NOT_FOUND' };
     *   
     *   // Session 隔离
     *   if (resourceType === 'session') {
     *     if (resource.sessionId !== ctx.sessionId) {
     *       return { allowed: false, reason: 'RESOURCE_ACCESS_DENIED' };
     *     }
     *   }
     *   
     *   return { allowed: true };
     * }
     * ```
     */
    PermissionCheckResult check_resource_access(
        const ClientContext& client_ctx,
        const std::string& resource_type,
        const std::string& resource_id
    ) const;
    
    /**
     * 注册资源所有权
     * 
     * 当创建资源时,记录其所有者信息。
     * 
     * @param resource_type 资源类型
     * @param resource_id 资源 ID
     * @param owner 所有权信息
     */
    void register_resource_ownership(
        const std::string& resource_type,
        const std::string& resource_id,
        const ResourceOwnership& owner
    );
    
    /**
     * 检查资源所有权
     * @param resource_type 资源类型
     * @param resource_id 资源 ID
     * @return 所有权信息 (如果存在)
     */
    std::optional<ResourceOwnership> get_resource_ownership(
        const std::string& resource_type,
        const std::string& resource_id
    ) const;
    
    /**
     * 删除资源所有权记录
     * @param resource_type 资源类型
     * @param resource_id 资源 ID
     */
    void unregister_resource_ownership(
        const std::string& resource_type,
        const std::string& resource_id
    );
    
    // ========== 方法-权限映射 ==========
    
    /**
     * 设置方法所需权限
     * @param method 方法名
     * @param required_scope 所需权限
     */
    void set_method_scope(
        const std::string& method,
        const std::string& required_scope
    );
    
    /**
     * 批量设置方法权限映射
     * @param mappings 方法 → 权限映射
     */
    void set_method_scopes(
        const std::map<std::string, std::string>& mappings
    );
    
    /**
     * 获取方法所需权限
     * @param method 方法名
     * @return 所需权限 (如果定义)
     */
    std::optional<std::string> get_method_scope(
        const std::string& method
    ) const;
    
    // ========== 审计日志 ==========
    
    /**
     * 记录审计日志
     * 
     * 记录敏感操作 (创建/修改/删除资源)。
     * 
     * @param client_ctx 客户端上下文
     * @param method 方法名
     * @param params 参数
     * @param result 结果
     */
    void log_audit(
        const ClientContext& client_ctx,
        const std::string& method,
        const json& params,
        const json& result
    );
    
    /**
     * 获取审计日志
     * @param limit 最大返回条数
     * @return 审计日志列表
     */
    std::vector<json> get_audit_logs(int limit = 100) const;
    
    // ========== 静态工具方法 ==========
    
    /**
     * 获取默认的方法-权限映射
     * 
     * OpenClaw 对齐的完整映射表。
     * 
     * @return 方法 → 权限映射
     */
    static std::map<std::string, std::string> get_default_method_scopes();
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// 工具函数
// ============================================================================

/**
 * 从 RPC params 中提取目标 Session ID
 * 
 * 很多 RPC 方法需要指定 session_id,这个函数提取它。
 * 
 * @param params RPC 参数
 * @return Session ID (如果存在)
 */
inline std::optional<std::string> extract_target_session_id(const json& params) {
    if (params.contains("session_id") && params["session_id"].is_string()) {
        return params["session_id"].get<std::string>();
    }
    if (params.contains("sessionId") && params["sessionId"].is_string()) {
        return params["sessionId"].get<std::string>();
    }
    return std::nullopt;
}

/**
 * 从 RPC params 中提取目标 Run ID
 */
inline std::optional<std::string> extract_target_run_id(const json& params) {
    if (params.contains("run_id") && params["run_id"].is_string()) {
        return params["run_id"].get<std::string>();
    }
    if (params.contains("runId") && params["runId"].is_string()) {
        return params["runId"].get<std::string>();
    }
    return std::nullopt;
}

/**
 * 判断方法是否需要鉴权
 * 
 * 某些方法不需要鉴权 (如 ping, echo)。
 * 
 * @param method 方法名
 * @return 是否需要鉴权
 */
inline bool method_requires_auth(const std::string& method) {
    // 白名单: 不需要鉴权的方法
    static const std::vector<std::string> no_auth_methods = {
        "ping",
        "echo",
        "status"  // 基础状态查询
    };
    
    for (const auto& m : no_auth_methods) {
        if (method == m) {
            return false;
        }
    }
    
    return true;
}

/**
 * 判断方法是否为敏感操作
 * 
 * 敏感操作需要记录审计日志。
 * 
 * @param method 方法名
 * @return 是否为敏感操作
 */
inline bool is_sensitive_operation(const std::string& method) {
    // 创建、删除、修改操作都是敏感的
    return method.find("create") != std::string::npos ||
           method.find("delete") != std::string::npos ||
           method.find("update") != std::string::npos ||
           method.find("install") != std::string::npos ||
           method.find("uninstall") != std::string::npos ||
           method.find("revoke") != std::string::npos ||
           method.find("register") != std::string::npos;
}

} // namespace gateway
} // namespace hunterclaw
