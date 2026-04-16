#pragma once

// 🎯 对齐 OpenClaw: gateway/node-registry.ts (NodeSession, NodeInvokeResult)
// 🎯 对齐 OpenClaw: shared/node-list-types.ts (NodeListNode, PendingRequest, PairedNode)
// 🎯 对齐 OpenClaw: infra/node-commands.ts (NODE_SYSTEM_RUN_COMMANDS etc.)

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>
#include <set>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// Node Commands Constants (对齐 OpenClaw infra/node-commands.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: NODE_SYSTEM_RUN_COMMANDS
inline const std::vector<std::string> NODE_SYSTEM_RUN_COMMANDS = {
    "system.run.prepare",
    "system.run",
    "system.which",
};

// 🎯 对齐 OpenClaw: NODE_SYSTEM_NOTIFY_COMMAND
inline const std::string NODE_SYSTEM_NOTIFY_COMMAND = "system.notify";

// 🎯 对齐 OpenClaw: NODE_BROWSER_PROXY_COMMAND
inline const std::string NODE_BROWSER_PROXY_COMMAND = "browser.proxy";

// 🎯 对齐 OpenClaw: NODE_EXEC_APPROVALS_COMMANDS
inline const std::vector<std::string> NODE_EXEC_APPROVALS_COMMANDS = {
    "system.execApprovals.get",
    "system.execApprovals.set",
};

// ============================================================================
// NodeSession (对齐 OpenClaw gateway/node-registry.ts:4-23)
// ============================================================================

struct NodeSession {
    std::string node_id;                   // 🎯 nodeId
    std::string conn_id;                   // 🎯 connId
    std::string client_id;                 // 🎯 clientId
    std::string client_mode;               // 🎯 clientMode
    std::string display_name;              // 🎯 displayName
    std::string platform;                  // 🎯 platform
    std::string version;                   // 🎯 version
    std::string core_version;              // 🎯 coreVersion
    std::string ui_version;                // 🎯 uiVersion
    std::string device_family;             // 🎯 deviceFamily
    std::string model_identifier;          // 🎯 modelIdentifier
    std::string remote_ip;                 // 🎯 remoteIp
    std::vector<std::string> caps;         // 🎯 caps: string[]
    std::vector<std::string> commands;     // 🎯 commands: string[]
    std::map<std::string, bool> permissions; // 🎯 permissions?: Record<string, boolean>
    std::string path_env;                  // 🎯 pathEnv?: string
    int64_t connected_at_ms = 0;           // 🎯 connectedAtMs: number
};

// ============================================================================
// NodeInvokeResult (对齐 OpenClaw gateway/node-registry.ts:33-38)
// ============================================================================

struct NodeInvokeResult {
    bool ok = false;                                          // 🎯 ok: boolean
    std::optional<std::string> payload_json;                  // 🎯 payloadJSON?: string | null
    struct Error {                                            // 🎯 error?: { code?: string; message?: string }
        std::string code;
        std::string message;
    };
    std::optional<Error> error;
};

// ============================================================================
// NodeListNode (对齐 OpenClaw shared/node-list-types.ts:1-21)
// ============================================================================

struct NodeListNode {
    std::string node_id;                                      // 🎯 nodeId
    std::optional<std::string> display_name;                  // 🎯 displayName?
    std::optional<std::string> platform;                      // 🎯 platform?
    std::optional<std::string> version;                       // 🎯 version?
    std::optional<std::string> core_version;                  // 🎯 coreVersion?
    std::optional<std::string> ui_version;                    // 🎯 uiVersion?
    std::optional<std::string> client_id;                     // 🎯 clientId?
    std::optional<std::string> client_mode;                   // 🎯 clientMode?
    std::optional<std::string> remote_ip;                     // 🎯 remoteIp?
    std::optional<std::string> device_family;                 // 🎯 deviceFamily?
    std::optional<std::string> model_identifier;              // 🎯 modelIdentifier?
    std::optional<std::string> path_env;                      // 🎯 pathEnv?
    std::vector<std::string> caps;                            // 🎯 caps?: string[]
    std::vector<std::string> commands;                        // 🎯 commands?: string[]
    std::map<std::string, bool> permissions;                  // 🎯 permissions?: Record<string, boolean>
    bool paired = false;                                      // 🎯 paired?: boolean
    bool connected = false;                                   // 🎯 connected?: boolean
    std::optional<int64_t> connected_at_ms;                   // 🎯 connectedAtMs?: number
    std::optional<int64_t> approved_at_ms;                    // 🎯 approvedAtMs?: number
};

// ============================================================================
// PendingRequest (对齐 OpenClaw shared/node-list-types.ts:23-35)
// ============================================================================

struct PendingRequest {
    std::string request_id;                                   // 🎯 requestId
    std::string node_id;                                      // 🎯 nodeId
    std::optional<std::string> display_name;
    std::optional<std::string> platform;
    std::optional<std::string> version;
    std::optional<std::string> core_version;
    std::optional<std::string> ui_version;
    std::optional<std::string> remote_ip;
    int64_t ts = 0;                                           // 🎯 ts: number
    std::vector<std::string> commands;
    std::vector<std::string> required_approve_scopes;         // 🎯 requiredApproveScopes
};

// ============================================================================
// PairedNode (对齐 OpenClaw shared/node-list-types.ts:37-50)
// ============================================================================

struct PairedNode {
    std::string node_id;
    std::optional<std::string> token;
    std::optional<std::string> display_name;
    std::optional<std::string> platform;
    std::optional<std::string> version;
    std::optional<std::string> core_version;
    std::optional<std::string> ui_version;
    std::optional<std::string> remote_ip;
    std::map<std::string, bool> permissions;
    std::optional<int64_t> created_at_ms;
    std::optional<int64_t> approved_at_ms;
    std::optional<int64_t> last_connected_at_ms;
};

// ============================================================================
// PairingList (对齐 OpenClaw shared/node-list-types.ts:52-55)
// ============================================================================

struct PairingList {
    std::vector<PendingRequest> pending;
    std::vector<PairedNode> paired;
};

// ============================================================================
// NodeApprovalScope (对齐 OpenClaw infra/node-pairing-authz.ts:3)
// ============================================================================

enum class NodeApprovalScope {
    OperatorPairing,   // "operator.pairing"
    OperatorWrite,     // "operator.write"
    OperatorAdmin,     // "operator.admin"
};

// 🎯 对齐 OpenClaw: resolveNodePairApprovalScopes
std::vector<NodeApprovalScope> resolve_node_pair_approval_scopes(
    const std::vector<std::string>& commands);

std::string node_approval_scope_to_string(NodeApprovalScope scope);

} // namespace nodes
} // namespace hunterclaw
