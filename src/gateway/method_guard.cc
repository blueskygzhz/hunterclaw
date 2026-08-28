#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <mutex>
#include <shared_mutex>
#include <set>

namespace hunterclaw {
namespace gateway {

// Forward declarations
static const std::set<std::string>& node_role_methods();
static const std::vector<std::string>& admin_method_prefixes();

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
    
    // 🎯 对齐 OpenClaw: NODE_ROLE_METHODS 不受 OperatorScope 限制
    if (node_role_methods().count(method) > 0) {
        return { true, "", "" };
    }
    
    // 3. 查找方法所需权限 (显式映射)
    auto scope_opt = get_method_scope(method);
    
    std::string required_scope;
    if (scope_opt.has_value()) {
        required_scope = scope_opt.value();
    } else {
        // 🎯 对齐 OpenClaw: ADMIN_METHOD_PREFIXES 前缀匹配
        bool matched_prefix = false;
        for (const auto& prefix : admin_method_prefixes()) {
            if (method.compare(0, prefix.size(), prefix) == 0) {
                required_scope = operator_scope::ADMIN;
                matched_prefix = true;
                break;
            }
        }
        
        if (!matched_prefix) {
            // 🎯 对齐 OpenClaw: 未分类方法默认需要 admin scope
            required_scope = operator_scope::ADMIN;
        }
    }
    
    // 4. 检查权限
    return check_scope(client_ctx, required_scope);
}

PermissionCheckResult MethodGuard::check_scope(
    const ClientContext& client_ctx,
    const std::string& required_scope
) const {
    const auto& client_scopes = client_ctx.scopes();
    
    // 🎯 对齐 OpenClaw authorizeOperatorScopesForMethod:
    // 1. operator.admin 拥有所有权限
    if (AuthManager::has_scope(client_scopes, operator_scope::ADMIN)) {
        return { true, "", "" };
    }
    
    // 2. operator.read 方法可以被 operator.write 满足
    if (required_scope == operator_scope::READ) {
        if (AuthManager::has_scope(client_scopes, operator_scope::READ) ||
            AuthManager::has_scope(client_scopes, operator_scope::WRITE)) {
            return { true, "", "" };
        }
        return { false, auth_errors::PERMISSION_DENIED, required_scope };
    }
    
    // 3. 精确匹配
    if (AuthManager::has_scope(client_scopes, required_scope)) {
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

// 🎯 严格对齐 OpenClaw method-scopes.ts: NODE_ROLE_METHODS
static const std::set<std::string>& node_role_methods() {
    static const std::set<std::string> methods = {
        "node.invoke.result",
        "node.event",
        "node.pending.drain",
        "node.canvas.capability.refresh",
        "node.pending.pull",
        "node.pending.ack",
        "skills.bins"
    };
    return methods;
}

// 🎯 严格对齐 OpenClaw method-scopes.ts: ADMIN_METHOD_PREFIXES
static const std::vector<std::string>& admin_method_prefixes() {
    static const std::vector<std::string> prefixes = {
        "exec.approvals.",
        "config.",
        "wizard.",
        "update."
    };
    return prefixes;
}

std::map<std::string, std::string> MethodGuard::get_default_method_scopes() {
    // 🎯 严格对齐 OpenClaw method-scopes.ts: METHOD_SCOPE_GROUPS
    // 5 个 OperatorScope 分组，每个方法映射到恰好一个 scope
    return {
        // ========== operator.approvals ==========
        {"exec.approval.request",       operator_scope::APPROVALS},
        {"exec.approval.waitDecision",  operator_scope::APPROVALS},
        {"exec.approval.resolve",       operator_scope::APPROVALS},
        // ★ canonical 入口与 plugin(工具)审批同属 APPROVALS scope。
        //
        // ⚠⚠ 必须显式列出。审批决策是【授权动作】—— 漏登记会让它落到
        //   默认分支: 要么无 scope 校验(任何 operator 都能批别人的审批),
        //   要么被一律拒绝(功能静默不可用)。两种都不能接受, 且都不会报错。
        {"approval.resolve",               operator_scope::APPROVALS},
        {"plugin.approval.list",           operator_scope::APPROVALS},
        {"plugin.approval.request",        operator_scope::APPROVALS},
        {"plugin.approval.resolve",        operator_scope::APPROVALS},
        {"plugin.approval.waitDecision",   operator_scope::APPROVALS},

        // ========== operator.pairing ==========
        {"node.pair.request",    operator_scope::PAIRING},
        {"node.pair.list",       operator_scope::PAIRING},
        {"node.pair.approve",    operator_scope::PAIRING},
        {"node.pair.reject",     operator_scope::PAIRING},
        {"node.pair.verify",     operator_scope::PAIRING},
        {"device.pair.list",     operator_scope::PAIRING},
        {"device.pair.approve",  operator_scope::PAIRING},
        {"device.pair.reject",   operator_scope::PAIRING},
        {"device.pair.remove",   operator_scope::PAIRING},
        {"device.token.rotate",  operator_scope::PAIRING},
        {"device.token.revoke",  operator_scope::PAIRING},
        {"node.rename",          operator_scope::PAIRING},

        // ========== operator.read ==========
        {"health",                    operator_scope::READ},
        {"doctor.memory.status",      operator_scope::READ},
        {"logs.tail",                 operator_scope::READ},
        {"channels.status",           operator_scope::READ},
        {"status",                    operator_scope::READ},
        {"usage.status",              operator_scope::READ},
        {"usage.cost",                operator_scope::READ},
        {"tts.status",                operator_scope::READ},
        {"tts.providers",             operator_scope::READ},
        {"models.list",               operator_scope::READ},
        {"tools.catalog",             operator_scope::READ},
        {"agents.list",               operator_scope::READ},
        {"agent.identity.get",        operator_scope::READ},
        {"skills.status",             operator_scope::READ},
        {"voicewake.get",             operator_scope::READ},
        {"sessions.list",             operator_scope::READ},
        {"sessions.get",              operator_scope::READ},
        {"sessions.preview",          operator_scope::READ},
        {"sessions.resolve",          operator_scope::READ},
        {"sessions.usage",            operator_scope::READ},
        {"sessions.usage.timeseries", operator_scope::READ},
        {"sessions.usage.logs",       operator_scope::READ},
        {"cron.list",                 operator_scope::READ},
        {"cron.status",               operator_scope::READ},
        {"cron.runs",                 operator_scope::READ},
        {"gateway.identity.get",      operator_scope::READ},
        {"system-presence",           operator_scope::READ},
        {"last-heartbeat",            operator_scope::READ},
        {"node.list",                 operator_scope::READ},
        {"node.describe",             operator_scope::READ},
        {"chat.history",              operator_scope::READ},
        {"config.get",                operator_scope::READ},
        {"config.schema.lookup",      operator_scope::READ},
        {"talk.config",               operator_scope::READ},
        {"agents.files.list",         operator_scope::READ},
        {"agents.files.get",          operator_scope::READ},

        // ========== operator.write ==========
        {"send",                  operator_scope::WRITE},
        {"poll",                  operator_scope::WRITE},
        {"agent",                 operator_scope::WRITE},
        {"agent.wait",            operator_scope::WRITE},
        {"wake",                  operator_scope::WRITE},
        {"talk.mode",             operator_scope::WRITE},
        {"tts.enable",            operator_scope::WRITE},
        {"tts.disable",           operator_scope::WRITE},
        {"tts.convert",           operator_scope::WRITE},
        {"tts.setProvider",       operator_scope::WRITE},
        {"voicewake.set",         operator_scope::WRITE},
        {"node.invoke",           operator_scope::WRITE},
        {"chat.send",             operator_scope::WRITE},
        {"chat.abort",            operator_scope::WRITE},
        {"browser.request",       operator_scope::WRITE},
        {"push.test",             operator_scope::WRITE},
        {"node.pending.enqueue",  operator_scope::WRITE},

        // ========== operator.admin ==========
        {"channels.logout",   operator_scope::ADMIN},
        {"agents.create",     operator_scope::ADMIN},
        {"agents.update",     operator_scope::ADMIN},
        {"agents.delete",     operator_scope::ADMIN},
        {"skills.install",    operator_scope::ADMIN},
        {"skills.update",     operator_scope::ADMIN},
        {"secrets.reload",    operator_scope::ADMIN},
        {"secrets.resolve",   operator_scope::ADMIN},
        {"cron.add",          operator_scope::ADMIN},
        {"cron.update",       operator_scope::ADMIN},
        {"cron.remove",       operator_scope::ADMIN},
        {"cron.run",          operator_scope::ADMIN},
        {"sessions.patch",    operator_scope::ADMIN},
        {"sessions.reset",    operator_scope::ADMIN},
        {"sessions.delete",   operator_scope::ADMIN},
        {"sessions.compact",  operator_scope::ADMIN},
        {"connect",           operator_scope::ADMIN},
        {"chat.inject",       operator_scope::ADMIN},
        {"web.login.start",   operator_scope::ADMIN},
        {"web.login.wait",    operator_scope::ADMIN},
        {"set-heartbeats",    operator_scope::ADMIN},
        {"system-event",      operator_scope::ADMIN},
        {"agents.files.set",  operator_scope::ADMIN}
    };
}

} // namespace gateway
} // namespace hunterclaw
