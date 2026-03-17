#pragma once

#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace hunterclaw {
namespace tools {

/**
 * @brief Nodes Tool - 节点管理工具
 * 
 * 对齐 OpenClaw 的 Node 管理功能,支持:
 * - nodes_list: 列出所有节点及其状态
 * - nodes_invoke: 调用节点命令
 * - nodes_status: 查看节点详细状态
 * 
 * 对应 OpenClaw 文件:
 * - src/gateway/server-methods/nodes.ts
 * - src/cli/nodes-cli/register.ts
 */
class NodesTool {
 public:
  NodesTool();
  ~NodesTool();

  // 执行 Nodes 动作
  nlohmann::json execute(const std::string& action,
                         const nlohmann::json& params);

 private:
  // ==================== nodes_list ====================
  /**
   * @brief 列出所有已配对的节点
   * 
   * 参数:
   * - connected_only (bool, optional): 仅显示已连接的节点 (默认 false)
   * - include_capabilities (bool, optional): 包含节点能力信息 (默认 true)
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "nodes": [
   *     {
   *       "id": "node-123",
   *       "label": "My iPhone",
   *       "platform": "iOS 17.0",
   *       "connected": true,
   *       "last_seen_ms": 1234567890,
   *       "capabilities": ["camera", "screen", "notifications"]
   *     }
   *   ],
   *   "count": 1
   * }
   * 
   * OpenClaw 对齐:
   * - src/gateway/server-methods/nodes.ts (node.list)
   * - src/cli/nodes-cli/register.status.ts (nodes list)
   */
  nlohmann::json nodes_list(const nlohmann::json& params);

  // ==================== nodes_invoke ====================
  /**
   * @brief 调用节点命令
   * 
   * 参数:
   * - node_id (string, required): 节点 ID
   * - command (string, required): 命令名称 (如 "camera.snap", "screen.record")
   * - params (object, optional): 命令参数
   * - timeout_seconds (int, optional): 超时时间 (默认 30)
   * - idempotency_key (string, optional): 幂等性键
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "result": { ... },
   *   "node_id": "node-123",
   *   "command": "camera.snap"
   * }
   * 
   * OpenClaw 对齐:
   * - src/gateway/server-methods/nodes.ts (node.invoke)
   * - src/gateway/node-command-policy.ts (isNodeCommandAllowed)
   * - src/cli/nodes-cli/register.invoke.ts (nodes invoke)
   */
  nlohmann::json nodes_invoke(const nlohmann::json& params);

  // ==================== nodes_status ====================
  /**
   * @brief 查看节点详细状态
   * 
   * 参数:
   * - node_id (string, optional): 节点 ID (不提供则返回所有节点状态)
   * - last_connected (string, optional): 过滤最后连接时间 (如 "24h")
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "nodes": [
   *     {
   *       "id": "node-123",
   *       "label": "My iPhone",
   *       "platform": "iOS 17.0",
   *       "connected": true,
   *       "last_seen_ms": 1234567890,
   *       "capabilities": [...],
   *       "stats": {
   *         "total_commands": 42,
   *         "uptime_seconds": 86400
   *       }
   *     }
   *   ]
   * }
   * 
   * OpenClaw 对齐:
   * - src/cli/nodes-cli/register.status.ts (nodes status)
   * - src/shared/node-list-parse.ts (parseNodeList)
   */
  nlohmann::json nodes_status(const nlohmann::json& params);

  // ==================== 辅助方法 ====================
  
  // 节点信息结构
  struct NodeInfo {
    std::string id;
    std::string label;
    std::string platform;
    bool connected;
    int64_t last_seen_ms;
    std::vector<std::string> capabilities;
    std::optional<nlohmann::json> stats;
  };
  
  // 从注册表加载节点列表
  std::vector<NodeInfo> load_nodes();
  
  // 检查节点是否在线
  bool is_node_connected(const std::string& node_id);
  
  // 获取节点能力
  std::vector<std::string> get_node_capabilities(const std::string& node_id);
  
  // 验证命令是否允许 (对齐 isNodeCommandAllowed)
  bool is_command_allowed(const std::string& command);
  
  // 发送节点命令 (通过 WebSocket 或 HTTP)
  nlohmann::json send_node_command(const std::string& node_id,
                                    const std::string& command,
                                    const nlohmann::json& params,
                                    int timeout_seconds);
  
  // 解析时间范围 (如 "24h" -> 86400000 ms)
  int64_t parse_time_range(const std::string& range);
  
  // 节点唤醒 (对齐 OpenClaw NODE_WAKE_RECONNECT_WAIT_MS)
  struct NodeWakeAttempt {
    bool available;
    bool throttled;
    std::string path;
    int64_t duration_ms;
  };
  NodeWakeAttempt wake_node(const std::string& node_id);
  
  // 挂起的前台动作 (对齐 OpenClaw PendingNodeAction)
  struct PendingNodeAction {
    std::string id;
    std::string node_id;
    std::string command;
    std::optional<std::string> params_json;
    std::string idempotency_key;
    int64_t enqueued_at_ms;
  };
  std::vector<PendingNodeAction> get_pending_actions(const std::string& node_id);
  
  // 私有成员
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tools
} // namespace hunterclaw
