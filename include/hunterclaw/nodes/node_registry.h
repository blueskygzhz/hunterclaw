#pragma once

#include "hunterclaw/nodes/node.h"
#include "hunterclaw/core/result.h"
#include "hunterclaw/tools/tool_result.h"
#include <memory>
#include <functional>
#include <optional>
#include <mutex>

namespace hunterclaw {

// Forward declarations
namespace tools {
class ToolManager;
}

namespace sandbox {
class SandboxManager;
}

namespace nodes {

/**
 * @brief Node event callback type
 */
using NodeEventCallback = std::function<void(const std::string& node_id, 
                                             const std::string& event_type,
                                             const std::string& event_data)>;

/**
 * @brief NodeRegistry manages registered nodes and routes commands to them
 * 
 * This class aligns with OpenClaw's NodeRegistry interface and provides:
 * - Node registration and lifecycle management
 * - Command invocation with routing to ToolManager
 * - Permission and capability checking
 * - Event handling
 */
class NodeRegistry {
public:
    /**
     * @brief Constructor
     * @param tool_manager Shared ToolManager for command execution
     * @param sandbox_manager Shared SandboxManager for sandbox integration
     */
    NodeRegistry(std::shared_ptr<tools::ToolManager> tool_manager,
                 std::shared_ptr<sandbox::SandboxManager> sandbox_manager);
    
    ~NodeRegistry() = default;
    
    // ============ Node Lifecycle ============
    
    /**
     * @brief Register a new node (aligned with OpenClaw node.register)
     * @param node Node information
     * @return Result with error message if failed
     */
    core::Result<void> register_node(const Node& node);
    
    /**
     * @brief Unregister a node
     * @param node_id Node identifier
     * @return Result with error message if failed
     */
    core::Result<void> unregister_node(const std::string& node_id);
    
    /**
     * @brief Update node status
     * @param node_id Node identifier
     * @param status New status
     */
    void update_node_status(const std::string& node_id, NodeStatus status);
    
    /**
     * @brief Update node heartbeat
     * @param node_id Node identifier
     */
    void update_heartbeat(const std::string& node_id);
    
    // ============ Node Query ============
    
    /**
     * @brief List all registered nodes (aligned with OpenClaw node.list)
     * @return Vector of nodes
     */
    std::vector<Node> list_nodes() const;
    
    /**
     * @brief Get node by ID
     * @param node_id Node identifier
     * @return Optional node, nullopt if not found
     */
    std::optional<Node> get_node(const std::string& node_id) const;
    
    /**
     * @brief Find nodes by platform
     * @param platform Platform name (e.g., "ios", "android")
     * @return Vector of matching nodes
     */
    std::vector<Node> find_nodes_by_platform(const std::string& platform) const;
    
    /**
     * @brief Find nodes by capability
     * @param capability Capability name (e.g., "camera", "canvas")
     * @return Vector of matching nodes
     */
    std::vector<Node> find_nodes_by_capability(const std::string& capability) const;
    
    // ============ Command Invocation ============
    
    /**
     * @brief Invoke a command on a node (aligned with OpenClaw NodeRegistry.invoke)
     * 
     * This method routes the command to the underlying ToolManager and adds
     * node context for execution.
     * 
     * @param node_id Target node identifier
     * @param command Command name (e.g., "browser.snapshot")
     * @param params Command parameters as JSON string
     * @param timeout_ms Timeout in milliseconds (default 30s)
     * @param idempotency_key Optional idempotency key for deduplication
     * @return Result with NodeInvokeResult or error
     */
    core::Result<NodeInvokeResult> invoke(
        const std::string& node_id,
        const std::string& command,
        const std::string& params_json,
        int timeout_ms = 30000,
        const std::string& idempotency_key = ""
    );
    
    /**
     * @brief Describe a node's capabilities (aligned with OpenClaw node.describe)
     * @param node_id Node identifier
     * @return JSON string with node details
     */
    core::Result<std::string> describe(const std::string& node_id) const;
    
    // ============ Event Handling ============
    
    /**
     * @brief Set event callback for node events
     * @param callback Event callback function
     */
    void set_event_callback(NodeEventCallback callback);
    
    /**
     * @brief Emit a node event
     * @param node_id Node identifier
     * @param event_type Event type
     * @param event_data Event data (JSON string)
     */
    void emit_event(const std::string& node_id,
                    const std::string& event_type,
                    const std::string& event_data);
    
    // ============ Maintenance ============
    
    /**
     * @brief Remove disconnected nodes older than threshold
     * @param threshold_ms Age threshold in milliseconds
     * @return Number of nodes removed
     */
    int cleanup_stale_nodes(int64_t threshold_ms = 300000); // 5 minutes
    
    /**
     * @brief Get statistics
     * @return JSON string with registry statistics
     */
    std::string get_statistics() const;
    
private:
    // Internal helpers
    bool check_permission(const Node& node, const std::string& command) const;
    core::Result<void> validate_node(const Node& node) const;
    int64_t current_time_ms() const;
    
    // Dependencies
    std::shared_ptr<tools::ToolManager> tool_manager_;
    std::shared_ptr<sandbox::SandboxManager> sandbox_manager_;
    
    // State
    mutable std::mutex mutex_;
    std::map<std::string, Node> nodes_;
    NodeEventCallback event_callback_;
    
    // Statistics
    std::atomic<uint64_t> total_invocations_{0};
    std::atomic<uint64_t> failed_invocations_{0};
};

} // namespace nodes
} // namespace hunterclaw
