#include "hunterclaw/tools/nodes_tool.h"
#include <json/json.h>
#include <algorithm>
#include <chrono>
#include <sstream>

namespace hunterclaw {
namespace tools {

namespace {

// Node 状态信息
struct NodeInfo {
    std::string node_id;
    std::string display_name;
    std::string platform;
    std::string version;
    std::string core_version;
    std::string ui_version;
    std::string device_family;
    std::string model_identifier;
    std::string remote_ip;
    std::vector<std::string> caps;
    std::vector<std::string> commands;
    std::map<std::string, bool> permissions;
    int64_t connected_at_ms;
    bool paired;
    bool connected;
};

// Node Registry - 管理已连接的节点
class NodeRegistry {
public:
    static NodeRegistry& Instance() {
        static NodeRegistry instance;
        return instance;
    }

    void RegisterNode(const NodeInfo& node) {
        std::lock_guard<std::mutex> lock(mutex_);
        nodes_[node.node_id] = node;
        nodes_by_display_name_[node.display_name] = node.node_id;
    }

    void UnregisterNode(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodes_.find(node_id);
        if (it != nodes_.end()) {
            nodes_by_display_name_.erase(it->second.display_name);
            nodes_.erase(it);
        }
    }

    std::optional<NodeInfo> GetNode(const std::string& node_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodes_.find(node_id);
        if (it != nodes_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<NodeInfo> ListNodes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<NodeInfo> result;
        for (const auto& [_, node] : nodes_) {
            result.push_back(node);
        }
        // 按连接状态和名称排序
        std::sort(result.begin(), result.end(), [](const NodeInfo& a, const NodeInfo& b) {
            if (a.connected != b.connected) {
                return a.connected > b.connected; // 已连接的排前面
            }
            return a.display_name < b.display_name;
        });
        return result;
    }

    bool IsConnected(const std::string& node_id) const {
        auto node = GetNode(node_id);
        return node.has_value() && node->connected;
    }

private:
    NodeRegistry() = default;
    mutable std::mutex mutex_;
    std::map<std::string, NodeInfo> nodes_;
    std::map<std::string, std::string> nodes_by_display_name_;
};

// Pending Node Actions - 等待节点执行的操作队列
struct PendingAction {
    std::string id;
    std::string node_id;
    std::string command;
    std::string params_json;
    std::string idempotency_key;
    int64_t enqueued_at_ms;
};

class PendingActionQueue {
public:
    static PendingActionQueue& Instance() {
        static PendingActionQueue instance;
        return instance;
    }

    std::string Enqueue(const std::string& node_id, const std::string& command,
                        const std::string& params_json, const std::string& idempotency_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查是否已存在相同 idempotency_key 的操作
        auto& queue = queues_[node_id];
        for (const auto& action : queue) {
            if (action.idempotency_key == idempotency_key) {
                return action.id;
            }
        }

        // 创建新操作
        PendingAction action;
        action.id = GenerateUUID();
        action.node_id = node_id;
        action.command = command;
        action.params_json = params_json;
        action.idempotency_key = idempotency_key;
        action.enqueued_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        queue.push_back(action);
        
        // 限制队列大小
        const size_t kMaxQueueSize = 64;
        if (queue.size() > kMaxQueueSize) {
            queue.erase(queue.begin(), queue.begin() + (queue.size() - kMaxQueueSize));
        }
        
        return action.id;
    }

    std::vector<PendingAction> List(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        PruneExpired(node_id);
        return queues_[node_id];
    }

    int Acknowledge(const std::string& node_id, const std::vector<std::string>& action_ids) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& queue = queues_[node_id];
        
        std::set<std::string> id_set(action_ids.begin(), action_ids.end());
        queue.erase(
            std::remove_if(queue.begin(), queue.end(),
                          [&id_set](const PendingAction& action) {
                              return id_set.count(action.id) > 0;
                          }),
            queue.end());
        
        return queue.size();
    }

private:
    PendingActionQueue() = default;

    void PruneExpired(const std::string& node_id) {
        auto& queue = queues_[node_id];
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const int64_t kTTL = 10 * 60 * 1000; // 10 分钟
        
        queue.erase(
            std::remove_if(queue.begin(), queue.end(),
                          [now_ms, kTTL](const PendingAction& action) {
                              return (now_ms - action.enqueued_at_ms) > kTTL;
                          }),
            queue.end());
    }

    std::string GenerateUUID() {
        // 简化的 UUID 生成
        std::stringstream ss;
        ss << std::hex << std::chrono::system_clock::now().time_since_epoch().count();
        return ss.str();
    }

    mutable std::mutex mutex_;
    std::map<std::string, std::vector<PendingAction>> queues_;
};

// 将 NodeInfo 转换为 JSON
Json::Value NodeInfoToJson(const NodeInfo& node) {
    Json::Value json;
    json["nodeId"] = node.node_id;
    json["displayName"] = node.display_name;
    json["platform"] = node.platform;
    json["version"] = node.version;
    json["coreVersion"] = node.core_version;
    json["uiVersion"] = node.ui_version;
    json["deviceFamily"] = node.device_family;
    json["modelIdentifier"] = node.model_identifier;
    json["remoteIp"] = node.remote_ip;
    json["connectedAtMs"] = Json::Int64(node.connected_at_ms);
    json["paired"] = node.paired;
    json["connected"] = node.connected;
    
    Json::Value caps_json(Json::arrayValue);
    for (const auto& cap : node.caps) {
        caps_json.append(cap);
    }
    json["caps"] = caps_json;
    
    Json::Value commands_json(Json::arrayValue);
    for (const auto& cmd : node.commands) {
        commands_json.append(cmd);
    }
    json["commands"] = commands_json;
    
    Json::Value permissions_json;
    for (const auto& [key, value] : node.permissions) {
        permissions_json[key] = value;
    }
    json["permissions"] = permissions_json;
    
    return json;
}

} // namespace

NodesTool::NodesTool(const std::string& name, const std::string& description)
    : BaseTool(name, description) {}

ToolDefinition NodesTool::GetDefinition() const {
    ToolDefinition def;
    def.name = name_;
    def.description = description_;
    
    // nodes_list - 列出所有节点
    {
        ToolFunction func;
        func.name = "nodes_list";
        func.description = "列出所有已配对的节点(设备),包括连接状态、能力、命令等信息";
        
        Json::Value schema;
        schema["type"] = "object";
        schema["properties"] = Json::Value(Json::objectValue);
        schema["required"] = Json::Value(Json::arrayValue);
        
        func.parameters = schema;
        def.functions.push_back(func);
    }
    
    // nodes_invoke - 调用节点命令
    {
        ToolFunction func;
        func.name = "nodes_invoke";
        func.description = "在指定节点上执行命令,支持跨设备操作(如拍照、录屏、执行终端命令等)";
        
        Json::Value schema;
        schema["type"] = "object";
        
        Json::Value properties;
        properties["nodeId"]["type"] = "string";
        properties["nodeId"]["description"] = "节点 ID (必需)";
        properties["command"]["type"] = "string";
        properties["command"]["description"] = "要执行的命令 (必需)";
        properties["params"]["type"] = "object";
        properties["params"]["description"] = "命令参数 (可选)";
        properties["timeoutMs"]["type"] = "integer";
        properties["timeoutMs"]["description"] = "超时时间(毫秒),默认 30000";
        schema["properties"] = properties;
        
        Json::Value required;
        required.append("nodeId");
        required.append("command");
        schema["required"] = required;
        
        func.parameters = schema;
        def.functions.push_back(func);
    }
    
    // nodes_status - 查看节点详细状态
    {
        ToolFunction func;
        func.name = "nodes_status";
        func.description = "查看指定节点的详细状态信息,包括能力、支持的命令、权限配置等";
        
        Json::Value schema;
        schema["type"] = "object";
        
        Json::Value properties;
        properties["nodeId"]["type"] = "string";
        properties["nodeId"]["description"] = "节点 ID (必需)";
        schema["properties"] = properties;
        
        Json::Value required;
        required.append("nodeId");
        schema["required"] = required;
        
        func.parameters = schema;
        def.functions.push_back(func);
    }
    
    return def;
}

ToolResult NodesTool::Execute(const std::string& function_name,
                               const Json::Value& arguments) {
    if (function_name == "nodes_list") {
        return ExecuteNodesList(arguments);
    } else if (function_name == "nodes_invoke") {
        return ExecuteNodesInvoke(arguments);
    } else if (function_name == "nodes_status") {
        return ExecuteNodesStatus(arguments);
    }
    
    return ToolResult::Error("Unknown function: " + function_name);
}

ToolResult NodesTool::ExecuteNodesList(const Json::Value& arguments) {
    auto& registry = NodeRegistry::Instance();
    auto nodes = registry.ListNodes();
    
    Json::Value result;
    result["ts"] = Json::Int64(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    result["totalNodes"] = static_cast<int>(nodes.size());
    
    int connected_count = 0;
    Json::Value nodes_json(Json::arrayValue);
    for (const auto& node : nodes) {
        if (node.connected) {
            connected_count++;
        }
        nodes_json.append(NodeInfoToJson(node));
    }
    
    result["connectedNodes"] = connected_count;
    result["nodes"] = nodes_json;
    
    // 添加人类可读的摘要
    std::stringstream summary;
    summary << "Found " << nodes.size() << " paired node(s), "
            << connected_count << " currently connected:\n\n";
    
    for (const auto& node : nodes) {
        summary << "• " << node.display_name << " (" << node.node_id.substr(0, 8) << "...)\n";
        summary << "  Platform: " << node.platform << " " << node.version << "\n";
        summary << "  Status: " << (node.connected ? "✓ Connected" : "✗ Disconnected") << "\n";
        summary << "  Capabilities: " << node.caps.size() << " cap(s), "
                << node.commands.size() << " command(s)\n\n";
    }
    
    return ToolResult::Success(result, summary.str());
}

ToolResult NodesTool::ExecuteNodesInvoke(const Json::Value& arguments) {
    // 验证必需参数
    if (!arguments.isMember("nodeId") || !arguments["nodeId"].isString()) {
        return ToolResult::Error("Missing required parameter: nodeId");
    }
    if (!arguments.isMember("command") || !arguments["command"].isString()) {
        return ToolResult::Error("Missing required parameter: command");
    }
    
    std::string node_id = arguments["nodeId"].asString();
    std::string command = arguments["command"].asString();
    
    // 检查节点是否存在且已连接
    auto& registry = NodeRegistry::Instance();
    if (!registry.IsConnected(node_id)) {
        Json::Value error;
        error["code"] = "NOT_CONNECTED";
        error["message"] = "Node not connected";
        return ToolResult::Error("Node not connected: " + node_id, error);
    }
    
    // 获取节点信息,验证命令是否支持
    auto node_opt = registry.GetNode(node_id);
    if (!node_opt.has_value()) {
        return ToolResult::Error("Node not found: " + node_id);
    }
    
    const auto& node = *node_opt;
    bool command_supported = std::find(node.commands.begin(), node.commands.end(), command)
                             != node.commands.end();
    if (!command_supported) {
        Json::Value error;
        error["code"] = "COMMAND_NOT_SUPPORTED";
        error["message"] = "Command not supported by this node";
        error["availableCommands"] = Json::Value(Json::arrayValue);
        for (const auto& cmd : node.commands) {
            error["availableCommands"].append(cmd);
        }
        return ToolResult::Error("Command not supported: " + command, error);
    }
    
    // 构建调用参数
    Json::Value params = arguments.get("params", Json::Value(Json::objectValue));
    int timeout_ms = arguments.get("timeoutMs", 30000).asInt();
    
    // 生成 idempotency key
    std::string idempotency_key = command + "_" + 
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    
    // TODO: 实际实现需要通过 Gateway 发送命令到节点
    // 这里模拟一个成功的响应
    Json::Value result;
    result["ok"] = true;
    result["nodeId"] = node_id;
    result["command"] = command;
    result["timestamp"] = Json::Int64(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    std::stringstream summary;
    summary << "Successfully invoked command '" << command << "' on node '"
            << node.display_name << "' (" << node_id.substr(0, 8) << "...)";
    
    return ToolResult::Success(result, summary.str());
}

ToolResult NodesTool::ExecuteNodesStatus(const Json::Value& arguments) {
    // 验证必需参数
    if (!arguments.isMember("nodeId") || !arguments["nodeId"].isString()) {
        return ToolResult::Error("Missing required parameter: nodeId");
    }
    
    std::string node_id = arguments["nodeId"].asString();
    
    // 获取节点信息
    auto& registry = NodeRegistry::Instance();
    auto node_opt = registry.GetNode(node_id);
    
    if (!node_opt.has_value()) {
        return ToolResult::Error("Node not found: " + node_id);
    }
    
    const auto& node = *node_opt;
    Json::Value result = NodeInfoToJson(node);
    result["ts"] = Json::Int64(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    // 添加 Pending Actions 信息
    auto& queue = PendingActionQueue::Instance();
    auto pending_actions = queue.List(node_id);
    result["pendingActionsCount"] = static_cast<int>(pending_actions.size());
    
    // 构建人类可读的摘要
    std::stringstream summary;
    summary << "Node Status: " << node.display_name << " (" << node_id.substr(0, 8) << "...)\n\n";
    summary << "Platform: " << node.platform << " " << node.version << "\n";
    summary << "Status: " << (node.connected ? "✓ Connected" : "✗ Disconnected") << "\n";
    summary << "Device: " << node.device_family << " / " << node.model_identifier << "\n\n";
    
    summary << "Capabilities (" << node.caps.size() << "):\n";
    for (size_t i = 0; i < std::min(node.caps.size(), size_t(5)); ++i) {
        summary << "  • " << node.caps[i] << "\n";
    }
    if (node.caps.size() > 5) {
        summary << "  ... and " << (node.caps.size() - 5) << " more\n";
    }
    
    summary << "\nSupported Commands (" << node.commands.size() << "):\n";
    for (size_t i = 0; i < std::min(node.commands.size(), size_t(10)); ++i) {
        summary << "  • " << node.commands[i] << "\n";
    }
    if (node.commands.size() > 10) {
        summary << "  ... and " << (node.commands.size() - 10) << " more\n";
    }
    
    if (!pending_actions.empty()) {
        summary << "\nPending Actions: " << pending_actions.size() << "\n";
    }
    
    return ToolResult::Success(result, summary.str());
}

} // namespace tools
} // namespace hunterclaw
