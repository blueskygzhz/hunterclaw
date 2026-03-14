#ifndef HUNTERCLAW_GATEWAY_GATEWAY_METHODS_H
#define HUNTERCLAW_GATEWAY_GATEWAY_METHODS_H

#include "hunterclaw/gateway/websocket_server.h"
#include <memory>
#include <functional>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// Gateway 方法处理器工厂 (对齐 OpenClaw server-methods.ts)
// ============================================================================

/**
 * @brief 创建所有核心 Gateway 方法处理器
 * 
 * 对齐 OpenClaw:
 * - gateway/server-methods.ts (coreGatewayHandlers)
 * - gateway/server-methods/*.ts (各个方法的实现)
 * 
 * 支持的方法 (39+):
 * 
 * 1. **connect** - 连接和认证
 *    - connect.auth
 *    - connect.device
 * 
 * 2. **health** - 健康检查
 *    - health
 *    - health.presence
 * 
 * 3. **chat** - AI 对话
 *    - chat
 *    - chat.abort
 * 
 * 4. **agent** - Agent 管理
 *    - agent.spawn
 *    - agent.identity.get
 *    - agent.identity.set
 * 
 * 5. **agents** - Agents 列表
 *    - agents.list
 *    - agents.create
 *    - agents.delete
 * 
 * 6. **sessions** - 会话管理
 *    - sessions.list
 *    - sessions.send
 *    - sessions.spawn
 *    - sessions.history
 *    - sessions.status
 * 
 * 7. **nodes** - 节点管理
 *    - nodes.list
 *    - nodes.invoke
 *    - nodes.status
 * 
 * 8. **cron** - 定时任务
 *    - cron.list
 *    - cron.create
 *    - cron.delete
 * 
 * 9. **config** - 配置管理
 *    - config.get
 *    - config.apply
 *    - config.patch
 * 
 * 10. **tools** - 工具目录
 *     - tools.catalog
 *     - tools.invoke
 * 
 * 11. **channels** - 频道管理
 *     - channels.list
 *     - channels.send
 * 
 * 12. **models** - 模型管理
 *     - models.list
 *     - models.catalog
 * 
 * 13. **logs** - 日志查询
 *     - logs.query
 * 
 * 14. **system** - 系统信息
 *     - system.info
 *     - system.restart
 * 
 * 15. **usage** - 使用统计
 *     - usage.get
 * 
 * 16. **其他**
 *     - doctor.run
 *     - wizard.start
 *     - update.run
 *     - tts.synthesize
 */
std::vector<std::shared_ptr<MethodHandler>> create_core_gateway_methods();

// ============================================================================
// 具体方法处理器类 (示例)
// ============================================================================

/**
 * @brief Health 健康检查
 * 
 * 请求: {}
 * 响应: { "status": "ok", "version": "1.0.0", "uptime_ms": 123456 }
 */
class HealthMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "health"; }
};

/**
 * @brief Chat 对话请求
 * 
 * 请求: {
 *   "messages": [...],
 *   "model": "gpt-4",
 *   "stream": true
 * }
 * 
 * 响应: {
 *   "run_id": "...",
 *   "status": "running"
 * }
 */
class ChatMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "chat"; }
};

/**
 * @brief Sessions List - 列出所有会话
 * 
 * 请求: { "include_system": false }
 * 响应: {
 *   "sessions": [
 *     { "session_id": "...", "name": "...", "created_at": 123 },
 *     ...
 *   ]
 * }
 */
class SessionsListMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "sessions.list"; }
};

/**
 * @brief Sessions Send - 向会话发送消息
 * 
 * 请求: {
 *   "session_id": "...",
 *   "message": "...",
 *   "wait": true
 * }
 * 
 * 响应: {
 *   "message_id": "...",
 *   "status": "sent"
 * }
 */
class SessionsSendMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "sessions.send"; }
};

/**
 * @brief Nodes List - 列出所有节点
 * 
 * 请求: {}
 * 响应: {
 *   "nodes": [
 *     { "node_id": "...", "status": "online", "platform": "android" },
 *     ...
 *   ]
 * }
 */
class NodesListMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "nodes.list"; }
};

/**
 * @brief Nodes Invoke - 调用节点命令
 * 
 * 请求: {
 *   "node_id": "...",
 *   "command": "...",
 *   "args": {...}
 * }
 * 
 * 响应: {
 *   "result": {...}
 * }
 */
class NodesInvokeMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "nodes.invoke"; }
};

/**
 * @brief Cron List - 列出所有定时任务
 * 
 * 请求: {}
 * 响应: {
 *   "jobs": [
 *     { "job_id": "...", "schedule": "0 0 * * *", "next_run": 123 },
 *     ...
 *   ]
 * }
 */
class CronListMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "cron.list"; }
};

/**
 * @brief Cron Create - 创建定时任务
 * 
 * 请求: {
 *   "schedule": "0 0 * * *",
 *   "command": "...",
 *   "args": {...}
 * }
 * 
 * 响应: {
 *   "job_id": "...",
 *   "created": true
 * }
 */
class CronCreateMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "cron.create"; }
};

/**
 * @brief Config Get - 获取配置
 * 
 * 请求: { "path": "agents.defaults.model" }
 * 响应: { "value": "gpt-4" }
 */
class ConfigGetMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "config.get"; }
};

/**
 * @brief Tools Catalog - 获取工具目录
 * 
 * 请求: {}
 * 响应: {
 *   "tools": [
 *     { "name": "camera_snap", "description": "...", "category": "media" },
 *     ...
 *   ]
 * }
 */
class ToolsCatalogMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "tools.catalog"; }
};

/**
 * @brief System Info - 获取系统信息
 * 
 * 请求: {}
 * 响应: {
 *   "version": "1.0.0",
 *   "platform": "linux",
 *   "uptime_ms": 123456,
 *   "memory_mb": 512
 * }
 */
class SystemInfoMethodHandler : public MethodHandler {
public:
    nlohmann::json handle(const RequestContext& ctx) override;
    std::string method_name() const override { return "system.info"; }
};

// ============================================================================
// Gateway Methods Router - 方法路由器
// ============================================================================

/**
 * @brief Gateway 方法路由器
 * 
 * 负责:
 * 1. 注册所有核心方法
 * 2. 方法权限验证
 * 3. Rate Limiting
 * 4. 方法调用统计
 */
class GatewayMethodsRouter {
public:
    GatewayMethodsRouter();
    ~GatewayMethodsRouter();
    
    /**
     * @brief 注册所有核心方法到 WebSocket Server
     * @param server WebSocket 服务器实例
     */
    void register_all_methods(WebSocketServer& server);
    
    /**
     * @brief 检查方法是否需要认证
     */
    bool requires_auth(const std::string& method) const;
    
    /**
     * @brief 检查方法是否需要管理员权限
     */
    bool requires_admin(const std::string& method) const;
    
    /**
     * @brief 获取方法统计信息
     */
    struct MethodStats {
        uint64_t total_calls{0};
        uint64_t success_calls{0};
        uint64_t error_calls{0};
        uint64_t total_duration_ms{0};
        double avg_duration_ms{0.0};
    };
    
    std::unordered_map<std::string, MethodStats> get_method_stats() const;
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gateway
} // namespace hunterclaw

#endif // HUNTERCLAW_GATEWAY_GATEWAY_METHODS_H
