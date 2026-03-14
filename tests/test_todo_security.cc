/**
 * 🔐 TODO 严格鉴权测试
 * 
 * 测试 Session 级别隔离和访问控制。
 */

#include <gtest/gtest.h>
#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/gateway/todo_types.h"

using namespace hunterclaw::gateway;

class TodoSecurityTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建 Gateway Server
        auto config = std::make_shared<core::Config>();
        config->gateway.port = 18789;
        server = std::make_shared<GatewayServer>(config);
        
        // 创建 AuthManager 和 MethodGuard
        auth_mgr = std::make_shared<AuthManager>(config);
        method_guard = std::make_shared<MethodGuard>(auth_mgr);
        method_guard->set_method_scopes(MethodGuard::get_default_method_scopes());
        
        // 创建两个不同的 Session Context
        session_a.client_id = "client-a";
        session_a.session_id = "session-a";
        session_a.device_id = "device-a";
        session_a.account_id = "account-a";
        session_a.authenticated = true;
        session_a.scopes = {"session.write", "session.read"};
        session_a.connected_at = core::utils::now();
        
        session_b.client_id = "client-b";
        session_b.session_id = "session-b";
        session_b.device_id = "device-b";
        session_b.account_id = "account-b";
        session_b.authenticated = true;
        session_b.scopes = {"session.write", "session.read"};
        session_b.connected_at = core::utils::now();
    }
    
    std::shared_ptr<GatewayServer> server;
    std::shared_ptr<AuthManager> auth_mgr;
    std::shared_ptr<MethodGuard> method_guard;
    
    ClientContext session_a;
    ClientContext session_b;
};

// ============================================================================
// 测试 1: Session 隔离 - 跨 Session 访问被拒绝
// ============================================================================

TEST_F(TodoSecurityTest, SessionIsolation) {
    // Session A 创建 TODO
    auto result_a = method_todo_create(
        server, method_guard, session_a,
        {{"content", "TODO from Session A"}}
    );
    
    ASSERT_TRUE(result_a.is_ok()) << "Failed to create TODO: " << result_a.error;
    std::string todo_id_a = result_a.value()["id"];
    
    // Session B 尝试访问 Session A 的 TODO
    auto result_b = method_todo_get(
        server, method_guard, session_b,
        {{"todo_id", todo_id_a}}
    );
    
    // ✅ 应该被拒绝
    ASSERT_TRUE(result_b.is_err());
    ASSERT_EQ(result_b.error, "TODO_ACCESS_DENIED");
}

// ============================================================================
// 测试 2: 列表过滤 - 仅返回当前 Session 的 TODO
// ============================================================================

TEST_F(TodoSecurityTest, ListFiltering) {
    // Session A 创建 2 个 TODO
    method_todo_create(server, method_guard, session_a, {{"content", "A1"}});
    method_todo_create(server, method_guard, session_a, {{"content", "A2"}});
    
    // Session B 创建 1 个 TODO
    method_todo_create(server, method_guard, session_b, {{"content", "B1"}});
    
    // Session A 列出 TODO
    auto result_a = method_todo_list(server, method_guard, session_a, {});
    ASSERT_TRUE(result_a.is_ok());
    auto todos_a = result_a.value();
    
    // ✅ 应该只看到自己的 TODO (2 个)
    ASSERT_EQ(todos_a.size(), 2);
    for (const auto& todo : todos_a) {
        ASSERT_EQ(todo["session_id"], "session-a");
    }
    
    // Session B 列出 TODO
    auto result_b = method_todo_list(server, method_guard, session_b, {});
    ASSERT_TRUE(result_b.is_ok());
    auto todos_b = result_b.value();
    
    // ✅ 应该只看到自己的 TODO (1 个)
    ASSERT_EQ(todos_b.size(), 1);
    ASSERT_EQ(todos_b[0]["session_id"], "session-b");
}

// ============================================================================
// 测试 3: 跨 Session 更新被拒绝
// ============================================================================

TEST_F(TodoSecurityTest, CrossSessionUpdateDenied) {
    // Session A 创建 TODO
    auto result_a = method_todo_create(
        server, method_guard, session_a,
        {{"content", "Original"}}
    );
    ASSERT_TRUE(result_a.is_ok());
    std::string todo_id = result_a.value()["id"];
    
    // Session B 尝试更新 Session A 的 TODO
    auto result_b = method_todo_update(
        server, method_guard, session_b,
        {{"todo_id", todo_id}, {"content", "Hacked!"}}
    );
    
    // ✅ 应该被拒绝
    ASSERT_TRUE(result_b.is_err());
    ASSERT_EQ(result_b.error, "TODO_ACCESS_DENIED");
    
    // 验证 TODO 未被修改
    auto result_get = method_todo_get(
        server, method_guard, session_a,
        {{"todo_id", todo_id}}
    );
    ASSERT_TRUE(result_get.is_ok());
    ASSERT_EQ(result_get.value()["content"], "Original");
}

// ============================================================================
// 测试 4: 跨 Session 删除被拒绝
// ============================================================================

TEST_F(TodoSecurityTest, CrossSessionDeleteDenied) {
    // Session A 创建 TODO
    auto result_a = method_todo_create(
        server, method_guard, session_a,
        {{"content", "Important TODO"}}
    );
    ASSERT_TRUE(result_a.is_ok());
    std::string todo_id = result_a.value()["id"];
    
    // Session B 尝试删除 Session A 的 TODO
    auto result_b = method_todo_delete(
        server, method_guard, session_b,
        {{"todo_id", todo_id}}
    );
    
    // ✅ 应该被拒绝
    ASSERT_TRUE(result_b.is_err());
    ASSERT_EQ(result_b.error, "TODO_ACCESS_DENIED");
    
    // 验证 TODO 仍然存在
    auto result_get = method_todo_get(
        server, method_guard, session_a,
        {{"todo_id", todo_id}}
    );
    ASSERT_TRUE(result_get.is_ok());
}

// ============================================================================
// 测试 5: 未鉴权访问被拒绝
// ============================================================================

TEST_F(TodoSecurityTest, UnauthenticatedAccessDenied) {
    // 未鉴权的客户端
    ClientContext unauthenticated;
    unauthenticated.client_id = "guest";
    unauthenticated.session_id = "guest-session";
    unauthenticated.authenticated = false;  // 未鉴权
    unauthenticated.scopes = {};  // 无权限
    
    // 尝试创建 TODO
    auto result = method_todo_create(
        server, method_guard, unauthenticated,
        {{"content", "Unauthorized TODO"}}
    );
    
    // ✅ 应该被拒绝
    ASSERT_TRUE(result.is_err());
    // 权限检查应该拒绝
}

// ============================================================================
// 测试 6: 缺少权限被拒绝
// ============================================================================

TEST_F(TodoSecurityTest, InsufficientPermissions) {
    // 客户端没有 session.write 权限
    ClientContext limited;
    limited.client_id = "limited-client";
    limited.session_id = "limited-session";
    limited.authenticated = true;
    limited.scopes = {"session.read"};  // 只有读权限,没有写权限
    
    // 尝试创建 TODO
    auto result = method_todo_create(
        server, method_guard, limited,
        {{"content", "Should fail"}}
    );
    
    // ✅ 应该被拒绝
    ASSERT_TRUE(result.is_err());
}

// ============================================================================
// 测试 7: 审计日志记录
// ============================================================================

TEST_F(TodoSecurityTest, AuditLogging) {
    // 创建 TODO
    method_todo_create(server, method_guard, session_a, {{"content", "Test"}});
    
    // 获取审计日志
    auto logs = method_guard->get_audit_logs(10);
    
    // ✅ 应该有日志记录
    ASSERT_GT(logs.size(), 0);
    
    // 验证日志内容
    auto last_log = logs.back();
    ASSERT_EQ(last_log["method"], "todo.create");
    ASSERT_EQ(last_log["client_id"], "client-a");
    ASSERT_EQ(last_log["session_id"], "session-a");
}

// ============================================================================
// 测试 8: 资源所有权注册
// ============================================================================

TEST_F(TodoSecurityTest, ResourceOwnership) {
    // 创建 TODO
    auto result = method_todo_create(
        server, method_guard, session_a,
        {{"content", "Test"}}
    );
    ASSERT_TRUE(result.is_ok());
    std::string todo_id = result.value()["id"];
    
    // 检查资源所有权
    auto ownership = method_guard->get_resource_ownership("todo", todo_id);
    ASSERT_TRUE(ownership.has_value());
    ASSERT_EQ(ownership->owner_session_id, "session-a");
    ASSERT_EQ(ownership->owner_device_id, "device-a");
    ASSERT_EQ(ownership->policy, ResourceAccessPolicy::SESSION_SCOPED);
}

// ============================================================================
// 测试 9: 正常流程 - 完整 CRUD
// ============================================================================

TEST_F(TodoSecurityTest, NormalCRUDFlow) {
    // 1. 创建
    auto create_result = method_todo_create(
        server, method_guard, session_a,
        {{"content", "Buy milk"}}
    );
    ASSERT_TRUE(create_result.is_ok());
    std::string todo_id = create_result.value()["id"];
    
    // 2. 读取
    auto get_result = method_todo_get(
        server, method_guard, session_a,
        {{"todo_id", todo_id}}
    );
    ASSERT_TRUE(get_result.is_ok());
    ASSERT_EQ(get_result.value()["content"], "Buy milk");
    
    // 3. 更新
    auto update_result = method_todo_update(
        server, method_guard, session_a,
        {{"todo_id", todo_id}, {"status", "completed"}}
    );
    ASSERT_TRUE(update_result.is_ok());
    ASSERT_EQ(update_result.value()["status"], "completed");
    
    // 4. 列表
    auto list_result = method_todo_list(server, method_guard, session_a, {});
    ASSERT_TRUE(list_result.is_ok());
    ASSERT_EQ(list_result.value().size(), 1);
    
    // 5. 删除
    auto delete_result = method_todo_delete(
        server, method_guard, session_a,
        {{"todo_id", todo_id}}
    );
    ASSERT_TRUE(delete_result.is_ok());
    
    // 6. 验证已删除
    auto final_list = method_todo_list(server, method_guard, session_a, {});
    ASSERT_TRUE(final_list.is_ok());
    ASSERT_EQ(final_list.value().size(), 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
