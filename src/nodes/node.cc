// 🎯 对齐 OpenClaw: shared/node-list-types.ts + infra/node-pairing-authz.ts
// Node 类型实现及 approval scope 解析

#include "hunterclaw/nodes/node.h"
#include <algorithm>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// resolveNodePairApprovalScopes
// 🎯 对齐 OpenClaw: infra/node-pairing-authz.ts:9-22
// ============================================================================

std::vector<NodeApprovalScope> resolve_node_pair_approval_scopes(
    const std::vector<std::string>& commands) {
    // 🎯 对齐 OpenClaw: 如果 commands 包含 system.run 命令，需要 admin scope
    bool has_system_run = false;
    for (const auto& cmd : commands) {
        for (const auto& sys_cmd : NODE_SYSTEM_RUN_COMMANDS) {
            if (cmd == sys_cmd) {
                has_system_run = true;
                break;
            }
        }
        if (has_system_run) break;
    }

    if (has_system_run) {
        return {NodeApprovalScope::OperatorPairing, NodeApprovalScope::OperatorAdmin};
    }
    if (!commands.empty()) {
        return {NodeApprovalScope::OperatorPairing, NodeApprovalScope::OperatorWrite};
    }
    return {NodeApprovalScope::OperatorPairing};
}

std::string node_approval_scope_to_string(NodeApprovalScope scope) {
    switch (scope) {
        case NodeApprovalScope::OperatorPairing: return "operator.pairing";
        case NodeApprovalScope::OperatorWrite:   return "operator.write";
        case NodeApprovalScope::OperatorAdmin:   return "operator.admin";
    }
    return "operator.pairing";
}

} // namespace nodes
} // namespace hunterclaw
