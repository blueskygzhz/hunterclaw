#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/json.h"

namespace hunterclaw {
namespace tools {

/**
 * @brief Session information structure
 * 
 * Represents a single session with its metadata and state.
 */
struct SessionInfo {
    std::string key;                          // Session key (internal or display)
    std::string kind;                         // main, group, cron, hook, node, other
    std::string channel;                      // discord, slack, telegram, http, etc.
    std::optional<std::string> label;         // User-defined label
    std::optional<std::string> agent_id;      // Agent ID (for cross-agent)
    std::optional<std::string> model;         // Current model
    int64_t created_at;                       // Creation timestamp (unix ms)
    int64_t last_active_at;                   // Last activity timestamp (unix ms)
    bool is_active;                           // Currently active?
    std::vector<std::string> spawned_by;      // Parent session keys
    std::optional<std::string> account_id;    // Account ID
    std::optional<std::string> thread_id;     // Thread ID
    int message_count;                        // Total messages in session
};

/**
 * @brief Session message structure
 * 
 * Represents a single message in session history.
 */
struct SessionMessage {
    std::string role;                         // user, assistant, system, tool
    std::string content;                      // Message text content
    int64_t timestamp;                        // Message timestamp (unix ms)
    std::optional<std::string> tool_name;     // Tool name (if tool message)
    std::optional<std::string> tool_call_id;  // Tool call ID
    bool truncated;                           // Content truncated?
    bool redacted;                            // Sensitive data redacted?
};

/**
 * @brief Session spawn result
 * 
 * Result of spawning a new session.
 */
struct SessionSpawnResult {
    std::string status;                       // success, error, forbidden
    std::string session_key;                  // Spawned session key
    std::optional<std::string> session_id;    // Session ID (for ACP runtime)
    std::optional<std::string> error;         // Error message if failed
    std::optional<std::string> run_id;        // Run ID (for one-shot mode)
};

/**
 * @brief Session send result
 * 
 * Result of sending a message to another session.
 */
struct SessionSendResult {
    std::string status;                       // success, error, forbidden, timeout
    std::string run_id;                       // Message run ID
    std::optional<std::string> error;         // Error message if failed
    std::optional<std::string> response;      // Response from target (if sync)
};

/**
 * @brief Session status information
 * 
 * Detailed status of a specific session.
 */
struct SessionStatus {
    std::string session_key;                  // Session key
    std::string kind;                         // Session kind
    std::string model;                        // Current model
    std::string status;                       // active, idle, completed, error
    int64_t created_at;                       // Creation timestamp
    int64_t last_active_at;                   // Last activity timestamp
    int message_count;                        // Total messages
    int pending_messages;                     // Pending messages in queue
    std::optional<std::string> error;         // Error message if error state
    std::optional<std::string> label;         // Session label
    std::optional<std::string> channel;       // Delivery channel
};

/**
 * @brief SessionsTool - Multi-modal Session Management Tool
 * 
 * Provides comprehensive session management capabilities:
 * 1. sessions_list - List all sessions with filters
 * 2. sessions_send - Send messages to other sessions
 * 3. sessions_spawn - Create new isolated sessions
 * 4. sessions_history - View session conversation history
 * 5. session_status - Get detailed session status
 * 
 * Design Philosophy:
 * - Cross-session communication (inter-agent messaging)
 * - Session lifecycle management (spawn, monitor, cleanup)
 * - Security & sandboxing (visibility controls, A2A policy)
 * - Performance (caching, pagination, efficient queries)
 * 
 * OpenClaw Alignment:
 * - Full compatibility with OpenClaw sessions API
 * - Cross-agent messaging with agent-to-agent policy
 * - Session visibility controls (tree, self, agent, all)
 * - Label-based session resolution
 * 
 * Security Features:
 * - Sandboxed session visibility (restrict to spawned)
 * - Agent-to-agent policy enforcement
 * - Sensitive data redaction in history
 * - Cross-agent authorization checks
 * 
 * Performance Features:
 * - Efficient session listing with filters
 * - Paginated history queries
 * - Cached session metadata
 * - Lazy message loading
 * 
 * Usage Example:
 * ```cpp
 * // List all active sessions
 * json params = {
 *     {"kinds", json::array({"main", "group"})},
 *     {"active_minutes", 60},
 *     {"limit", 20}
 * };
 * auto result = sessions_tool.execute("sessions_list", params);
 * 
 * // Send message to another session
 * json send_params = {
 *     {"label", "assistant-1"},
 *     {"message", "Please analyze the logs"},
 *     {"timeout_seconds", 30}
 * };
 * auto send_result = sessions_tool.execute("sessions_send", send_params);
 * 
 * // Spawn new subagent session
 * json spawn_params = {
 *     {"task", "Analyze security logs"},
 *     {"label", "security-analyzer"},
 *     {"runtime", "subagent"},
 *     {"mode", "session"}
 * };
 * auto spawn_result = sessions_tool.execute("sessions_spawn", spawn_params);
 * ```
 * 
 * Supported Actions:
 * - sessions_list: List sessions (filters: kinds, limit, activeMinutes)
 * - sessions_send: Send to session (sessionKey or label, message, timeout)
 * - sessions_spawn: Spawn session (task, label, runtime, mode, model)
 * - sessions_history: View history (sessionKey, limit, includeTools)
 * - session_status: Get status (sessionKey, model override)
 * 
 * Configuration:
 * - Session storage path (default: ~/.hunterclaw/sessions/)
 * - Visibility mode (tree, self, agent, all)
 * - Agent-to-agent policy (enabled, allow list)
 * - History retention (days, max messages)
 * - Sandbox mode (inherit, require)
 * 
 * @note Requires active HunterClaw gateway for session management
 * @note Cross-agent messaging requires agent-to-agent policy enabled
 */
class SessionsTool : public Tool {
public:
    /**
     * @brief Configuration for SessionsTool
     */
    struct Config {
        std::string session_storage_path;     // Session storage directory
        std::string visibility_mode;          // tree, self, agent, all
        bool agent_to_agent_enabled;          // Enable cross-agent messaging
        std::vector<std::string> a2a_allow;   // Allowed agent pairs (a:b)
        bool sandboxed;                       // Sandboxed mode (restrict visibility)
        std::string requester_session_key;    // Current session key
        std::string requester_agent_id;       // Current agent ID
        int history_max_messages;             // Max messages in history query
        int history_retention_days;           // History retention period
        std::string gateway_endpoint;         // Gateway API endpoint
    };

    /**
     * @brief Construct SessionsTool with configuration
     * 
     * @param config Tool configuration
     */
    explicit SessionsTool(const Config& config);

    /**
     * @brief Get tool metadata
     */
    ToolMetadata get_metadata() const override;

    /**
     * @brief Execute tool action
     * 
     * @param action Action name (sessions_list, sessions_send, etc.)
     * @param params Action parameters
     * @return Execution result
     */
    json execute(const std::string& action, const json& params) override;

private:
    // Sessions List
    json sessions_list(const json& params);
    
    // Sessions Send
    json sessions_send(const json& params);
    
    // Sessions Spawn
    json sessions_spawn(const json& params);
    
    // Sessions History
    json sessions_history(const json& params);
    
    // Session Status
    json session_status(const json& params);
    
    // Helper methods
    std::vector<SessionInfo> load_sessions_from_storage();
    std::vector<SessionInfo> filter_sessions(
        const std::vector<SessionInfo>& sessions,
        const std::optional<std::vector<std::string>>& kinds,
        const std::optional<int>& active_minutes,
        const std::optional<int>& limit
    );
    bool check_session_visibility(const SessionInfo& session);
    bool check_agent_to_agent_allowed(
        const std::string& requester_agent,
        const std::string& target_agent
    );
    std::string resolve_session_key_from_label(
        const std::string& label,
        const std::optional<std::string>& agent_id
    );
    std::vector<SessionMessage> load_session_history(
        const std::string& session_key,
        int limit,
        bool include_tools
    );
    SessionStatus get_session_status_internal(const std::string& session_key);
    SessionSpawnResult spawn_session_internal(
        const std::string& task,
        const std::optional<std::string>& label,
        const std::string& runtime,
        const std::string& mode,
        const std::optional<std::string>& model
    );
    SessionSendResult send_message_internal(
        const std::string& target_key,
        const std::string& message,
        const std::optional<int>& timeout_seconds
    );

    // Gateway communication
    json call_gateway(const std::string& method, const json& params);

    // Configuration
    Config config_;
    
    // Session cache
    mutable std::unordered_map<std::string, SessionInfo> session_cache_;
    mutable int64_t cache_timestamp_;
    static constexpr int64_t CACHE_TTL_MS = 5000; // 5 seconds
};

/**
 * @brief Factory function to create SessionsTool
 * 
 * @param config Tool configuration
 * @return Unique pointer to SessionsTool
 */
std::unique_ptr<SessionsTool> create_sessions_tool(const SessionsTool::Config& config);

} // namespace tools
} // namespace hunterclaw
