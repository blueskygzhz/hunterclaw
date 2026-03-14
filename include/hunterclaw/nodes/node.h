#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace hunterclaw {
namespace nodes {

/**
 * @brief Node connection status
 */
enum class NodeStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

/**
 * @brief Node data structure (aligned with OpenClaw NodeSession)
 * 
 * This structure represents a connected node that can execute commands.
 * It aligns with OpenClaw's NodeSession interface.
 */
struct Node {
    // Core identity
    std::string node_id;                    // Unique node identifier
    std::string conn_id;                    // WebSocket connection ID
    std::string display_name;               // Human-readable name
    
    // Platform and capabilities
    std::string platform;                   // "ios", "android", "macos", etc.
    std::vector<std::string> caps;          // Capability list (e.g., "canvas", "camera")
    std::vector<std::string> commands;      // Supported commands
    
    // Permissions
    std::map<std::string, bool> permissions; // Command -> allowed mapping
    
    // Connection state
    NodeStatus status = NodeStatus::DISCONNECTED;
    int64_t connected_at_ms = 0;
    int64_t last_ping_ms = 0;
    
    // Metadata
    std::map<std::string, std::string> metadata;
    
    /**
     * @brief Check if node supports a command
     */
    bool supports_command(const std::string& command) const;
    
    /**
     * @brief Check if command is permitted
     */
    bool has_permission(const std::string& command) const;
    
    /**
     * @brief Check if node is connected
     */
    bool is_connected() const {
        return status == NodeStatus::CONNECTED;
    }
};

/**
 * @brief Node invocation result (aligned with OpenClaw NodeInvokeResult)
 */
struct NodeInvokeResult {
    bool success = false;
    std::string node_id;
    std::string command;
    std::string result_json;        // JSON serialized result
    std::string error_message;
    int64_t duration_ms = 0;
    
    // Metadata
    std::map<std::string, std::string> metadata;
};

} // namespace nodes
} // namespace hunterclaw
