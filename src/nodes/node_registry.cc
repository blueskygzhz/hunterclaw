#include "hunterclaw/nodes/node_registry.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/sandbox/sandbox.h"  // ✅ 修复: SandboxManager 在 sandbox.h 中
#include <chrono>
#include <sstream>

namespace hunterclaw {
namespace nodes {

NodeRegistry::NodeRegistry(std::shared_ptr<tools::ToolManager> tool_manager,
                           std::shared_ptr<sandbox::SandboxManager> sandbox_manager)
    : tool_manager_(tool_manager)
    , sandbox_manager_(sandbox_manager) {
}

core::Result<void> NodeRegistry::register_node(const Node& node) {
    auto validation = validate_node(node);
    if (!validation.is_ok()) {
        return validation;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if already registered
    if (nodes_.count(node.node_id) > 0) {
        return core::Result<void>::Err("Node already registered: " + node.node_id);
    }
    
    // Register node
    nodes_[node.node_id] = node;
    
    // Emit event
    if (event_callback_) {
        event_callback_(node.node_id, "node.registered", "{}");
    }
    
    return core::Result<void>::Ok();
}

core::Result<void> NodeRegistry::unregister_node(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return core::Result<void>::Err("Node not found: " + node_id);
    }
    
    nodes_.erase(it);
    
    // Emit event
    if (event_callback_) {
        event_callback_(node_id, "node.unregistered", "{}");
    }
    
    return core::Result<void>::Ok();
}

void NodeRegistry::update_node_status(const std::string& node_id, NodeStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        it->second.status = status;
        if (status == NodeStatus::CONNECTED) {
            it->second.connected_at_ms = current_time_ms();
        }
    }
}

void NodeRegistry::update_heartbeat(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        it->second.last_ping_ms = current_time_ms();
    }
}

std::vector<Node> NodeRegistry::list_nodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Node> result;
    result.reserve(nodes_.size());
    
    for (const auto& [id, node] : nodes_) {
        result.push_back(node);
    }
    
    return result;
}

std::optional<Node> NodeRegistry::get_node(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return std::nullopt;
    }
    
    return it->second;
}

std::vector<Node> NodeRegistry::find_nodes_by_platform(const std::string& platform) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Node> result;
    
    for (const auto& [id, node] : nodes_) {
        if (node.platform == platform) {
            result.push_back(node);
        }
    }
    
    return result;
}

std::vector<Node> NodeRegistry::find_nodes_by_capability(const std::string& capability) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Node> result;
    
    for (const auto& [id, node] : nodes_) {
        auto& caps = node.caps;
        if (std::find(caps.begin(), caps.end(), capability) != caps.end()) {
            result.push_back(node);
        }
    }
    
    return result;
}

core::Result<NodeInvokeResult> NodeRegistry::invoke(
    const std::string& node_id,
    const std::string& command,
    const std::string& params_json,
    int timeout_ms,
    const std::string& idempotency_key
) {
    ++total_invocations_;
    
    auto start_time = current_time_ms();
    
    // Get node
    Node node;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodes_.find(node_id);
        if (it == nodes_.end()) {
            ++failed_invocations_;
            return core::Result<NodeInvokeResult>::Err("Node not found: " + node_id);
        }
        node = it->second;
    }
    
    // Check connection status
    if (!node.is_connected()) {
        ++failed_invocations_;
        return core::Result<NodeInvokeResult>::Err("Node not connected: " + node_id);
    }
    
    // Check command support
    if (!node.supports_command(command)) {
        ++failed_invocations_;
        return core::Result<NodeInvokeResult>::Err("Command not supported by node: " + command);
    }
    
    // Check permission
    if (!check_permission(node, command)) {
        ++failed_invocations_;
        return core::Result<NodeInvokeResult>::Err("Permission denied for command: " + command);
    }
    
    // Route to ToolManager with node context
    // Parse params_json to JsonValue
    auto params_result = core::JsonValue::parse(params_json);
    if (!params_result.is_ok()) {
        ++failed_invocations_;
        return core::Result<NodeInvokeResult>::Err("Invalid JSON parameters");
    }
    core::JsonValue params = params_result.get();
    
    // Execute tool (metadata in ToolContext will be available to the tool)
    auto tool_result = tool_manager_->execute_tool(command, params);
    
    // Build result
    NodeInvokeResult result;
    result.node_id = node_id;
    result.command = command;
    result.duration_ms = current_time_ms() - start_time;
    
    // ToolResult 已经包含 success 和 error 字段
    result.success = tool_result.success;
    if (!tool_result.content.empty()) {
        // 使用第一个content item的text
        result.result_json = tool_result.content[0].text;
    }
    result.error_message = tool_result.error;
    
    if (!result.success) {
        ++failed_invocations_;
    }
    
    return core::Result<NodeInvokeResult>::Ok(result);
}

core::Result<std::string> NodeRegistry::describe(const std::string& node_id) const {
    auto node_opt = get_node(node_id);
    if (!node_opt) {
        return core::Result<std::string>::Err("Node not found: " + node_id);
    }
    
    auto& node = *node_opt;
    
    // Build JSON description
    std::ostringstream oss;
    oss << "{";
    oss << "\"nodeId\":\"" << node.node_id << "\",";
    oss << "\"displayName\":\"" << node.display_name << "\",";
    oss << "\"platform\":\"" << node.platform << "\",";
    oss << "\"status\":\"" << (node.is_connected() ? "connected" : "disconnected") << "\",";
    oss << "\"connectedAtMs\":" << node.connected_at_ms << ",";
    oss << "\"caps\":[";
    for (size_t i = 0; i < node.caps.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << node.caps[i] << "\"";
    }
    oss << "],";
    oss << "\"commands\":[";
    for (size_t i = 0; i < node.commands.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << node.commands[i] << "\"";
    }
    oss << "]";
    oss << "}";
    
    return core::Result<std::string>::Ok(oss.str());
}

void NodeRegistry::set_event_callback(NodeEventCallback callback) {
    event_callback_ = callback;
}

void NodeRegistry::emit_event(const std::string& node_id,
                              const std::string& event_type,
                              const std::string& event_data) {
    if (event_callback_) {
        event_callback_(node_id, event_type, event_data);
    }
}

int NodeRegistry::cleanup_stale_nodes(int64_t threshold_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    int removed = 0;
    auto current = current_time_ms();
    
    auto it = nodes_.begin();
    while (it != nodes_.end()) {
        auto& node = it->second;
        if (node.status == NodeStatus::DISCONNECTED &&
            (current - node.last_ping_ms) > threshold_ms) {
            it = nodes_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    
    return removed;
}

std::string NodeRegistry::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    int connected = 0;
    for (const auto& [id, node] : nodes_) {
        if (node.is_connected()) {
            ++connected;
        }
    }
    
    std::ostringstream oss;
    oss << "{";
    oss << "\"totalNodes\":" << nodes_.size() << ",";
    oss << "\"connectedNodes\":" << connected << ",";
    oss << "\"totalInvocations\":" << total_invocations_.load() << ",";
    oss << "\"failedInvocations\":" << failed_invocations_.load();
    oss << "}";
    
    return oss.str();
}

bool NodeRegistry::check_permission(const Node& node, const std::string& command) const {
    return node.has_permission(command);
}

core::Result<void> NodeRegistry::validate_node(const Node& node) const {
    if (node.node_id.empty()) {
        return core::Result<void>::Err("Node ID cannot be empty");
    }
    if (node.platform.empty()) {
        return core::Result<void>::Err("Platform cannot be empty");
    }
    if (node.commands.empty()) {
        return core::Result<void>::Err("Node must support at least one command");
    }
    return core::Result<void>::Ok();
}

int64_t NodeRegistry::current_time_ms() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

} // namespace nodes
} // namespace hunterclaw
