#pragma once

#include "hunterclaw/agents/acp_protocol.h"
#include "hunterclaw/core/result.h"
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace agents {

// ============================================================================
// Agent 资源限制 (Resource Limits)
// ============================================================================

struct AcpResourceLimits {
    std::optional<size_t> max_memory_mb;         // 最大内存占用 (MB)
    std::optional<size_t> max_disk_mb;           // 最大磁盘占用 (MB)
    std::optional<uint32_t> max_threads;         // 最大线程数
    std::optional<uint32_t> max_file_descriptors;  // 最大文件描述符
    std::optional<std::chrono::milliseconds> max_execution_time;  // 最大执行时间
    std::optional<uint32_t> max_tool_calls;      // 最大工具调用数
    
    // 序列化
    core::JsonValue to_json() const;
    static core::Result<AcpResourceLimits> from_json(const core::JsonValue& json);
};

// ============================================================================
// Agent 作用域隔离 (Scope Isolation)
// ============================================================================

struct AcpScope {
    bool isolated_workspace = false;     // 独立工作目录
    bool isolated_memory = true;         // 独立 Memory 存储
    bool isolated_skills = false;        // 独立 Skill 注册表
    bool isolated_tools = false;         // 独立 Tool 注册表
    bool share_parent_context = true;    // 继承父 Agent 上下文
    
    std::optional<std::string> workspace_path;  // 自定义工作目录
    std::optional<std::string> memory_path;     // 自定义 Memory 路径
    
    core::JsonValue to_json() const;
    static core::Result<AcpScope> from_json(const core::JsonValue& json);
};

// ============================================================================
// Agent 生命周期钩子 (Lifecycle Hooks)
// ============================================================================

struct AcpLifecycleHooks {
    std::optional<std::string> on_start;      // Agent 启动时执行 (JS 代码)
    std::optional<std::string> on_message;    // 收到消息时执行
    std::optional<std::string> on_tool_call;  // 工具调用前执行
    std::optional<std::string> on_error;      // 错误发生时执行
    std::optional<std::string> on_end;        // Agent 结束时执行
    
    core::JsonValue to_json() const;
    static core::Result<AcpLifecycleHooks> from_json(const core::JsonValue& json);
};

// ============================================================================
// Agent 运行时选项 (Runtime Options)
// ============================================================================

struct AcpRuntimeOptions {
    std::optional<std::string> model;             // 覆盖默认模型
    std::optional<std::vector<std::string>> tools;  // 可用工具列表
    std::optional<std::map<std::string, std::string>> env_vars;  // 环境变量
    std::optional<std::string> system_prompt;     // 自定义系统提示
    std::optional<float> temperature;             // 温度参数
    std::optional<uint32_t> max_tokens;           // 最大 Token 数
    
    core::JsonValue to_json() const;
    static core::Result<AcpRuntimeOptions> from_json(const core::JsonValue& json);
};

// ============================================================================
// Agent 元数据 (Metadata)
// ============================================================================

struct SessionAcpMeta {
    std::string agent_id;                       // Agent 唯一标识
    std::string session_id;                     // 绑定的 Session ID
    std::string parent_agent_id;                // 父 Agent ID (Subagent 场景)
    
    AcpLabelMapping labels;                     // UI 标签映射
    AcpScope scope;                             // 作用域隔离
    AcpLifecycleHooks lifecycle;                // 生命周期钩子
    AcpResourceLimits limits;                   // 资源限制
    AcpRuntimeOptions runtime;                  // 运行时选项
    
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_active_at;
    
    core::JsonValue to_json() const;
    static core::Result<SessionAcpMeta> from_json(const core::JsonValue& json);
};

// ============================================================================
// Agent 实例 (Agent Instance)
// ============================================================================

struct AgentInstanceStatus {
    enum class State {
        Idle,           // 空闲
        Running,        // 运行中
        WaitingInput,   // 等待用户输入
        Paused,         // 暂停
        Error,          // 错误
        Terminated      // 已终止
    };
    
    State state = State::Idle;
    std::string session_id;
    std::optional<std::string> current_task;
    size_t message_count = 0;
    size_t tool_call_count = 0;
    std::chrono::milliseconds total_execution_time{0};
    
    // 资源使用情况
    size_t memory_usage_mb = 0;
    size_t disk_usage_mb = 0;
    uint32_t thread_count = 0;
    
    core::JsonValue to_json() const;
};

class AgentInstance {
public:
    AgentInstance(const SessionAcpMeta& meta);
    
    const SessionAcpMeta& metadata() const { return meta_; }
    AgentInstanceStatus status() const;
    
    // 生命周期管理
    core::Result<void> start();
    core::Result<void> pause();
    core::Result<void> resume();
    core::Result<void> terminate();
    
    // 消息处理
    core::Result<void> send_message(const AcpMessage& message);
    core::Result<std::vector<AcpMessage>> receive_messages();
    
    // 资源监控
    core::Result<void> check_resource_limits();
    
    // 生命周期钩子执行
    core::Result<void> execute_hook(const std::string& hook_name, const core::JsonValue& context);
    
private:
    SessionAcpMeta meta_;
    AgentInstanceStatus status_;
    std::chrono::system_clock::time_point start_time_;
    std::mutex mutex_;
};

// ============================================================================
// Agent 池 (Agent Pool) - 多租户管理
// ============================================================================

struct AgentPoolConfig {
    size_t max_agents = 100;                    // 最大 Agent 数
    std::chrono::seconds idle_timeout{300};     // 空闲超时 (5分钟)
    bool auto_cleanup = true;                   // 自动清理
    std::chrono::seconds cleanup_interval{60};  // 清理间隔 (1分钟)
};

class AgentPool {
public:
    explicit AgentPool(const AgentPoolConfig& config);
    ~AgentPool();
    
    // Agent 管理
    core::Result<std::shared_ptr<AgentInstance>> get_or_create_agent(
        const std::string& session_id,
        const std::optional<std::string>& agent_id = std::nullopt
    );
    
    core::Result<std::shared_ptr<AgentInstance>> create_agent(
        const SessionAcpMeta& meta
    );
    
    std::optional<std::shared_ptr<AgentInstance>> get_agent(const std::string& agent_id);
    core::Result<void> remove_agent(const std::string& agent_id);
    
    // 批量操作
    std::vector<std::string> list_agents() const;
    std::vector<std::shared_ptr<AgentInstance>> list_idle_agents(std::chrono::seconds idle_time) const;
    size_t cleanup_idle_agents(std::optional<std::chrono::seconds> ttl = std::nullopt);
    
    // 统计信息
    size_t agent_count() const;
    size_t running_agent_count() const;
    AgentPoolConfig config() const { return config_; }
    
    // 自动清理控制
    void start_auto_cleanup();
    void stop_auto_cleanup();
    
private:
    void cleanup_worker();
    
    AgentPoolConfig config_;
    std::map<std::string, std::shared_ptr<AgentInstance>> agents_;
    mutable std::mutex mutex_;
    
    // 自动清理
    std::atomic<bool> cleanup_running_{false};
    std::unique_ptr<std::thread> cleanup_thread_;
};

// ============================================================================
// Agent 消息路由器 (Message Router)
// ============================================================================

class AcpMessageRouter {
public:
    explicit AcpMessageRouter(std::shared_ptr<AgentPool> pool);
    
    // 消息路由
    core::Result<void> route_message(const AcpMessage& message);
    
    // 广播消息 (发送给所有 Agent)
    core::Result<void> broadcast_message(const AcpMessage& message);
    
    // 注册消息处理器
    using MessageHandler = std::function<core::Result<void>(const AcpMessage&)>;
    void register_handler(const std::string& message_type, MessageHandler handler);
    
private:
    std::shared_ptr<AgentPool> pool_;
    std::map<std::string, MessageHandler> handlers_;
    std::mutex mutex_;
};

// ============================================================================
// Agent 协作管理器 (Collaboration Manager)
// ============================================================================

struct TeamConfig {
    std::string team_id;
    std::string team_name;
    std::vector<std::string> member_agent_ids;
    std::optional<std::string> leader_agent_id;
    
    // 共享资源
    bool shared_memory = false;
    bool shared_workspace = false;
    
    core::JsonValue to_json() const;
    static core::Result<TeamConfig> from_json(const core::JsonValue& json);
};

class AgentCollaborationManager {
public:
    explicit AgentCollaborationManager(std::shared_ptr<AgentPool> pool);
    
    // 团队管理
    core::Result<void> create_team(const TeamConfig& config);
    core::Result<void> add_agent_to_team(const std::string& team_id, const std::string& agent_id);
    core::Result<void> remove_agent_from_team(const std::string& team_id, const std::string& agent_id);
    core::Result<void> delete_team(const std::string& team_id);
    
    std::optional<TeamConfig> get_team(const std::string& team_id) const;
    std::vector<TeamConfig> list_teams() const;
    
    // 任务委托
    core::Result<std::string> delegate_task(
        const std::string& from_agent_id,
        const std::string& to_agent_id,
        const std::string& task_description,
        const core::JsonValue& context
    );
    
    // 团队通信
    core::Result<void> send_team_message(
        const std::string& team_id,
        const std::string& from_agent_id,
        const std::string& content
    );
    
private:
    std::shared_ptr<AgentPool> pool_;
    std::map<std::string, TeamConfig> teams_;
    mutable std::mutex mutex_;
};

}  // namespace agents
}  // namespace hunterclaw
