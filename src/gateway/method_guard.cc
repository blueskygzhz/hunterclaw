#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <mutex>
#include <shared_mutex>

namespace hunterclaw {
namespace gateway {

// Helper function to get logger
static inline std::shared_ptr<spdlog::logger> get_logger() {
    auto logger = spdlog::get("console");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("console");
    }
    return logger;
}

// ============================================================================
// MethodGuard::Impl
// ============================================================================

struct MethodGuard::Impl {
    std::shared_ptr<AuthManager> auth_mgr;
    
    // 方法 → 权限映射
    std::map<std::string, std::string> method_scopes;
    mutable std::shared_mutex method_scopes_mutex;
    
    // 资源所有权记录 (resource_type:resource_id → ResourceOwnership)
    std::map<std::string, ResourceOwnership> resource_ownership;
    mutable std::shared_mutex resource_ownership_mutex;
    
    // 审计日志
    std::vector<json> audit_logs;
    mutable std::mutex audit_logs_mutex;
    const size_t max_audit_logs = 10000;  // 最多保存 10000 条
    
    explicit Impl(std::shared_ptr<AuthManager> mgr) : auth_mgr(mgr) {
        // 初始化默认方法权限映射
        method_scopes = MethodGuard::get_default_method_scopes();
    }
    
    std::string make_resource_key(
        const std::string& resource_type,
        const std::string& resource_id
    ) const {
        return resource_type + ":" + resource_id;
    }
};

// ============================================================================
// MethodGuard 实现
// ============================================================================

MethodGuard::MethodGuard(std::shared_ptr<AuthManager> auth_mgr)
    : impl_(std::make_unique<Impl>(auth_mgr))
{
    auto logger = get_logger();
    logger->info("[MethodGuard] Initialized with {} method scope mappings",
        impl_->method_scopes.size());
}

MethodGuard::~MethodGuard() = default;

// ========== 权限检查 ==========

PermissionCheckResult MethodGuard::check_permission(
    const ClientContext& client_ctx,
    const std::string& method
) const {
    // 1. 检查是否需要鉴权
    if (!method_requires_auth(method)) {
        return { true, "", "" };
    }
    
    // 2. 检查客户端是否已鉴权
    if (!client_ctx.authenticated) {
        return {
            false,
            auth_errors::NOT_AUTHENTICATED,
            ""
        };
    }
    
    // 3. 查找方法所需权限
    auto scope_opt = get_method_scope(method);
    if (!scope_opt.has_value()) {
        // 未定义权限要求 → 默认通过
        return { true, "", "" };
    }
    
    std::string required_scope = scope_opt.value();
    
    // 4. 检查权限
    return check_scope(client_ctx, required_scope);
}

PermissionCheckResult MethodGuard::check_scope(
    const ClientContext& client_ctx,
    const std::string& required_scope
) const {
    if (AuthManager::has_scope(client_ctx.scopes(), required_scope)) {
        return { true, "", "" };
    }
    
    return {
        false,
        auth_errors::PERMISSION_DENIED,
        required_scope
    };
}

// ========== 资源访问控制 ==========

PermissionCheckResult MethodGuard::check_resource_access(
    const ClientContext& client_ctx,
    const std::string& resource_type,
    const std::string& resource_id
) const {
    // 1. 获取资源所有权信息
    auto ownership_opt = get_resource_ownership(resource_type, resource_id);
    
    if (!ownership_opt.has_value()) {
        // 资源不存在或未注册所有权 → 拒绝访问
        auto logger = get_logger();
        logger->warn("[MethodGuard] Resource not found: {}:{}",
            resource_type, resource_id);
        return {
            false,
            "RESOURCE_NOT_FOUND",
            ""
        };
    }
    
    const auto& ownership = ownership_opt.value();
    
    // 2. 根据访问策略检查
    switch (ownership.policy) {
        case ResourceAccessPolicy::OWNER_ONLY:
            // 仅所有者可访问
            if (client_ctx.session_id != ownership.owner_session_id) {
                auto logger = get_logger();
                logger->warn("[MethodGuard] Access denied: client {} tried to access resource {}:{} owned by {}",
                    client_ctx.conn_id, resource_type, resource_id,
                    ownership.owner_session_id);
                return {
                    false,
                    "RESOURCE_ACCESS_DENIED",
                    ""
                };
            }
            break;
            
        case ResourceAccessPolicy::SESSION_SCOPED:
            // Session 级别隔离
            if (client_ctx.session_id != ownership.owner_session_id) {
                return {
                    false,
                    "RESOURCE_ACCESS_DENIED",
                    ""
                };
            }
            break;
            
        case ResourceAccessPolicy::DEVICE_SCOPED:
            // 设备级别隔离
            if (client_ctx.device_id != ownership.owner_device_id) {
                return {
                    false,
                    "RESOURCE_ACCESS_DENIED",
                    ""
                };
            }
            break;
            
        case ResourceAccessPolicy::GLOBAL:
            // 全局可访问 (但需要权限)
            // 权限检查由 check_permission() 处理
            break;
    }
    
    return { true, "", "" };
}

void MethodGuard::register_resource_ownership(
    const std::string& resource_type,
    const std::string& resource_id,
    const ResourceOwnership& owner
) {
    std::unique_lock<std::shared_mutex> lock(impl_->resource_ownership_mutex);
    
    std::string key = impl_->make_resource_key(resource_type, resource_id);
    impl_->resource_ownership[key] = owner;
    
    auto logger = get_logger();
    logger->debug("[MethodGuard] Registered ownership: {}:{} → session={}",
        resource_type, resource_id, owner.owner_session_id);
}

std::optional<ResourceOwnership> MethodGuard::get_resource_ownership(
    const std::string& resource_type,
    const std::string& resource_id
) const {
    std::shared_lock<std::shared_mutex> lock(impl_->resource_ownership_mutex);
    
    std::string key = impl_->make_resource_key(resource_type, resource_id);
    auto it = impl_->resource_ownership.find(key);
    
    if (it != impl_->resource_ownership.end()) {
        return it->second;
    }
    
    return std::nullopt;
}

void MethodGuard::unregister_resource_ownership(
    const std::string& resource_type,
    const std::string& resource_id
) {
    std::unique_lock<std::shared_mutex> lock(impl_->resource_ownership_mutex);
    
    std::string key = impl_->make_resource_key(resource_type, resource_id);
    impl_->resource_ownership.erase(key);
    
    auto logger = get_logger();
    logger->debug("[MethodGuard] Unregistered ownership: {}:{}",
        resource_type, resource_id);
}

// ========== 方法-权限映射 ==========

void MethodGuard::set_method_scope(
    const std::string& method,
    const std::string& required_scope
) {
    std::unique_lock<std::shared_mutex> lock(impl_->method_scopes_mutex);
    impl_->method_scopes[method] = required_scope;
}

void MethodGuard::set_method_scopes(
    const std::map<std::string, std::string>& mappings
) {
    std::unique_lock<std::shared_mutex> lock(impl_->method_scopes_mutex);
    for (const auto& [method, scope] : mappings) {
        impl_->method_scopes[method] = scope;
    }
}

std::optional<std::string> MethodGuard::get_method_scope(
    const std::string& method
) const {
    std::shared_lock<std::shared_mutex> lock(impl_->method_scopes_mutex);
    
    auto it = impl_->method_scopes.find(method);
    if (it != impl_->method_scopes.end()) {
        return it->second;
    }
    
    return std::nullopt;
}

// ========== 审计日志 ==========

void MethodGuard::log_audit(
    const ClientContext& client_ctx,
    const std::string& method,
    const json& params,
    const json& result
) {
    std::lock_guard<std::mutex> lock(impl_->audit_logs_mutex);
    
    json log_entry;
    log_entry["timestamp"] = core::utils::now().time_since_epoch().count();
    log_entry["client_id"] = client_ctx.conn_id;
    log_entry["session_id"] = client_ctx.session_id;
    log_entry["device_id"] = client_ctx.device_id;
    log_entry["method"] = method;
    log_entry["params"] = params;
    log_entry["result"] = result;
    log_entry["scopes"] = client_ctx.scopes();
    
    impl_->audit_logs.push_back(log_entry);
    
    // 限制日志数量
    if (impl_->audit_logs.size() > impl_->max_audit_logs) {
        // 删除最老的 1000 条
        impl_->audit_logs.erase(
            impl_->audit_logs.begin(),
            impl_->audit_logs.begin() + 1000
        );
    }
    
    auto logger = get_logger();
    logger->info("[Audit] {} by client {} (session={})",
        method, client_ctx.conn_id, client_ctx.session_id);
}

std::vector<json> MethodGuard::get_audit_logs(int limit) const {
    std::lock_guard<std::mutex> lock(impl_->audit_logs_mutex);
    
    if (limit <= 0 || limit >= static_cast<int>(impl_->audit_logs.size())) {
        return impl_->audit_logs;
    }
    
    // 返回最近的 N 条
    return std::vector<json>(
        impl_->audit_logs.end() - limit,
        impl_->audit_logs.end()
    );
}

// ========== 静态方法 ==========

std::map<std::string, std::string> MethodGuard::get_default_method_scopes() {
    // 🎯 100% OpenClaw 对齐的方法权限映射
    return {
        // ========== Agent Methods ==========
        {"agent.run", scopes::AGENT_RUN},
        {"agent.cancel", scopes::AGENT_CANCEL},
        {"agent.abort", scopes::AGENT_CANCEL},
        {"agent.pause", scopes::AGENT_CANCEL},
        {"agent.resume", scopes::AGENT_CANCEL},
        {"agent.status", scopes::AGENT_RUN},       // 读取也需要权限
        {"agent.wait", scopes::AGENT_RUN},
        {"agent.result", scopes::AGENT_RUN},
        {"agent.logs", scopes::AGENT_DEBUG},
        {"agent.snapshot", scopes::AGENT_DEBUG},
        {"agent.list", scopes::AGENT_RUN},
        {"agent.identity", scopes::AGENT_RUN},
        
        {"agents.spawn", scopes::AGENT_RUN},
        {"agents.list", scopes::AGENT_RUN},
        {"agents.terminate", scopes::AGENT_CANCEL},
        
        // ========== Session Methods ==========
        {"session.create", scopes::SESSION_WRITE},
        {"session.update", scopes::SESSION_WRITE},
        {"session.delete", scopes::SESSION_DELETE},
        {"session.reset", scopes::SESSION_WRITE},
        {"session.get", scopes::SESSION_READ},
        {"session.list", scopes::SESSION_READ},
        
        // ========== Chat Methods ==========
        {"chat.send", scopes::AGENT_RUN},          // 发送消息 = 运行 Agent
        {"chat.abort", scopes::AGENT_CANCEL},
        {"chat.history", scopes::SESSION_READ},
        {"chat.updateParams", scopes::SESSION_WRITE},
        {"chat.regenerate", scopes::AGENT_RUN},
        {"chat.fork", scopes::SESSION_WRITE},
        {"chat.summarize", scopes::AGENT_RUN},
        {"chat.inject", scopes::SESSION_WRITE},
        {"chat.edit", scopes::SESSION_WRITE},
        {"chat.delete", scopes::SESSION_WRITE},
        {"chat.transcript", scopes::SESSION_READ},
        {"chat.export", scopes::SESSION_READ},
        
        // ========== Skill Methods ==========
        {"skill.list", scopes::SKILL_LIST},
        {"skill.get", scopes::SKILL_LIST},
        {"skill.install", scopes::SKILL_INSTALL},
        {"skill.update", scopes::SKILL_INSTALL},
        {"skill.uninstall", scopes::SKILL_UNINSTALL},
        {"skill.reload", scopes::SKILL_INSTALL},
        
        // ========== Plugin Methods ==========
        {"plugin.list", scopes::PLUGIN_LIST},
        {"plugin.install", scopes::PLUGIN_INSTALL},
        {"plugin.uninstall", scopes::PLUGIN_UNINSTALL},
        
        // ========== Channel Methods ==========
        {"channel.list", scopes::CHANNEL_MANAGE},
        {"channel.get", scopes::CHANNEL_MANAGE},
        {"channel.start", scopes::CHANNEL_MANAGE},
        {"channel.stop", scopes::CHANNEL_MANAGE},
        {"channel.restart", scopes::CHANNEL_MANAGE},
        {"channel.send", scopes::CHANNEL_MANAGE},
        {"channel.health", scopes::CHANNEL_MANAGE},
        {"channel.config", scopes::CHANNEL_MANAGE},
        
        // ========== Config Methods ==========
        {"config.get", scopes::SESSION_READ},      // 读取配置需要基本权限
        {"config.set", scopes::ADMIN_FULL},        // 修改配置需要管理员权限
        {"config.reload", scopes::ADMIN_FULL},
        {"config.patch", scopes::ADMIN_FULL},
        {"config.apply", scopes::ADMIN_FULL},
        {"config.validate", scopes::SESSION_READ},
        {"config.export", scopes::SESSION_READ},
        {"config.schema", scopes::SESSION_READ},
        
        // ========== Discovery & Nodes (管理员功能) ==========
        {"discovery.announce", scopes::ADMIN_FULL},
        {"discovery.query", scopes::ADMIN_FULL},
        {"discovery.health", scopes::ADMIN_FULL},
        
        {"nodes.list", scopes::ADMIN_FULL},
        {"nodes.connect", scopes::ADMIN_FULL},
        {"nodes.disconnect", scopes::ADMIN_FULL},
        {"nodes.invoke", scopes::ADMIN_FULL},
        {"nodes.status", scopes::ADMIN_FULL},
        {"nodes.broadcast", scopes::ADMIN_FULL},
        {"nodes.subscribe", scopes::ADMIN_FULL},
        {"nodes.unsubscribe", scopes::ADMIN_FULL},
        {"nodes.discover", scopes::ADMIN_FULL},
        {"nodes.register", scopes::ADMIN_FULL},
        {"nodes.health", scopes::ADMIN_FULL},
        {"nodes.sync", scopes::ADMIN_FULL},
        
        // ========== Hooks (管理员功能) ==========
        {"hooks.register", scopes::ADMIN_FULL},
        {"hooks.unregister", scopes::ADMIN_FULL},
        {"hooks.list", scopes::ADMIN_FULL},
        {"hooks.trigger", scopes::ADMIN_FULL},
        
        // ========== Cron (管理员功能) ==========
        {"wake", scopes::SESSION_READ},            // Heartbeat 不需要特殊权限
        {"cron.status", scopes::ADMIN_FULL},
        {"cron.list", scopes::ADMIN_FULL},
        {"cron.add", scopes::ADMIN_FULL},
        {"cron.update", scopes::ADMIN_FULL},
        {"cron.remove", scopes::ADMIN_FULL},
        {"cron.run", scopes::ADMIN_FULL},
        {"cron.runs", scopes::ADMIN_FULL}
        
        // ========== 注意 ==========
        // ping, echo, status 不在此列表中,因为它们不需要鉴权
    };
}

} // namespace gateway
} // namespace hunterclaw
