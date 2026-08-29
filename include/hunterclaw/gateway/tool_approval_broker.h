// ============================================================================
// tool_approval_broker.h — 工具审批的 gateway 侧传输实现
//
// 🎯 严格对齐 OpenClaw: 工具审批走 PluginApprovalRequestPayload,
//    与 exec 审批共用同一个审批 store (一个 store + 两类 payload)。
//
// ★ 依赖方向: gateway → tools (实现 tools 侧声明的接口)。
//   ⚠ 反过来会成环: tools → gateway → agents → tools。
//   这也是接口声明在 tools/before_tool_call_policy.h、实现落在这里的原因。
// ============================================================================

#pragma once

#include "hunterclaw/gateway/exec_approval_manager.h"
#include "hunterclaw/tools/before_tool_call_policy.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace gateway {

// ── GatewayToolApprovalBroker ───────────────────────────────────────────────
// 把工具审批请求落到 ExecApprovalManager (plugin 形状载荷), 并广播
// plugin.approval.requested 事件让客户端可见。
//
// 决策回流路径 (无需本类参与):
//   客户端 → approval.resolve {id, kind:"plugin", decision}
//          → ExecApprovalManager::resolve()
//          → await_decision() 返回 → Stage 1 拿到决策
class GatewayToolApprovalBroker : public tools::ToolApprovalBroker {
public:
    /// 广播函数。
    /// ★ 用 std::function 注入而非直接依赖 GatewayServer:
    ///   本类只需要"把一个事件发出去"这一个能力, 引入整个 server
    ///   会让单测必须起一个 gateway。
    using BroadcastFn =
        std::function<void(const std::string& event, const nlohmann::json& payload)>;

    GatewayToolApprovalBroker(std::shared_ptr<ExecApprovalManager> manager,
                              BroadcastFn broadcast);

    std::string request_tool_approval(
        const std::string& tool_name,
        const std::string& tool_call_id,
        const std::string& agent_id,
        const std::string& session_key,
        int64_t timeout_ms,
        const std::vector<std::string>& allowed_decisions) override;

    std::optional<std::string> wait_tool_approval(
        const std::string& approval_id, int64_t timeout_ms,
        const std::atomic<bool>* aborted = nullptr) override;

private:
    std::shared_ptr<ExecApprovalManager> manager_;
    BroadcastFn broadcast_;
};

}  // namespace gateway
}  // namespace hunterclaw
