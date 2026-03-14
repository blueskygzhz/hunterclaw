#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace hunterclaw {
namespace agents {

AgentPool::AgentPool(
    std::shared_ptr<core::Config> config,
    std::shared_ptr<skills::SkillManager> skill_mgr,
    std::shared_ptr<channels::ChannelManager> channel_mgr,
    const Config& pool_config
) : config_(config),
    skill_mgr_(skill_mgr),
    channel_mgr_(channel_mgr),
    pool_config_(pool_config) {
    
    spdlog::info("[AgentPool] Initialized: max_agents={}, idle_ttl={}s, auto_cleanup={}",
        pool_config_.max_agents,
        pool_config_.idle_ttl.count(),
        pool_config_.auto_cleanup);
    
    if (pool_config_.auto_cleanup) {
        start_auto_cleanup();
    }
}

AgentPool::~AgentPool() {
    stop_auto_cleanup();
    
    // 清理所有 Agent 实例
    std::lock_guard<std::mutex> lock(mutex_);
    spdlog::info("[AgentPool] Cleaning up {} agent instances", agents_.size());
    
    for (auto& [session_id, agent] : agents_) {
        try {
            agent->cleanup();
        } catch (const std::exception& e) {
            spdlog::error("[AgentPool] Cleanup error for session {}: {}", session_id, e.what());
        }
    }
    agents_.clear();
    last_active_.clear();
}

std::shared_ptr<AgentInstance> AgentPool::get_or_create_agent(
    const std::string& session_id,
    const std::optional<std::string>& agent_id_override
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // ========== Step 1: 检查是否已存在 ==========
    auto it = agents_.find(session_id);
    if (it != agents_.end()) {
        // 更新最后活跃时间
        last_active_[session_id] = core::utils::now();
        it->second->touch();
        
        spdlog::debug("[AgentPool] Reusing existing agent: session={}, agent={}",
            session_id, it->second->context().agent_id);
        
        return it->second;
    }
    
    // ========== Step 2: 检查实例数限制 ==========
    if (agents_.size() >= pool_config_.max_agents) {
        spdlog::warn("[AgentPool] Max agents reached: {}/{}, attempting cleanup",
            agents_.size(), pool_config_.max_agents);
        
        // 强制清理最老的 Agent
        size_t cleaned = cleanup_idle_agents(std::chrono::seconds(0));
        
        spdlog::info("[AgentPool] Cleaned {} idle agents", cleaned);
        
        // 还是超限 → 拒绝创建
        if (agents_.size() >= pool_config_.max_agents) {
            spdlog::error("[AgentPool] Cannot create agent: pool full ({}/{})",
                agents_.size(), pool_config_.max_agents);
            return nullptr;
        }
    }
    
    // ========== Step 3: 解析 Agent ID ==========
    std::string agent_id = resolve_agent_id(session_id, agent_id_override);
    
    spdlog::info("[AgentPool] Creating new agent: session={}, agent_id={}",
        session_id, agent_id);
    
    // ========== Step 4: 创建 Agent 实例 ==========
    try {
        auto agent = std::make_shared<AgentInstance>(
            session_id,
            agent_id,
            config_,
            skill_mgr_,
            channel_mgr_
        );
        
        // ========== Step 5: 注册到池中 ==========
        agents_[session_id] = agent;
        last_active_[session_id] = core::utils::now();
        
        spdlog::info("[AgentPool] Agent created: session={}, agent={}, workspace={}",
            session_id, agent_id, agent->context().workspace_dir);
        
        return agent;
        
    } catch (const std::exception& e) {
        spdlog::error("[AgentPool] Failed to create agent: session={}, agent={}, error={}",
            session_id, agent_id, e.what());
        return nullptr;
    }
}

std::shared_ptr<AgentInstance> AgentPool::get_agent(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = agents_.find(session_id);
    if (it != agents_.end()) {
        // 更新最后活跃时间
        last_active_[session_id] = core::utils::now();
        it->second->touch();
        return it->second;
    }
    
    return nullptr;
}

void AgentPool::remove_agent(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = agents_.find(session_id);
    if (it != agents_.end()) {
        spdlog::info("[AgentPool] Removing agent: session={}, agent={}",
            session_id, it->second->context().agent_id);
        
        try {
            // 清理 Agent 资源
            it->second->cleanup();
        } catch (const std::exception& e) {
            spdlog::error("[AgentPool] Cleanup error: {}", e.what());
        }
        
        agents_.erase(it);
        last_active_.erase(session_id);
    }
}

std::vector<AgentInstanceInfo> AgentPool::list_agents() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<AgentInstanceInfo> result;
    result.reserve(agents_.size());
    
    for (const auto& [session_id, agent] : agents_) {
        const auto& ctx = agent->context();
        
        AgentInstanceInfo info;
        info.session_id = session_id;
        info.agent_id = ctx.agent_id;
        info.workspace_dir = ctx.workspace_dir;
        info.created_at = ctx.created_at;
        info.last_active_at = last_active_[session_id];
        info.phase = ctx.phase;
        info.current_run_id = ctx.current_run_id;
        
        result.push_back(info);
    }
    
    return result;
}

AgentPool::Stats AgentPool::get_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Stats stats;
    stats.total_agents = agents_.size();
    stats.active_agents = 0;
    
    auto now = core::utils::now();
    
    for (const auto& [session_id, agent] : agents_) {
        const auto& ctx = agent->context();
        
        // 统计活跃 Agent (最近 5 分钟有活动)
        auto last_active = last_active_[session_id];
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_active);
        
        if (elapsed < std::chrono::seconds(300)) {
            stats.active_agents++;
        }
        
        // 按 agent_id 统计
        stats.agents_by_agent_id[ctx.agent_id]++;
    }
    
    return stats;
}

size_t AgentPool::cleanup_idle_agents(std::optional<std::chrono::seconds> ttl_override) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto ttl = ttl_override.value_or(pool_config_.idle_ttl);
    auto now = core::utils::now();
    
    std::vector<std::string> to_remove;
    
    for (const auto& [session_id, last_active] : last_active_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_active);
        
        if (elapsed > ttl) {
            to_remove.push_back(session_id);
        }
    }
    
    if (!to_remove.empty()) {
        spdlog::info("[AgentPool] Cleaning up {} idle agents (ttl={}s)",
            to_remove.size(), ttl.count());
        
        for (const auto& session_id : to_remove) {
            auto it = agents_.find(session_id);
            if (it != agents_.end()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_active_[session_id]
                );
                
                spdlog::debug("[AgentPool] Cleanup: session={}, agent={}, idle={}s",
                    session_id,
                    it->second->context().agent_id,
                    elapsed.count());
                
                try {
                    it->second->cleanup();
                } catch (const std::exception& e) {
                    spdlog::error("[AgentPool] Cleanup error: {}", e.what());
                }
                
                agents_.erase(it);
                last_active_.erase(session_id);
            }
        }
    }
    
    return to_remove.size();
}

void AgentPool::start_auto_cleanup() {
    if (cleanup_running_.load()) {
        return;
    }
    
    cleanup_running_.store(true);
    
    cleanup_thread_ = std::thread([this]() {
        spdlog::info("[AgentPool] Auto-cleanup thread started (interval={}s)",
            pool_config_.cleanup_interval.count());
        
        while (cleanup_running_.load()) {
            std::this_thread::sleep_for(pool_config_.cleanup_interval);
            
            if (cleanup_running_.load()) {
                try {
                    size_t cleaned = cleanup_idle_agents();
                    if (cleaned > 0) {
                        spdlog::info("[AgentPool] Auto-cleanup: removed {} idle agents", cleaned);
                    }
                } catch (const std::exception& e) {
                    spdlog::error("[AgentPool] Auto-cleanup error: {}", e.what());
                }
            }
        }
        
        spdlog::info("[AgentPool] Auto-cleanup thread stopped");
    });
}

void AgentPool::stop_auto_cleanup() {
    if (!cleanup_running_.load()) {
        return;
    }
    
    spdlog::info("[AgentPool] Stopping auto-cleanup thread");
    cleanup_running_.store(false);
    
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
}

// ========== 辅助方法 ==========

std::string AgentPool::resolve_agent_id(
    const std::string& session_id,
    const std::optional<std::string>& agent_id_override
) {
    // 策略 1: 显式指定
    if (agent_id_override && !agent_id_override->empty()) {
        spdlog::debug("[AgentPool] Using explicit agent_id: {}", *agent_id_override);
        return *agent_id_override;
    }
    
    // 策略 2: 从 Session 获取
    auto session_info = get_session_info(session_id);
    if (session_info && !session_info->agent_id.empty()) {
        spdlog::debug("[AgentPool] Using session.agent_id: {}", session_info->agent_id);
        return session_info->agent_id;
    }
    
    // 策略 3: 从 Session Key 推断 (如果 session 中有 metadata)
    if (session_info && session_info->metadata.count("session_key")) {
        std::string session_key = session_info->metadata.at("session_key");
        std::string inferred_agent = resolve_session_agent_id(session_key, *config_);
        if (!inferred_agent.empty()) {
            spdlog::debug("[AgentPool] Inferred agent from session_key: {}", inferred_agent);
            return inferred_agent;
        }
    }
    
    // 策略 4: 默认 Agent
    std::string default_agent = resolve_default_agent_id(*config_);
    spdlog::debug("[AgentPool] Using default agent: {}", default_agent);
    return default_agent;
}

std::optional<core::SessionInfo> AgentPool::get_session_info(
    const std::string& session_id
) {
    // ========== SessionManager 集成 (对齐 OpenClaw) ==========
    
    // TODO: 从全局 SessionManager 获取 Session 信息
    // 参考: openclaw-analysis/src/gateway/session-manager.ts
    
    // 方案 1: 全局单例
    // auto& session_mgr = gateway::SessionManager::instance();
    // return session_mgr.get_session(session_id);
    
    // 方案 2: 依赖注入 (构造函数传入 SessionManager)
    // if (session_mgr_) {
    //     return session_mgr_->get_session(session_id);
    // }
    
    // 方案 3: 通过 Config 查询 (简化版)
    // Session 信息可能存储在配置文件或数据库中
    
    spdlog::debug("[AgentPool] SessionManager not integrated yet, returning empty SessionInfo");
    
    // 暂时返回空 (后续集成 SessionManager)
    return std::nullopt;
}

} // namespace agents
} // namespace hunterclaw
