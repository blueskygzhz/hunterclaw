#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/agents/types.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/manager.h"
#include "hunterclaw/memory/memory_manager.h"  // ✅ 新增: MemoryManager 支持
#include "hunterclaw/react/react_engine.h"     // ✅ 新增: ReactEngine (直接包含，不使用前向声明)
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace hunterclaw {
namespace agents {

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
    std::shared_ptr<memory::MemoryProvider> memory;  // Session 级别 Memory (使用完整命名空间)
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
     * 注册事件回调 (OpenClaw 对齐)
     * 
     * @param callback 事件回调函数
     * 
     * 使用示例:
     * ```cpp
     * agent->on_event([](const AgentEvent& event) {
     *     if (event.stream == AgentEventStream::Lifecycle) {
     *         // 处理生命周期事件
     *     }
     * });
     * ```
     */
    using EventCallback = std::function<void(const AgentEvent&)>;
    void on_event(EventCallback callback);
    
    /**
     * 获取 ReactEngine (用于 Gateway 集成)
     */
    std::shared_ptr<react::ReactEngine> react_engine() const {
        return react_engine_;
    }
    
private:
    AgentInstanceContext ctx_;
    
    // 组件
    std::shared_ptr<core::Config> config_;
    std::shared_ptr<skills::SkillManager> skill_mgr_;
    std::shared_ptr<channels::ChannelManager> channel_mgr_;
    std::shared_ptr<react::ReactEngine> react_engine_;  // ✅ 使用完整命名空间
    std::shared_ptr<memory::MemoryManager> memory_mgr_;  // ✅ Memory 管理器
    
    // ✅ OpenClaw 对齐: Reasoning stream 状态跟踪
    std::string last_streamed_reasoning_;  // 用于去重和增量计算
    
    // ✅ OpenClaw 对齐: Assistant stream 状态跟踪
    // 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts:49-50
    std::string assistant_buffer_;           // 累积所有 assistant delta (对应 deltaBuffer)
    std::string last_streamed_assistant_;    // 上次发送的完整文本 (用于增量计算)
    std::chrono::steady_clock::time_point last_assistant_emit_time_;  // 节流时间戳
    
    // ✅ OpenClaw 对齐: Block reply 状态跟踪
    // 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts:104-115, 480-533
    std::string block_buffer_;               // 用于 block chunking 的缓冲区 (对应 blockBuffer)
    std::string last_block_reply_text_;      // 上次发送的 block reply (去重用)
    bool suppress_block_chunks_;             // 动态抑制 block chunks 发送
    std::vector<std::string> messaging_tool_sent_texts_;  // 已通过 messaging tool 发送的文本
    
    // ✅ OpenClaw 对齐: 事件回调系统
    std::vector<EventCallback> event_callbacks_;
    
    // 辅助方法
    void initialize_workspace();
    void initialize_memory();
    void initialize_tools();
    void emit_lifecycle_event(AgentLifecyclePhase phase, const core::JsonValue& data);
    
    // Overflow 重试循环 (对齐 OpenClaw)
    core::Result<std::string> process_message_with_overflow_retry(
        const std::string& message,
        const std::map<std::string, std::string>& options
    );
};

} // namespace agents
} // namespace hunterclaw
