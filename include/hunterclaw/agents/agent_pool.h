#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/agents/types.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/manager.h"
#include <memory>
#include <map>
#include <vector>
#include <optional>
#include <thread>
#include <atomic>
#include <mutex>

namespace hunterclaw {
namespace agents {

// Forward declaration
class AgentInstance;

/**
 * Agent 实例信息
 */
struct AgentInstanceInfo {
    std::string session_id;
    std::string agent_id;
    std::string workspace_dir;
    core::Timestamp created_at;
    core::Timestamp last_active_at;
    AgentLifecyclePhase phase;
    std::optional<std::string> current_run_id;
};

/**
 * AgentPool - 多租户 Agent 实例管理器
 * 
 * 功能:
 * 1. Session → Agent 实例映射
 * 2. 自动创建和清理 Agent 实例
 * 3. 资源隔离 (Workspace, Memory, Tools)
 * 4. 闲置 Agent 清理
 * 
 * 对齐 OpenClaw:
 * - src/commands/agent.ts (Agent 运行时)
 * - src/gateway/session-manager.ts (Session 管理)
 * 
 * 使用示例:
 * ```cpp
 * auto agent_pool = std::make_shared<AgentPool>(config, skill_mgr, channel_mgr);
 * 
 * // 获取或创建 Agent
 * auto agent = agent_pool->get_or_create_agent("session-123");
 * 
 * // 处理消息
 * auto response = agent->process_message("Hello!");
 * 
 * // 清理
 * agent_pool->remove_agent("session-123");
 * ```
 */
class AgentPool {
public:
    struct Config {
        std::chrono::seconds idle_ttl;       // 1 小时闲置自动清理
        size_t max_agents;                    // 最大 Agent 实例数
        bool auto_cleanup;                    // 自动清理闲置 Agent
        std::chrono::seconds cleanup_interval; // 清理间隔 (5 分钟)
        
        // 默认构造函数
        Config() 
            : idle_ttl(3600)
            , max_agents(100)
            , auto_cleanup(true)
            , cleanup_interval(300) {}
    };
    
    /**
     * 构造函数
     * 
     * @param config 全局配置
     * @param skill_mgr Skill 管理器
     * @param channel_mgr Channel 管理器
     * @param pool_config AgentPool 配置
     */
    explicit AgentPool(
        std::shared_ptr<core::Config> config,
        std::shared_ptr<skills::SkillManager> skill_mgr,
        std::shared_ptr<channels::ChannelManager> channel_mgr,
        const Config& pool_config = Config()
    );
    
    ~AgentPool();
    
    // ========== 核心方法 ==========
    
    /**
     * 获取或创建 Agent 实例
     * 
     * 映射逻辑:
     * 1. 已存在 → 返回现有实例
     * 2. 不存在 → 创建新实例 (解析 agent_id)
     * 
     * @param session_id Session ID (必须)
     * @param agent_id Agent 配置 ID (可选,默认从 Session 推断)
     * @return Agent 实例 (失败返回 nullptr)
     */
    std::shared_ptr<AgentInstance> get_or_create_agent(
        const std::string& session_id,
        const std::optional<std::string>& agent_id = std::nullopt
    );
    
    /**
     * 获取现有 Agent 实例 (不创建)
     * 
     * @param session_id Session ID
     * @return Agent 实例 (不存在返回 nullptr)
     */
    std::shared_ptr<AgentInstance> get_agent(const std::string& session_id);
    
    /**
     * 移除 Agent 实例
     * 
     * @param session_id Session ID
     */
    void remove_agent(const std::string& session_id);
    
    /**
     * 列出所有活跃 Agent
     * 
     * @return Agent 实例信息列表
     */
    std::vector<AgentInstanceInfo> list_agents();
    
    /**
     * 获取统计信息
     */
    struct Stats {
        size_t total_agents;
        size_t active_agents;  // 最近 5 分钟有活动
        std::map<std::string, size_t> agents_by_agent_id;
    };
    Stats get_stats();
    
    // ========== 生命周期管理 ==========
    
    /**
     * 清理闲置 Agent
     * 
     * @param ttl 闲置时间阈值 (默认使用 pool_config.idle_ttl)
     * @return 清理的 Agent 数量
     */
    size_t cleanup_idle_agents(
        std::optional<std::chrono::seconds> ttl = std::nullopt
    );
    
    /**
     * 启动自动清理线程
     */
    void start_auto_cleanup();
    
    /**
     * 停止自动清理线程
     */
    void stop_auto_cleanup();
    
    /**
     * 是否正在运行自动清理
     */
    bool is_auto_cleanup_running() const {
        return cleanup_running_.load();
    }
    
private:
    // session_id → Agent 实例
    std::map<std::string, std::shared_ptr<AgentInstance>> agents_;
    
    // session_id → 最后活跃时间
    std::map<std::string, core::Timestamp> last_active_;
    
    // 配置
    std::shared_ptr<core::Config> config_;
    std::shared_ptr<skills::SkillManager> skill_mgr_;
    std::shared_ptr<channels::ChannelManager> channel_mgr_;
    Config pool_config_;
    
    // 自动清理线程
    std::thread cleanup_thread_;
    std::atomic<bool> cleanup_running_{false};
    
    mutable std::mutex mutex_;
    
    // 辅助方法
    std::string resolve_agent_id(
        const std::string& session_id,
        const std::optional<std::string>& agent_id_override
    );
    
    std::optional<core::SessionInfo> get_session_info(
        const std::string& session_id
    );
};

} // namespace agents
} // namespace hunterclaw
