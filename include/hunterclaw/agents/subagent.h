#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/agents/types.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <atomic>

namespace hunterclaw {
namespace agents {

// Forward declarations
class AgentInstance;
class AgentPool;
// AgentManager is an alias for AgentPool (HunterClaw uses AgentPool)
using AgentManager = AgentPool;

// Type aliases for forward-declared types
using AgentPtr = std::shared_ptr<AgentInstance>;

// ============================================================================
// Subagent Types & Configuration
// ============================================================================

/// Subagent spawn depth info
struct SubagentDepthInfo {
    int depth = 0;                          // Current nesting depth (0 = top-level)
    int max_depth = 5;                      // Maximum allowed depth
    std::vector<std::string> ancestry;      // Parent run IDs chain
    std::string root_run_id;                // Root (top-level) run ID
    
    bool can_spawn_child() const {
        return depth < max_depth;
    }
    
    SubagentDepthInfo child_depth(const std::string& child_run_id) const {
        SubagentDepthInfo child;
        child.depth = depth + 1;
        child.max_depth = max_depth;
        child.ancestry = ancestry;
        child.ancestry.push_back(child_run_id);
        child.root_run_id = root_run_id.empty() ? child_run_id : root_run_id;
        return child;
    }
};

/// Subagent spawn options
struct SubagentSpawnOptions {
    std::string subagent_name;              // Subagent type name (e.g., "code-explorer")
    std::string description;                // Short task description (3-5 words)
    std::string prompt;                     // Full task prompt for subagent
    
    // Optional overrides
    std::string model;                      // Override model
    std::string thinking_level;             // Override thinking level
    int timeout_seconds = 0;                // Override timeout (0 = use default)
    
    // Context
    std::map<std::string, std::string> variables;  // Additional variables
    std::vector<std::string> attachments;   // File paths to attach
    
    // Control
    bool wait_for_completion = false;       // Block until subagent completes
    bool inherit_context = true;            // Inherit parent context
};

/// Subagent lifecycle state
enum class SubagentState {
    Spawning,       // Being created
    Listening,      // Waiting for agent to start processing
    Running,        // Agent is running
    Completing,     // Producing final result
    Announcing,     // Sending result to requester
    Completed,      // Successfully completed
    Failed,         // Failed with error
    TimedOut,       // Exceeded timeout
    Killed          // Killed by user/parent
};

/// Subagent registry entry
struct SubagentEntry {
    // Identity
    std::string subagent_run_id;            // Child agent run ID
    std::string requester_run_id;           // Parent agent run ID
    std::string requester_agent_id;         // Parent agent ID
    std::string subagent_name;              // Subagent type name
    
    // Depth tracking
    SubagentDepthInfo depth_info;
    
    // State
    SubagentState state = SubagentState::Spawning;
    std::string description;                // Task description
    std::string prompt;                     // Full prompt
    
    // Timing
    uint64_t spawned_at = 0;                // Creation timestamp (ms)
    uint64_t started_at = 0;                // Agent start timestamp (ms)
    uint64_t completed_at = 0;              // Completion timestamp (ms)
    int timeout_seconds = 900;              // Timeout (default 15 min)
    
    // Result
    std::string result_text;                // Final result text
    bool has_error = false;                 // Error flag
    std::string error_message;              // Error details
    
    // Delivery tracking
    bool result_delivered = false;          // Result delivered to requester
    int delivery_attempts = 0;              // Delivery retry count
    uint64_t last_delivery_attempt = 0;     // Last delivery timestamp
    
    // Notifications
    std::vector<std::string> pending_notifications;  // Queued events
    
    // Check if timed out
    bool is_timed_out() const {
        if (state == SubagentState::Completed || 
            state == SubagentState::Failed || 
            state == SubagentState::Killed) {
            return false;
        }
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        return (now - spawned_at) > (timeout_seconds * 1000);
    }
    
    // Get age in seconds
    double age_seconds() const {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        return (now - spawned_at) / 1000.0;
    }
};

// ============================================================================
// Subagent Registry
// ============================================================================

class SubagentRegistry {
public:
    using Ptr = std::shared_ptr<SubagentRegistry>;
    
    static Ptr create();
    
    SubagentRegistry() = default;
    ~SubagentRegistry() = default;
    
    /// Register a new subagent
    core::Result<void> register_subagent(const SubagentEntry& entry);
    
    /// Get subagent entry
    std::shared_ptr<SubagentEntry> get_entry(const std::string& subagent_run_id);
    
    /// Update subagent state
    core::Result<void> update_state(
        const std::string& subagent_run_id,
        SubagentState new_state
    );
    
    /// Set subagent result
    core::Result<void> set_result(
        const std::string& subagent_run_id,
        const std::string& result_text,
        bool has_error = false,
        const std::string& error_message = ""
    );
    
    /// Mark result as delivered
    core::Result<void> mark_delivered(const std::string& subagent_run_id);
    
    /// Record delivery attempt
    core::Result<void> record_delivery_attempt(const std::string& subagent_run_id);
    
    /// List subagents by requester
    std::vector<std::shared_ptr<SubagentEntry>> list_by_requester(
        const std::string& requester_run_id
    );
    
    /// List all active subagents
    std::vector<std::shared_ptr<SubagentEntry>> list_active();
    
    /// Get subagent depth info
    SubagentDepthInfo get_depth_info(const std::string& run_id);
    
    /// Cleanup completed/timed out entries
    void cleanup_old_entries(int max_age_seconds = 3600);
    
    /// Kill subagent and all its descendants
    core::Result<void> kill_subagent_tree(const std::string& subagent_run_id);

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<SubagentEntry>> entries_;
    
    // Helper: recursively find all descendants
    void find_descendants_recursive(
        const std::string& run_id,
        std::vector<std::string>& descendants
    );
};

// ============================================================================
// Subagent Spawner
// ============================================================================

class SubagentSpawner : public std::enable_shared_from_this<SubagentSpawner> {
public:
    using Ptr = std::shared_ptr<SubagentSpawner>;
    
    static Ptr create(
        std::shared_ptr<AgentManager> agent_mgr,
        std::shared_ptr<SubagentRegistry> registry
    );
    
    SubagentSpawner(
        std::shared_ptr<AgentManager> agent_mgr,
        std::shared_ptr<SubagentRegistry> registry
    );
    
    ~SubagentSpawner() = default;
    
    /// Spawn a new subagent
    /// @param requester_run_id Parent run ID
    /// @param requester_agent_id Parent agent ID
    /// @param options Spawn options
    /// @return Result containing subagent run ID or error
    core::Result<std::string> spawn(
        const std::string& requester_run_id,
        const std::string& requester_agent_id,
        const SubagentSpawnOptions& options
    );
    
    /// Wait for subagent completion (blocking)
    /// @param subagent_run_id Subagent run ID
    /// @param timeout_ms Maximum wait time in milliseconds (0 = indefinite)
    /// @return Result containing final result or error
    core::Result<std::string> wait_for_completion(
        const std::string& subagent_run_id,
        int timeout_ms = 0
    );

private:
    std::shared_ptr<AgentManager> agent_manager_;
    std::shared_ptr<SubagentRegistry> registry_;
    
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, bool> completion_flags_;
    
    // Helper: validate spawn request
    core::Result<void> validate_spawn_request(
        const std::string& requester_run_id,
        const SubagentSpawnOptions& options
    );
    
    // Helper: build subagent config
    AgentConfig build_subagent_config(
        const std::string& requester_agent_id,
        const SubagentSpawnOptions& options,
        const SubagentDepthInfo& depth_info
    );
    
    // Helper: setup event handlers
    void setup_event_handlers(
        AgentPtr subagent,
        const std::string& subagent_run_id
    );
};

// ============================================================================
// Subagent Announcer (result delivery)
// ============================================================================

class SubagentAnnouncer {
public:
    using Ptr = std::shared_ptr<SubagentAnnouncer>;
    
    static Ptr create(
        std::shared_ptr<SubagentRegistry> registry,
        std::shared_ptr<AgentManager> agent_mgr
    );
    
    SubagentAnnouncer(
        std::shared_ptr<SubagentRegistry> registry,
        std::shared_ptr<AgentManager> agent_mgr
    );
    
    ~SubagentAnnouncer();
    
    /// Start background delivery worker
    void start();
    
    /// Stop background delivery worker
    void stop();
    
    /// Immediately attempt to deliver result
    core::Result<void> deliver_now(const std::string& subagent_run_id);

private:
    std::shared_ptr<SubagentRegistry> registry_;
    std::shared_ptr<AgentManager> agent_manager_;
    
    // Background worker
    std::unique_ptr<std::thread> worker_thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    
    void worker_loop();
    
    // Delivery logic
    core::Result<void> attempt_delivery(std::shared_ptr<SubagentEntry> entry);
    
    // Build delivery message
    std::string build_delivery_message(const SubagentEntry& entry);
    
    // Retry logic
    bool should_retry(const SubagentEntry& entry);
    int get_retry_delay_ms(int attempt);
};

// ============================================================================
// Subagent Manager (high-level facade)
// ============================================================================

class SubagentManager {
public:
    using Ptr = std::shared_ptr<SubagentManager>;
    
    static Ptr create(std::shared_ptr<AgentManager> agent_mgr);
    
    SubagentManager(std::shared_ptr<AgentManager> agent_mgr);
    ~SubagentManager();
    
    /// Spawn a subagent
    core::Result<std::string> spawn(
        const std::string& requester_run_id,
        const std::string& requester_agent_id,
        const SubagentSpawnOptions& options
    );
    
    /// Get subagent status
    core::Result<SubagentEntry> get_status(const std::string& subagent_run_id);
    
    /// Kill subagent and all descendants
    core::Result<void> kill(const std::string& subagent_run_id);
    
    /// List subagents for a run
    std::vector<SubagentEntry> list(const std::string& requester_run_id);
    
    /// Check if can spawn more subagents
    bool can_spawn(
        const std::string& requester_run_id,
        const SubagentConfig& config
    );
    
    /// Get registry
    std::shared_ptr<SubagentRegistry> get_registry() { return registry_; }
    
    /// Get spawner
    std::shared_ptr<SubagentSpawner> get_spawner() { return spawner_; }
    
    /// Get announcer
    std::shared_ptr<SubagentAnnouncer> get_announcer() { return announcer_; }

private:
    std::shared_ptr<AgentManager> agent_manager_;
    std::shared_ptr<SubagentRegistry> registry_;
    std::shared_ptr<SubagentSpawner> spawner_;
    std::shared_ptr<SubagentAnnouncer> announcer_;
};

} // namespace agents
} // namespace hunterclaw
