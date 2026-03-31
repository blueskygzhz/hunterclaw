#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>
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
    
    spdlog::get("default")->info("[AgentPool] Initialized: max_agents={}, idle_ttl={}s, auto_cleanup={}",
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
    spdlog::get("default")->info("[AgentPool] Cleaning up {} agent instances", agents_.size());
    
    for (auto& [session_id, agent] : agents_) {
        try {
            agent->cleanup();
        } catch (const std::exception& e) {
            spdlog::get("default")->error("[AgentPool] Cleanup error for session {}: {}", session_id, e.what());
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
        
        spdlog::get("default")->debug("[AgentPool] Reusing existing agent: session={}, agent={}",
            session_id, it->second->context().agent_id);
        
        return it->second;
    }
    
    // ========== Step 2: 检查实例数限制 ==========
    if (agents_.size() >= pool_config_.max_agents) {
        spdlog::get("default")->warn("[AgentPool] Max agents reached: {}/{}, attempting cleanup",
            agents_.size(), pool_config_.max_agents);
        
        // 强制清理最老的 Agent
        size_t cleaned = cleanup_idle_agents(std::chrono::seconds(0));
        
        spdlog::get("default")->info("[AgentPool] Cleaned {} idle agents", cleaned);
        
        // 还是超限 → 拒绝创建
        if (agents_.size() >= pool_config_.max_agents) {
            spdlog::get("default")->error("[AgentPool] Cannot create agent: pool full ({}/{})",
                agents_.size(), pool_config_.max_agents);
            return nullptr;
        }
    }
    
    // ========== Step 3: 解析 Agent ID ==========
    std::string agent_id = resolve_agent_id(session_id, agent_id_override);
    
    spdlog::get("default")->info("[AgentPool] Creating new agent: session={}, agent_id={}",
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
        
        spdlog::get("default")->info("[AgentPool] Agent created: session={}, agent={}, workspace={}",
            session_id, agent_id, agent->context().workspace_dir);
        
        return agent;
        
    } catch (const std::exception& e) {
        spdlog::get("default")->error("[AgentPool] Failed to create agent: session={}, agent={}, error={}",
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

bool AgentPool::register_agent(
    const std::string& session_key,
    std::shared_ptr<AgentInstance> agent
) {
    if (session_key.empty() || !agent) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Do not overwrite an existing entry — caller must remove first.
    if (agents_.count(session_key)) {
        spdlog::get("default")->warn(
            "[AgentPool] register_agent: session_key already registered: {}", session_key);
        return false;
    }
    agents_[session_key] = agent;
    last_active_[session_key] = core::utils::now();
    spdlog::get("default")->debug(
        "[AgentPool] Registered isolated agent: session_key={}, agent_id={}",
        session_key, agent->context().agent_id);
    return true;
}

void AgentPool::remove_agent(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = agents_.find(session_id);
    if (it != agents_.end()) {
        spdlog::get("default")->info("[AgentPool] Removing agent: session={}, agent={}",
            session_id, it->second->context().agent_id);
        
        try {
            // 清理 Agent 资源
            it->second->cleanup();
        } catch (const std::exception& e) {
            spdlog::get("default")->error("[AgentPool] Cleanup error: {}", e.what());
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
        spdlog::get("default")->info("[AgentPool] Cleaning up {} idle agents (ttl={}s)",
            to_remove.size(), ttl.count());
        
        for (const auto& session_id : to_remove) {
            auto it = agents_.find(session_id);
            if (it != agents_.end()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_active_[session_id]
                );
                
                spdlog::get("default")->debug("[AgentPool] Cleanup: session={}, agent={}, idle={}s",
                    session_id,
                    it->second->context().agent_id,
                    elapsed.count());
                
                try {
                    it->second->cleanup();
                } catch (const std::exception& e) {
                    spdlog::get("default")->error("[AgentPool] Cleanup error: {}", e.what());
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
        spdlog::get("default")->info("[AgentPool] Auto-cleanup thread started (interval={}s)",
            pool_config_.cleanup_interval.count());
        
        while (cleanup_running_.load()) {
            std::this_thread::sleep_for(pool_config_.cleanup_interval);
            
            if (cleanup_running_.load()) {
                try {
                    size_t cleaned = cleanup_idle_agents();
                    if (cleaned > 0) {
                        spdlog::get("default")->info("[AgentPool] Auto-cleanup: removed {} idle agents", cleaned);
                    }
                } catch (const std::exception& e) {
                    spdlog::get("default")->error("[AgentPool] Auto-cleanup error: {}", e.what());
                }
            }
        }
        
        spdlog::get("default")->info("[AgentPool] Auto-cleanup thread stopped");
    });
}

void AgentPool::stop_auto_cleanup() {
    if (!cleanup_running_.load()) {
        return;
    }
    
    spdlog::get("default")->info("[AgentPool] Stopping auto-cleanup thread");
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
        spdlog::get("default")->debug("[AgentPool] Using explicit agent_id: {}", *agent_id_override);
        return *agent_id_override;
    }
    
    // 策略 2: 从 Session 获取
    auto session_info = get_session_info(session_id);
    if (session_info && session_info->metadata.count("agent_id")) {
        std::string agent_id_from_session = session_info->metadata.at("agent_id");
        if (!agent_id_from_session.empty()) {
            spdlog::get("default")->debug("[AgentPool] Using session.agent_id: {}", agent_id_from_session);
            return agent_id_from_session;
        }
    }
    
    // 策略 3: 从 Session Key 推断 (如果 session 中有 metadata)
    if (session_info && session_info->metadata.count("session_key")) {
        std::string session_key = session_info->metadata.at("session_key");
        std::string inferred_agent = resolve_session_agent_id(session_key, *config_);
        if (!inferred_agent.empty()) {
            spdlog::get("default")->debug("[AgentPool] Inferred agent from session_key: {}", inferred_agent);
            return inferred_agent;
        }
    }
    
    // 策略 4: 默认 Agent
    std::string default_agent = resolve_default_agent_id(*config_);
    spdlog::get("default")->debug("[AgentPool] Using default agent: {}", default_agent);
    return default_agent;
}

std::optional<core::SessionInfo> AgentPool::get_session_info(
    const std::string& session_id
) {
    // ========== SessionManager 集成 (对齐 OpenClaw) ==========
    // 
    // 实现策略:
    // 1. 优先从内存中的 Session 映射获取 (如果 SessionManager 已集成)
    // 2. 从配置文件中的 sessions 数据获取 (OpenClaw 存储在 .openclaw/sessions.json)
    // 3. 从 Agent 实例的元数据推断 (简化版)
    //
    // OpenClaw 参考:
    // - src/config/sessions.ts: loadSessionStore(), updateSessionStore()
    // - src/gateway/sessions-resolve.ts: resolveSessionKeyFromResolveParams()
    // - src/sessions/session-key.ts: parseSessionKey()
    // ========================================================================
    
    // Step 1: 检查是否已有对应的 Agent 实例 (可能包含 session 元数据)
    auto agent = get_agent(session_id);
    if (agent) {
        const auto& ctx = agent->context();
        
        // 构造 SessionInfo (从 Agent context 推断)
        core::SessionInfo info;
        info.session_id = session_id;
        
        // 推断 session_key (格式: "agent_id:session_name")
        // OpenClaw 格式: "main:default", "assistant:chat-123"
        std::string session_key = ctx.agent_id + ":session-" + session_id.substr(0, 8);
        info.key = core::SessionKey::from_string(session_key);
        
        info.created_at = ctx.created_at;
        info.last_active_at = last_active_[session_id];
        
        // 添加元数据
        info.metadata["agent_id"] = ctx.agent_id;
        info.metadata["workspace"] = ctx.workspace_dir;
        info.metadata["phase"] = std::to_string(static_cast<int>(ctx.phase));
        
        if (ctx.current_run_id) {
            info.metadata["run_id"] = *ctx.current_run_id;
        }
        
        spdlog::get("default")->debug("[AgentPool] Resolved SessionInfo from Agent context: session={}, agent={}, key={}",
            session_id, ctx.agent_id, session_key);
        
        return info;
    }
    
    // Step 2: 从持久化存储加载 (对齐 OpenClaw)
    // OpenClaw 路径: ~/.openclaw/agents/{agent_id}/sessions/sessions.json
    // HunterClaw 路径: ~/.hunterclaw/agents/{agent_id}/sessions/sessions.json
    //
    // 参考: openclaw-analysis/src/config/sessions/store.ts: loadSessionStore()
    //       openclaw-analysis/src/config/sessions/paths.ts: resolveDefaultSessionStorePath()
    
    spdlog::get("default")->debug("[AgentPool] Session not found in active agents, trying to load from disk: session={}", 
        session_id);
    
    try {
        // 构造 sessions.json 路径
        std::string state_dir = core::utils::get_home_dir() + "/.hunterclaw";
        std::string default_agent = resolve_default_agent_id(*config_);
        std::string sessions_path = state_dir + "/agents/" + default_agent + "/sessions/sessions.json";
        
        spdlog::get("default")->debug("[AgentPool] Attempting to load sessions from: {}", sessions_path);
        
        // 检查文件是否存在
        std::ifstream file(sessions_path);
        if (!file.is_open()) {
            spdlog::get("default")->debug("[AgentPool] Sessions file not found: {}", sessions_path);
            return std::nullopt;
        }
        
        // 读取 JSON 文件
        nlohmann::json sessions_store;
        try {
            file >> sessions_store;
        } catch (const std::exception& e) {
            spdlog::get("default")->warn("[AgentPool] Failed to parse sessions.json: {}", e.what());
            return std::nullopt;
        }
        
        // 查找对应的 session entry
        // OpenClaw 使用 session_key 作为键 (格式: "agent:id:name")
        // 我们需要推断或构造 session_key
        
        // 策略 1: 遍历所有 entries，查找匹配的 sessionId
        for (auto& [session_key, entry] : sessions_store.items()) {
            if (!entry.contains("sessionId")) continue;
            
            std::string stored_session_id = entry["sessionId"].get<std::string>();
            if (stored_session_id == session_id) {
                // 找到匹配的 entry，构造 SessionInfo
                core::SessionInfo info;
                info.session_id = session_id;
                info.key = core::SessionKey::from_string(session_key);
                
                // 填充时间戳
                if (entry.contains("createdAt")) {
                    info.created_at = core::Timestamp(
                        std::chrono::milliseconds(entry["createdAt"].get<int64_t>())
                    );
                }
                if (entry.contains("updatedAt")) {
                    info.last_active_at = core::Timestamp(
                        std::chrono::milliseconds(entry["updatedAt"].get<int64_t>())
                    );
                }
                
                // 填充元数据 (OpenClaw SessionEntry 的其他字段)
                if (entry.contains("agentId")) {
                    info.metadata["agent_id"] = entry["agentId"].get<std::string>();
                }
                if (entry.contains("displayName")) {
                    info.metadata["displayName"] = entry["displayName"].get<std::string>();
                }
                if (entry.contains("model")) {
                    info.metadata["model"] = entry["model"].get<std::string>();
                }
                if (entry.contains("modelProvider")) {
                    info.metadata["modelProvider"] = entry["modelProvider"].get<std::string>();
                }
                if (entry.contains("workspace")) {
                    info.metadata["workspace"] = entry["workspace"].get<std::string>();
                }
                
                spdlog::get("default")->info("[AgentPool] Loaded SessionInfo from disk: session={}, key={}, agent={}",
                    session_id, session_key, info.metadata["agent_id"]);
                
                return info;
            }
        }
        
        spdlog::get("default")->debug("[AgentPool] Session not found in sessions.json: session={}", session_id);
        
    } catch (const std::exception& e) {
        spdlog::get("default")->error("[AgentPool] Error loading session from disk: session={}, error={}",
            session_id, e.what());
    }
    
    // Step 3: 返回空 (Session 不存在或未激活)
    return std::nullopt;
}

} // namespace agents
} // namespace hunterclaw
