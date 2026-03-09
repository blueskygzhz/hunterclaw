#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/memory/compaction.h"
#include "hunterclaw/memory/pruning.h"
#include "hunterclaw/memory/memory.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <chrono>
#include <mutex>

namespace hunterclaw {
namespace agents {

// Forward declarations
class Agent;
class AgentManager;
struct AgentRunContext;

// ============================================================================
// Agent Configuration Types
// ============================================================================

/// Model configuration for agent
struct AgentModelConfig {
    std::string provider;           // "openai", "anthropic", "google", etc.
    std::string model;              // Model name (e.g., "claude-3-5-sonnet-20241022")
    std::string api_key;            // API key (can use env variable)
    std::string endpoint;           // Optional custom endpoint
    int max_tokens = 4096;          // Max output tokens
    double temperature = 0.7;       // Sampling temperature
    int context_tokens = 200000;    // Context window size
    std::vector<std::string> fallback_models;  // Fallback models on error
};

/// Memory search configuration
struct MemorySearchConfig {
    bool enabled = false;
    std::string endpoint;           // Remote memory service endpoint
    std::string api_key;            // Memory service API key
    std::map<std::string, std::string> providers;  // Custom providers
};

/// Human-like delay configuration
struct HumanDelayConfig {
    bool enabled = false;
    int min_delay_ms = 100;        // Minimum delay between actions
    int max_delay_ms = 500;        // Maximum delay between actions
};

/// Heartbeat configuration
struct HeartbeatConfig {
    bool enabled = true;
    int interval_ms = 30000;       // Send heartbeat every 30s
};

/// Identity configuration (system prompt additions)
struct IdentityConfig {
    std::string name;              // Agent name
    std::string role;              // Agent role description
    std::string persona;           // Personality traits
    std::vector<std::string> rules;  // Additional rules/instructions
};

/// Group chat configuration
struct GroupChatConfig {
    bool enabled = false;
    std::string mention_trigger;   // "@agent" trigger pattern
    bool auto_respond = false;     // Auto respond without mention
};

/// Subagent configuration
struct SubagentConfig {
    bool enabled = false;
    int max_concurrent = 3;        // Max concurrent subagents
    int timeout_seconds = 300;     // Subagent timeout
    std::vector<std::string> allowed_types;  // Allowed subagent types
};

/// Sandbox configuration
struct AgentSandboxConfig {
    std::string mode = "none";     // "none", "docker", "podman", "process"
    std::string image;             // Docker/Podman image
    std::map<std::string, std::string> env;  // Environment variables
    std::vector<std::string> mounts;  // Volume mounts
    int cpu_limit = 0;             // CPU limit (0 = no limit)
    int memory_limit_mb = 0;       // Memory limit in MB
};

/// Tools configuration
struct AgentToolsConfig {
    std::vector<std::string> enabled;   // Enabled tool names
    std::vector<std::string> disabled;  // Disabled tool names
    std::map<std::string, core::JsonValue> tool_configs;  // Per-tool configs
};

/// Complete agent configuration
struct AgentConfig {
    std::string id;                     // Unique agent ID
    bool is_default = false;            // Is this the default agent?
    std::string name;                   // Display name
    std::string workspace;              // Workspace directory path
    std::string agent_dir;              // Agent-specific directory
    
    // Core configurations
    AgentModelConfig model;
    std::vector<std::string> skills;    // Skill filter (empty = all)
    MemorySearchConfig memory_search;
    HumanDelayConfig human_delay;
    HeartbeatConfig heartbeat;
    IdentityConfig identity;
    GroupChatConfig group_chat;
    SubagentConfig subagents;
    AgentSandboxConfig sandbox;
    AgentToolsConfig tools;
    
    // Memory management configurations (对齐 OpenClaw)
    memory::CompactionConfig compaction;
    memory::PruningConfig pruning;
    memory::HistoryManager::HistoryConfig history;
    
    // Runtime settings
    std::string thinking_level = "normal";  // "off", "low", "normal", "high"
    std::string verbose_level = "normal";   // "off", "normal", "verbose"
    int timeout_seconds = 600;              // Agent execution timeout
};

// ============================================================================
// Agent Event System
// ============================================================================

enum class AgentEventStream {
    Lifecycle,      // Agent lifecycle events (start, end, pause, resume)
    Tool,           // Tool execution events
    Assistant,      // Assistant message events
    Error           // Error events
};

enum class AgentLifecyclePhase {
    Start,          // Agent started
    Running,        // Agent running
    Paused,         // Agent paused (waiting for input)
    Resumed,        // Agent resumed
    End             // Agent ended
};

enum class AgentStopReason {
    Complete,       // Task completed successfully
    Error,          // Error occurred
    Timeout,        // Execution timeout
    UserStop,       // User requested stop
    SessionReset,   // Session was reset
    SessionDelete   // Session was deleted
};

struct AgentEvent {
    std::string run_id;             // Run ID (unique per execution)
    uint64_t seq;                   // Sequence number (monotonic)
    AgentEventStream stream;        // Event stream
    uint64_t timestamp_ms;          // Timestamp in milliseconds
    std::string session_key;        // Optional session key
    core::JsonValue data;           // Event data (varies by stream/phase)
};

using AgentEventCallback = std::function<void(const AgentEvent&)>;

// ============================================================================
// Agent Run Context
// ============================================================================

struct AgentRunContext {
    std::string run_id;             // Unique run ID
    std::string session_key;        // Associated session key
    std::string agent_id;           // Agent ID
    uint64_t started_at;            // Start timestamp (ms)
    uint64_t updated_at;            // Last update timestamp (ms)
    std::string status;             // "running", "paused", "completed", "error"
    
    // Runtime state
    std::shared_ptr<skills::SkillManager> skill_manager;
    std::shared_ptr<channels::ChannelManager> channel_manager;
    std::map<std::string, std::string> variables;  // Runtime variables
    
    // Event tracking
    uint64_t next_seq = 0;          // Next event sequence number
    std::vector<AgentEvent> events; // Event history
};

// ============================================================================
// Agent Delivery Plan (for message routing)
// ============================================================================

struct AgentDeliveryPlan {
    std::string resolved_channel;          // Channel ID (e.g., "telegram")
    std::string delivery_target_mode;      // "explicit" or "implicit"
    std::string resolved_account_id;       // Account ID
    std::string resolved_to;               // Target user/chat ID
    std::string resolved_thread_id;        // Optional thread ID
};

// ============================================================================
// Agent Class
// ============================================================================

class Agent {
public:
    using Ptr = std::shared_ptr<Agent>;
    
    virtual ~Agent() = default;
    
    /// Get agent ID
    virtual std::string get_id() const = 0;
    
    /// Get agent configuration
    virtual const AgentConfig& get_config() const = 0;
    
    /// Run agent with a message
    /// @param message User message text
    /// @param options Additional run options
    /// @return Result containing run_id or error
    virtual core::Result<std::string> run(
        const std::string& message,
        const std::map<std::string, std::string>& options = {}
    ) = 0;
    
    /// Stop agent execution
    /// @param run_id Run ID to stop
    /// @param reason Stop reason
    virtual core::Result<void> stop(
        const std::string& run_id,
        AgentStopReason reason = AgentStopReason::UserStop
    ) = 0;
    
    /// Get current run context
    virtual std::shared_ptr<AgentRunContext> get_run_context(
        const std::string& run_id
    ) const = 0;
    
    /// List all active runs
    virtual std::vector<std::string> list_active_runs() const = 0;
    
    /// Register event callback
    virtual void on_event(AgentEventCallback callback) = 0;
    
    /// Build delivery plan for message
    virtual core::Result<AgentDeliveryPlan> build_delivery_plan(
        const std::string& channel_hint,
        const std::string& to_hint = ""
    ) const = 0;
};

// ============================================================================
// Default Agent Implementation
// ============================================================================

class DefaultAgent : public Agent {
public:
    static Ptr create(
        const AgentConfig& config,
        std::shared_ptr<skills::SkillManager> skill_mgr,
        std::shared_ptr<channels::ChannelManager> channel_mgr
    );
    
    DefaultAgent(
        const AgentConfig& config,
        std::shared_ptr<skills::SkillManager> skill_mgr,
        std::shared_ptr<channels::ChannelManager> channel_mgr
    );
    
    ~DefaultAgent() override = default;
    
    // Agent interface implementation
    std::string get_id() const override;
    const AgentConfig& get_config() const override;
    
    core::Result<std::string> run(
        const std::string& message,
        const std::map<std::string, std::string>& options = {}
    ) override;
    
    core::Result<void> stop(
        const std::string& run_id,
        AgentStopReason reason = AgentStopReason::UserStop
    ) override;
    
    std::shared_ptr<AgentRunContext> get_run_context(
        const std::string& run_id
    ) const override;
    
    std::vector<std::string> list_active_runs() const override;
    
    void on_event(AgentEventCallback callback) override;
    
    core::Result<AgentDeliveryPlan> build_delivery_plan(
        const std::string& channel_hint,
        const std::string& to_hint = ""
    ) const override;

private:
    AgentConfig config_;
    std::shared_ptr<skills::SkillManager> skill_manager_;
    std::shared_ptr<channels::ChannelManager> channel_manager_;
    
    // Runtime state
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<AgentRunContext>> active_runs_;
    std::vector<AgentEventCallback> event_callbacks_;
    
    // Helper methods
    std::string generate_run_id() const;
    void emit_event(const AgentEvent& event);
    void emit_lifecycle_event(
        const std::string& run_id,
        AgentLifecyclePhase phase,
        const core::JsonValue& data = core::JsonValue::null()
    );
    
    core::Result<std::string> build_system_prompt() const;
    core::Result<std::string> call_llm(
        const std::string& system_prompt,
        const std::string& user_message,
        const std::string& run_id
    );
};

// ============================================================================
// Agent Manager (manages multiple agents)
// ============================================================================

class AgentManager {
public:
    using Ptr = std::shared_ptr<AgentManager>;
    
    static Ptr create(
        const core::Config& config,
        std::shared_ptr<skills::SkillManager> skill_mgr,
        std::shared_ptr<channels::ChannelManager> channel_mgr
    );
    
    AgentManager(
        const core::Config& config,
        std::shared_ptr<skills::SkillManager> skill_mgr,
        std::shared_ptr<channels::ChannelManager> channel_mgr
    );
    
    ~AgentManager() = default;
    
    /// Register an agent
    core::Result<void> register_agent(const AgentConfig& config);
    
    /// Unregister an agent
    core::Result<void> unregister_agent(const std::string& agent_id);
    
    /// Get agent by ID
    Agent::Ptr get_agent(const std::string& agent_id) const;
    
    /// Get default agent
    Agent::Ptr get_default_agent() const;
    
    /// Resolve agent ID from session key
    std::string resolve_agent_id(const std::string& session_key) const;
    
    /// List all agent IDs
    std::vector<std::string> list_agents() const;
    
    /// Get agent configuration
    core::Result<AgentConfig> get_agent_config(const std::string& agent_id) const;
    
    /// Update agent configuration
    core::Result<void> update_agent_config(
        const std::string& agent_id,
        const AgentConfig& config
    );

private:
    core::Config base_config_;
    std::shared_ptr<skills::SkillManager> skill_manager_;
    std::shared_ptr<channels::ChannelManager> channel_manager_;
    
    mutable std::mutex mutex_;
    std::map<std::string, Agent::Ptr> agents_;
    std::string default_agent_id_;
    
    // Helper methods
    AgentConfig merge_with_defaults(const AgentConfig& config) const;
};

} // namespace agents
} // namespace hunterclaw
