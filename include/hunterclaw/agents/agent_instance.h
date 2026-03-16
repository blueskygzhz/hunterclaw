#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/agents/types.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/memory/memory_manager.h"  // ✅ 新增: MemoryManager 支持
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace hunterclaw {
namespace agents {

// Forward declarations
class ReactEngine;
class ToolRegistry;
class MemoryProvider;

/**
 * Agent 实例隔离上下文
 * 
 * 每个 Session 有独立的上下文:
 * - 独立 Workspace
 * - 独立 Memory
 * - 过滤后的 Tools
 */
struct AgentInstanceContext {
    std::string session_id;
    std::string agent_id;
    
    // 独立资源
    std::string workspace_dir;          // {workspace}/{agent_id}/sessions/{session_id}
    std::shared_ptr<MemoryProvider> memory;  // Session 级别 Memory
    std::vector<std::string> tools;     // Agent 配置过滤后的工具
    
    // 配置
    AgentConfig config;
    
    // 运行时状态
    std::optional<std::string> current_run_id;
    AgentLifecyclePhase phase = AgentLifecyclePhase::INITIALIZING;
    core::Timestamp created_at;
    core::Timestamp last_active_at;
    int event_sequence_ = 0;  // 事件序列号 (递增)
};

/**
 * AgentInstance - 独立的 Agent 实例
 * 
 * 每个 Session 对应一个 AgentInstance,完全隔离:
 * - Workspace 隔离
 * - Memory 隔离
 * - Tools 隔离
 * - 上下文隔离
 * 
 * 对齐 OpenClaw:
 * - src/commands/agent.ts (Agent 运行时)
 * - src/agents/agent-config.ts (Agent 配置)
 * 
 * 使用示例:
 * ```cpp
 * auto agent = std::make_shared<AgentInstance>(
 *     "session-123",
 *     "default",
 *     config,
 *     skill_mgr,
 *     channel_mgr
 * );
 * 
 * // 处理消息
 * auto response = agent->process_message("Hello!");
 * 
 * // 获取上下文
 * std::cout << "Workspace: " << agent->context().workspace_dir << std::endl;
 * ```
 */
class AgentInstance {
public:
    /**
     * 构造函数
     * 
     * @param session_id Session ID
     * @param agent_id Agent 配置 ID
     * @param config 全局配置
     * @param skill_mgr Skill 管理器
     * @param channel_mgr Channel 管理器
     */
    AgentInstance(
        const std::string& session_id,
        const std::string& agent_id,
        std::shared_ptr<core::Config> config,
        std::shared_ptr<skills::SkillManager> skill_mgr,
        std::shared_ptr<channels::ChannelManager> channel_mgr
    );
    
    ~AgentInstance();
    
    // ========== 核心方法 ==========
    
    /**
     * 处理用户消息
     * 
     * @param message 用户消息
     * @param options 可选参数 (channel_id, account_id, etc.)
     * @return Agent 响应 (或错误)
     */
    core::Result<std::string> process_message(
        const std::string& message,
        const std::map<std::string, std::string>& options = {}
    );
    
    /**
     * 获取隔离上下文
     */
    const AgentInstanceContext& context() const {
        return ctx_;
    }
    
    /**
     * 更新最后活跃时间
     */
    void touch() {
        ctx_.last_active_at = core::utils::now();
    }
    
    /**
     * 清理资源 (在 Agent 移除时调用)
     */
    void cleanup();
    
    // ========== 生命周期事件 ==========
    
    /**
     * 注册事件回调
     * TODO: Define AgentEvent type
     */
    // using EventCallback = std::function<void(const AgentEvent&)>;
    // void on_event(EventCallback callback);
    
private:
    AgentInstanceContext ctx_;
    
    // 组件
    std::shared_ptr<core::Config> config_;
    std::shared_ptr<skills::SkillManager> skill_mgr_;
    std::shared_ptr<channels::ChannelManager> channel_mgr_;
    std::shared_ptr<ReactEngine> react_engine_;
    std::shared_ptr<ToolRegistry> tool_registry_;
    
    // 事件回调
    // TODO: Define EventCallback type
    // std::vector<EventCallback> event_callbacks_;
    
    // 辅助方法
    void initialize_workspace();
    void initialize_memory();
    void initialize_tools();
    void emit_lifecycle_event(AgentLifecyclePhase phase, const core::JsonValue& data);
};

} // namespace agents
} // namespace hunterclaw
