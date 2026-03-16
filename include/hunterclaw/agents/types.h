#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <map>

namespace hunterclaw {
namespace agents {

// ============================================================================
// Agent Configuration Types (OpenClaw-aligned)
// ============================================================================

/// Model configuration
struct ModelConfig {
    std::string provider = "anthropic";         // LLM provider (anthropic, openai, etc.)
    std::string model = "claude-3-5-sonnet-20241022";
    std::string api_key;                        // API key (or from env)
    std::string endpoint;                       // Custom endpoint (optional)
    int max_tokens = 4096;                      // Max output tokens
    double temperature = 1.0;                   // Temperature
    int context_tokens = 200000;                // Context window size
};

/// Agent identity (persona/role)
struct AgentIdentity {
    std::string name;                           // Agent name
    std::string role;                           // Agent role description
    std::string persona;                        // Persona/character
    std::vector<std::string> rules;            // Behavior rules
};

/// Subagent configuration
struct SubagentConfig {
    bool enabled = false;                       // Enable subagent spawning
    int max_concurrent = 3;                     // Max concurrent subagents
    int timeout_seconds = 900;                  // Default timeout (15 min)
    int max_depth = 5;                          // Max nesting depth
};

/// Complete agent configuration
struct AgentConfig {
    // Identity
    std::string id;                             // Unique agent ID
    bool is_default = false;                    // Is default agent
    std::string name;                           // Display name
    
    // Workspace
    std::string workspace;                      // Workspace directory
    std::string agent_dir;                      // Agent-specific files directory
    
    // Model
    ModelConfig model;                          // LLM model configuration
    
    // Skills
    std::vector<std::string> skills;           // Skill filter (wildcards supported)
    
    // Behavior
    std::string thinking_level = "medium";      // Thinking level (off, low, medium, high)
    int timeout_seconds = 600;                  // Execution timeout
    
    // Identity/Persona
    AgentIdentity identity;
    
    // Subagents
    SubagentConfig subagents;
    
    // Extra configuration
    std::map<std::string, core::JsonValue> extra;
};

// ============================================================================
// Agent Scope Helper Types
// ============================================================================

/// Agent context (workspace, agent dir, etc.)
struct AgentContext {
    std::string agent_id;
    std::string workspace_dir;
    std::string agent_dir;
    std::string skills_dir;
    std::string memory_dir;
    AgentConfig config;
};

/// Agent resolution result
struct AgentResolution {
    std::string agent_id;
    std::string session_key;
    AgentConfig config;
    AgentContext context;
};

// ============================================================================
// Helper Functions (declared, defined in scope.cc)
// ============================================================================

/// List all agent IDs from configuration
std::vector<std::string> list_agent_ids(const core::Config& config);

/// Resolve default agent ID
std::string resolve_default_agent_id(const core::Config& config);

/// Resolve agent ID from session key
std::string resolve_session_agent_id(
    const std::string& session_key,
    const core::Config& config
);

/// Resolve agent configuration by ID
core::Result<AgentConfig> resolve_agent_config(
    const core::Config& config,
    const std::string& agent_id
);

/// Resolve agent workspace directory
std::string resolve_agent_workspace_dir(
    const core::Config& config,
    const std::string& agent_id
);

/// Resolve agent directory
std::string resolve_agent_dir(
    const core::Config& config,
    const std::string& agent_id
);

/// Resolve skill filter for agent
std::vector<std::string> resolve_agent_skills_filter(
    const core::Config& config,
    const std::string& agent_id
);

/// Check if agent has specific skill enabled
bool is_skill_enabled_for_agent(
    const core::Config& config,
    const std::string& agent_id,
    const std::string& skill_id
);

/// Build agent context
core::Result<AgentContext> build_agent_context(
    const core::Config& config,
    const std::string& agent_id
);

/// Resolve agent from multiple sources
core::Result<AgentResolution> resolve_agent(
    const core::Config& config,
    const std::string& session_key,
    const std::string& explicit_agent_id = ""
);

// ============================================================================
// Agent Events (OpenClaw-aligned)
// ============================================================================

/**
 * Agent 生命周期阶段 (扩展)
 */
enum class AgentLifecyclePhase {
    // 实例状态
    INITIALIZING,    // 初始化中
    IDLE,            // 空闲
    PROCESSING,      // 处理中
    WAITING,         // 等待中
    TERMINATING,     // 终止中
    
    // ✅ OpenClaw 对齐: 执行阶段事件
    Start,           // Agent 启动
    End,             // Agent 结束
    Reasoning,       // 推理中 (thinking)
    Responding,      // 回复中 (assistant output streaming)
    ToolExecution    // 工具执行
};

/**
 * Agent 事件流类型 (OpenClaw 对齐)
 */
enum class AgentEventStream {
    Lifecycle,       // 生命周期事件
    Message,         // 消息事件
    Tool,            // 工具事件
    Memory,          // 内存事件
    Error            // 错误事件
};

/**
 * Agent 事件 (OpenClaw 对齐)
 * 参考: openclaw-analysis/src/agents/agent.ts
 */
struct AgentEvent {
    std::string run_id;              // 运行 ID
    int seq;                         // 事件序列号
    AgentEventStream stream;         // 事件流类型
    int64_t timestamp_ms;            // 时间戳 (毫秒)
    std::string session_key;         // Session Key
    core::JsonValue data;            // 事件数据
};

} // namespace agents
} // namespace hunterclaw
