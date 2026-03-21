#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/gateway/todo_types.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;

// ============================================================================
// 🔐 TODO 方法 - 100% OpenClaw 对齐的严格鉴权实现
// ============================================================================

/**
 * todo.create - 创建 TODO
 * 
 * 鉴权流程:
 * 1. 检查客户端权限 (需要 session.write)
 * 2. 创建 TODO,绑定到当前 Session
 * 3. 注册资源所有权 (SESSION_SCOPED)
 * 4. 记录审计日志
 * 5. 广播事件(仅限当前 Session)
 */
core::Result<json> method_todo_create(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ========== 第 1 步: 权限检查 ==========
    auto perm_check = method_guard->check_permission(client_ctx, "todo.create");
    if (!perm_check.allowed) {
        return core::Result<json>::Err(perm_check.reason);
    }
    
    // ========== 第 2 步: 参数验证 ==========
    if (!params.contains("content") || !params["content"].is_string()) {
        return core::Result<json>::Err("Missing or invalid 'content' parameter");
    }
    
    std::string content = params["content"];
    std::string status = params.value("status", "pending");
    
    // 验证状态
    if (!todo_status::is_valid_status(status)) {
        return core::Result<json>::Err("Invalid status: " + status);
    }
    
    // ========== 第 3 步: 创建 TODO ==========
    TodoItem todo;
    todo.id = core::utils::generate_uuid();
    todo.session_id = client_ctx.session_id;  // 🔐 关键:绑定到 Session!
    todo.content = content;
    todo.status = status;
    todo.created_at = core::utils::now();
    todo.updated_at = todo.created_at;
    
    // 保存到存储
    auto state = server->runtime_state();
    std::lock_guard<std::mutex> lock(state->mutex);
    state->todos[todo.id] = todo;
    
    // ========== 第 4 步: 注册资源所有权 ==========
    ResourceOwnership ownership;
    ownership.owner_session_id = client_ctx.session_id;
    ownership.owner_device_id = client_ctx.device_id;
    ownership.owner_account_id = client_ctx.account_id;
    ownership.created_at = todo.created_at;
    ownership.policy = ResourceAccessPolicy::SESSION_SCOPED;  // Session 隔离
    
    method_guard->register_resource_ownership("todo", todo.id, ownership);
    
    // ========== 第 5 步: 审计日志 ==========
    method_guard->log_audit(
        client_ctx,
        "todo.create",
        params,
        {{"todo_id", todo.id}, {"status", "created"}}
    );
    
    // ========== 第 6 步: 广播事件 ==========
    core::EventFrame event;
    event.event = "todo.created";
    event.payload = todo.to_json();
    
    // 仅发送给当前 Session 的客户端
    std::vector<std::string> target_conns;
    for (const auto& [conn_id, client] : state->clients) {
        if (client->session_id.value_or("") == client_ctx.session_id) {
            target_conns.push_back(conn_id);
        }
    }
    server->broadcast_to_conn_ids(target_conns, event);
    
    return core::Result<json>::Ok(todo.to_json());
}

/**
 * todo.get - 获取 TODO
 * 
 * 鉴权流程:
 * 1. 检查方法权限
 * 2. 检查资源访问权限 (Session 隔离)
 * 3. 二次验证 session_id
 */
core::Result<json> method_todo_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ========== 第 1 步: 权限检查 ==========
    auto perm_check = method_guard->check_permission(client_ctx, "todo.get");
    if (!perm_check.allowed) {
        return core::Result<json>::Err(perm_check.reason);
    }
    
    // ========== 第 2 步: 参数验证 ==========
    if (!params.contains("todo_id") || !params["todo_id"].is_string()) {
        return core::Result<json>::Err("Missing or invalid 'todo_id' parameter");
    }
    
    std::string todo_id = params["todo_id"];
    
    // ========== 第 3 步: 资源访问检查 ==========
    auto access_check = method_guard->check_resource_access(
        client_ctx, "todo", todo_id
    );
    
    if (!access_check.allowed) {
        return core::Result<json>::Err("TODO_ACCESS_DENIED");
    }
    
    // ========== 第 4 步: 读取 TODO ==========
    auto state = server->runtime_state();
    std::lock_guard<std::mutex> lock(state->mutex);
    
    auto it = state->todos.find(todo_id);
    if (it == state->todos.end()) {
        return core::Result<json>::Err("TODO not found: " + todo_id);
    }
    
    const auto& todo = it->second;
    
    // ========== 第 5 步: 二次验证 (防御性检查) ==========
    if (todo.session_id != client_ctx.session_id) {
        return core::Result<json>::Err("SECURITY_VIOLATION");
    }
    
    return core::Result<json>::Ok(todo.to_json());
}

/**
 * todo.list - 列出 TODO
 * 
 * 自动过滤:仅返回当前 Session 的 TODO
 */
core::Result<json> method_todo_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ========== 第 1 步: 权限检查 ==========
    auto perm_check = method_guard->check_permission(client_ctx, "todo.list");
    if (!perm_check.allowed) {
        return core::Result<json>::Err(perm_check.reason);
    }
    
    // ========== 第 2 步: 获取所有 TODO ==========
    auto state = server->runtime_state();
    std::lock_guard<std::mutex> lock(state->mutex);
    
    // ========== 第 3 步: 过滤 (仅返回当前 Session 的 TODO) ==========
    json result = json::array();
    
    for (const auto& [todo_id, todo] : state->todos) {
        // 🔐 Session 隔离:只返回当前 Session 的 TODO
        if (todo.session_id == client_ctx.session_id) {
            result.push_back(todo.to_json());
        }
    }
    
    return core::Result<json>::Ok(result);
}

/**
 * todo.update - 更新 TODO
 * 
 * 鉴权流程:
 * 1. 检查方法权限
 * 2. 检查资源访问权限
 * 3. 二次验证 session_id
 * 4. 更新字段
 * 5. 记录审计日志
 */
core::Result<json> method_todo_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ========== 第 1 步: 权限检查 ==========
    auto perm_check = method_guard->check_permission(client_ctx, "todo.update");
    if (!perm_check.allowed) {
        return core::Result<json>::Err(perm_check.reason);
    }
    
    // ========== 第 2 步: 参数验证 ==========
    if (!params.contains("todo_id") || !params["todo_id"].is_string()) {
        return core::Result<json>::Err("Missing or invalid 'todo_id' parameter");
    }
    
    std::string todo_id = params["todo_id"];
    
    // ========== 第 3 步: 资源访问检查 ==========
    auto access_check = method_guard->check_resource_access(
        client_ctx, "todo", todo_id
    );
    
    if (!access_check.allowed) {
        return core::Result<json>::Err("TODO_ACCESS_DENIED");
    }
    
    // ========== 第 4 步: 获取 TODO ==========
    auto state = server->runtime_state();
    std::lock_guard<std::mutex> lock(state->mutex);
    
    auto it = state->todos.find(todo_id);
    if (it == state->todos.end()) {
        return core::Result<json>::Err("TODO not found: " + todo_id);
    }
    
    auto& todo = it->second;
    
    // ========== 第 5 步: 二次验证 (防御性检查) ==========
    if (todo.session_id != client_ctx.session_id) {
        return core::Result<json>::Err("SECURITY_VIOLATION");
    }
    
    // ========== 第 6 步: 更新字段 ==========
    bool updated = false;
    
    if (params.contains("content") && params["content"].is_string()) {
        todo.content = params["content"];
        updated = true;
    }
    
    if (params.contains("status") && params["status"].is_string()) {
        std::string new_status = params["status"];
        
        // 验证状态有效性
        if (!todo_status::is_valid_status(new_status)) {
            return core::Result<json>::Err("Invalid status: " + new_status);
        }
        
        todo.status = new_status;
        updated = true;
    }
    
    if (params.contains("metadata") && params["metadata"].is_object()) {
        for (const auto& [key, value] : params["metadata"].items()) {
            if (value.is_string()) {
                todo.metadata[key] = value.get<std::string>();
            } else {
                todo.metadata[key] = value.dump();
            }
        }
        updated = true;
    }
    
    if (updated) {
        todo.updated_at = core::utils::now();
    }
    
    // ========== 第 7 步: 审计日志 ==========
    method_guard->log_audit(
        client_ctx,
        "todo.update",
        params,
        {{"todo_id", todo_id}, {"updated", updated}}
    );
    
    // ========== 第 8 步: 广播事件 ==========
    core::EventFrame event;
    event.event = "todo.updated";
    event.payload = todo.to_json();
    
    std::vector<std::string> target_conns;
    for (const auto& [conn_id, client] : state->clients) {
        if (client->session_id.value_or("") == client_ctx.session_id) {
            target_conns.push_back(conn_id);
        }
    }
    server->broadcast_to_conn_ids(target_conns, event);
    
    return core::Result<json>::Ok(todo.to_json());
}

/**
 * todo.delete - 删除 TODO
 * 
 * 鉴权流程:
 * 1. 检查方法权限
 * 2. 检查资源访问权限
 * 3. 二次验证 session_id
 * 4. 删除 TODO
 * 5. 清理资源所有权
 * 6. 记录审计日志
 */
core::Result<json> method_todo_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // ========== 第 1 步: 权限检查 ==========
    auto perm_check = method_guard->check_permission(client_ctx, "todo.delete");
    if (!perm_check.allowed) {
        return core::Result<json>::Err(perm_check.reason);
    }
    
    // ========== 第 2 步: 参数验证 ==========
    if (!params.contains("todo_id") || !params["todo_id"].is_string()) {
        return core::Result<json>::Err("Missing or invalid 'todo_id' parameter");
    }
    
    std::string todo_id = params["todo_id"];
    
    // ========== 第 3 步: 资源访问检查 ==========
    auto access_check = method_guard->check_resource_access(
        client_ctx, "todo", todo_id
    );
    
    if (!access_check.allowed) {
        return core::Result<json>::Err("TODO_ACCESS_DENIED");
    }
    
    // ========== 第 4 步: 获取 TODO ==========
    auto state = server->runtime_state();
    std::lock_guard<std::mutex> lock(state->mutex);
    
    auto it = state->todos.find(todo_id);
    if (it == state->todos.end()) {
        return core::Result<json>::Err("TODO not found: " + todo_id);
    }
    
    const auto& todo = it->second;
    
    // ========== 第 5 步: 二次验证 (防御性检查) ==========
    if (todo.session_id != client_ctx.session_id) {
        return core::Result<json>::Err("SECURITY_VIOLATION");
    }
    
    // ========== 第 6 步: 删除 TODO ==========
    json deleted_todo = todo.to_json();
    state->todos.erase(it);
    
    // ========== 第 7 步: 清理资源所有权 ==========
    method_guard->unregister_resource_ownership("todo", todo_id);
    
    // ========== 第 8 步: 审计日志 ==========
    method_guard->log_audit(
        client_ctx,
        "todo.delete",
        params,
        {{"todo_id", todo_id}, {"deleted", true}}
    );
    
    // ========== 第 9 步: 广播事件 ==========
    core::EventFrame event;
    event.event = "todo.deleted";
    event.payload = {{"todo_id", todo_id}};
    
    std::vector<std::string> target_conns;
    for (const auto& [conn_id, client] : state->clients) {
        if (client->session_id.value_or("") == client_ctx.session_id) {
            target_conns.push_back(conn_id);
        }
    }
    server->broadcast_to_conn_ids(target_conns, event);
    
    return core::Result<json>::Ok({
        {"ok", true},
        {"todo_id", todo_id},
        {"deleted_todo", deleted_todo}
    });
}

} // namespace gateway
} // namespace hunterclaw
