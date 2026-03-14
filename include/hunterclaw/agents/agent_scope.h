#pragma once

#include "hunterclaw/config/config_system.h"
#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <vector>
#include <optional>
#include <unordered_set>

namespace hunterclaw {
namespace agents {

// ============================================================================
// 🎯 OpenClaw 对齐: agents/agent-scope.ts
// ============================================================================
// Agent Scope 系统负责管理 Agent 的配置、识别和权限范围
// 严格对齐 OpenClaw 的 agent-scope.ts 实现
// ============================================================================

// Agent Entry - 单个 Agent 的配置条目
struct AgentEntry {
    std::string id;                                // Agent ID (normalized)
    std::string name;                              // Agent 显示名称
    bool is_default = false;                       // 是否为默认 Agent
    std::string workspace;                         // Workspace 目录
    std::string agent_dir;                         // Agent 特定目录
    
    // Model configuration
    std::optional<std::string> model_primary;      // Primary model
    std::vector<std::string> model_fallbacks;      // Model fallbacks
    
    // Skills configuration
    std::vector<std::string> skills;               // 技能过滤器 (["*"], ["skill1", "skill2"], etc.)
    
    // Feature flags
    struct {
        std::optional<bool> memory_search;         // Memory search enabled
        std::optional<int> human_delay_ms;         // Human typing delay
        std::optional<bool> heartbeat_enabled;     // Heartbeat enabled
    } features;
    
    // Identity & Chat settings
    struct {
        std::optional<std::string> identity_name;  // Agent identity name
        std::optional<bool> group_chat_enabled;    // Group chat support
    } identity;
    
    // Subagent settings
    struct {
        std::optional<int> max_spawn_depth;        // Max subagent spawn depth
        std::optional<bool> allow_spawn;           // Allow spawning subagents
    } subagents;
    
    // Sandbox settings
    struct {
        std::optional<bool> enabled;               // Sandbox enabled
        std::optional<std::string> mode;           // Sandbox mode
    } sandbox;
    
    // Tools settings
    struct {
        std::optional<bool> exec_enabled;          // Allow exec tool
        std::optional<std::vector<std::string>> disabled_tools;  // Disabled tools
    } tools;
};

// Resolved Agent Config - 解析后的 Agent 配置
struct ResolvedAgentConfig {
    std::string id;
    std::string name;
    std::string workspace;
    std::string agent_dir;
    
    // Model settings
    std::optional<std::string> model_primary;
    std::vector<std::string> model_fallbacks;
    
    // Skills
    std::vector<std::string> skills_filter;
    
    // Features
    std::optional<bool> memory_search;
    std::optional<int> human_delay_ms;
    std::optional<bool> heartbeat_enabled;
    
    // Identity
    std::optional<std::string> identity_name;
    std::optional<bool> group_chat_enabled;
    
    // Subagents
    std::optional<int> max_spawn_depth;
    std::optional<bool> allow_spawn;
    
    // Sandbox
    std::optional<bool> sandbox_enabled;
    std::optional<std::string> sandbox_mode;
    
    // Tools
    std::optional<bool> exec_enabled;
    std::vector<std::string> disabled_tools;
};

// Session Agent IDs - 会话 Agent ID 解析结果
struct SessionAgentIds {
    std::string default_agent_id;    // 默认 Agent ID
    std::string session_agent_id;    // 当前会话 Agent ID
};

// ============================================================================
// Agent Scope 管理器
// ============================================================================

class AgentScopeManager {
public:
    explicit AgentScopeManager(const core::Config& config);
    ~AgentScopeManager() = default;
    
    // ========================================================================
    // 🎯 OpenClaw 对齐: Basic Agent Resolution
    // ========================================================================
    
    /// List all agent entries from config
    /// 对齐: listAgentEntries(cfg)
    std::vector<AgentEntry> list_agent_entries() const;
    
    /// List all agent IDs
    /// 对齐: listAgentIds(cfg)
    std::vector<std::string> list_agent_ids() const;
    
    /// Resolve default agent ID
    /// 对齐: resolveDefaultAgentId(cfg)
    std::string resolve_default_agent_id() const;
    
    /// Resolve agent entry by ID
    /// 对齐: resolveAgentEntry(cfg, agentId)
    std::optional<AgentEntry> resolve_agent_entry(const std::string& agent_id) const;
    
    /// Resolve agent config by ID
    /// 对齐: resolveAgentConfig(cfg, agentId)
    std::optional<ResolvedAgentConfig> resolve_agent_config(const std::string& agent_id) const;
    
    // ========================================================================
    // 🎯 OpenClaw 对齐: Session Agent Resolution
    // ========================================================================
    
    /// Resolve session agent IDs from session key
    /// 对齐: resolveSessionAgentIds({ sessionKey, config, agentId })
    SessionAgentIds resolve_session_agent_ids(
        const std::string& session_key,
        const std::optional<std::string>& explicit_agent_id = std::nullopt
    ) const;
    
    /// Resolve session agent ID (simplified)
    /// 对齐: resolveSessionAgentId({ sessionKey, config })
    std::string resolve_session_agent_id(const std::string& session_key) const;
    
    /// Resolve agent ID from session key format
    /// 对齐: resolveAgentIdFromSessionKey(sessionKey)
    std::string resolve_agent_id_from_session_key(const std::string& session_key) const;
    
    // ========================================================================
    // 🎯 OpenClaw 对齐: Model Configuration
    // ========================================================================
    
    /// Resolve explicit model primary for agent
    /// 对齐: resolveAgentExplicitModelPrimary(cfg, agentId)
    std::optional<std::string> resolve_agent_explicit_model_primary(
        const std::string& agent_id
    ) const;
    
    /// Resolve effective model primary (with defaults fallback)
    /// 对齐: resolveAgentEffectiveModelPrimary(cfg, agentId)
    std::optional<std::string> resolve_agent_effective_model_primary(
        const std::string& agent_id
    ) const;
    
    /// Resolve model fallbacks override for agent
    /// 对齐: resolveAgentModelFallbacksOverride(cfg, agentId)
    std::optional<std::vector<std::string>> resolve_agent_model_fallbacks_override(
        const std::string& agent_id
    ) const;
    
    /// Resolve effective model fallbacks
    /// 对齐: resolveEffectiveModelFallbacks({ cfg, agentId, hasSessionModelOverride })
    std::optional<std::vector<std::string>> resolve_effective_model_fallbacks(
        const std::string& agent_id,
        bool has_session_model_override
    ) const;
    
    /// Check if agent has configured model fallbacks
    /// 对齐: hasConfiguredModelFallbacks({ cfg, agentId, sessionKey })
    bool has_configured_model_fallbacks(
        const std::string& agent_id,
        const std::string& session_key = ""
    ) const;
    
    // ========================================================================
    // 🎯 OpenClaw 对齐: Skills Configuration
    // ========================================================================
    
    /// Resolve skills filter for agent
    /// 对齐: resolveAgentSkillsFilter(cfg, agentId)
    std::vector<std::string> resolve_agent_skills_filter(const std::string& agent_id) const;
    
    /// Check if skill is enabled for agent
    bool is_skill_enabled_for_agent(
        const std::string& agent_id,
        const std::string& skill_id
    ) const;
    
    /// Check if skill matches filter pattern
    bool skill_matches_filter(
        const std::string& skill_id,
        const std::vector<std::string>& filter
    ) const;
    
    // ========================================================================
    // 🎯 OpenClaw 对齐: Workspace & Directory Resolution
    // ========================================================================
    
    /// Resolve agent workspace directory
    /// 对齐: resolveAgentWorkspaceDir(cfg, agentId)
    std::string resolve_agent_workspace_dir(const std::string& agent_id) const;
    
    /// Resolve agent directory (for agent-specific state)
    /// 对齐: resolveAgentDir(cfg, agentId)
    std::string resolve_agent_dir(const std::string& agent_id) const;
    
    /// Resolve agent IDs by workspace path (for workspace → agent mapping)
    /// 对齐: resolveAgentIdsByWorkspacePath(cfg, workspacePath)
    std::vector<std::string> resolve_agent_ids_by_workspace_path(
        const std::string& workspace_path
    ) const;
    
    /// Resolve single agent ID by workspace path
    /// 对齐: resolveAgentIdByWorkspacePath(cfg, workspacePath)
    std::optional<std::string> resolve_agent_id_by_workspace_path(
        const std::string& workspace_path
    ) const;
    
    // ========================================================================
    // 🎯 OpenClaw 对齐: Utility Methods
    // ========================================================================
    
    /// Normalize agent ID (lowercase, trim)
    /// 对齐: normalizeAgentId(agentId)
    static std::string normalize_agent_id(const std::string& agent_id);
    
    /// Parse agent session key format ("agentId:sessionName")
    /// 对齐: parseAgentSessionKey(sessionKey)
    struct ParsedSessionKey {
        std::string agent_id;
        std::string session_name;
    };
    static std::optional<ParsedSessionKey> parse_agent_session_key(
        const std::string& session_key
    );
    
    /// Strip null bytes from paths (security)
    /// 对齐: stripNullBytes(s)
    static std::string strip_null_bytes(const std::string& path);
    
    /// Normalize path for comparison (resolve symlinks, case-insensitive on Windows)
    /// 对齐: normalizePathForComparison(input)
    static std::string normalize_path_for_comparison(const std::string& path);
    
    /// Check if path is within root directory
    /// 对齐: isPathWithinRoot(candidatePath, rootPath)
    static bool is_path_within_root(
        const std::string& candidate_path,
        const std::string& root_path
    );
    
    // ========================================================================
    // Config Management
    // ========================================================================
    
    /// Update config
    void update_config(const core::Config& config);
    
    /// Get current config
    const core::Config& get_config() const { return config_; }
    
private:
    core::Config config_;
    mutable bool default_agent_warned_ = false;
    
    // Helper: resolve agent entry from config
    std::optional<AgentEntry> find_agent_entry_in_config(const std::string& agent_id) const;
    
    // Helper: load AgentEntry from config JSON
    AgentEntry load_agent_entry_from_json(const core::JsonValue& json) const;
    
    // Helper: resolve default workspace dir (from env)
    std::string resolve_default_agent_workspace_dir() const;
    
    // Helper: expand user path (~, $HOME, etc.)
    std::string expand_user_path(const std::string& path) const;
};

// ============================================================================
// 全局辅助函数 (对齐 OpenClaw 的顶层函数)
// ============================================================================

/// Resolve fallback agent ID from params
/// 对齐: resolveFallbackAgentId({ agentId, sessionKey })
std::string resolve_fallback_agent_id(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& session_key
);

/// Resolve run model fallbacks override
/// 对齐: resolveRunModelFallbacksOverride({ cfg, agentId, sessionKey })
std::optional<std::vector<std::string>> resolve_run_model_fallbacks_override(
    const core::Config& config,
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& session_key
);

}  // namespace agents
}  // namespace hunterclaw
