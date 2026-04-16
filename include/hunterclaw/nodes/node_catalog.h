#pragma once

// 🎯 对齐 OpenClaw: gateway/node-catalog.ts
// 合并三个数据源（DevicePairing + NodePairing + LiveSession）构建已知节点目录

#include "hunterclaw/nodes/node.h"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// KnownNodeDevicePairingSource (对齐 OpenClaw line 7-15)
// ============================================================================

struct KnownNodeDevicePairingSource {
    std::string node_id;
    std::optional<std::string> display_name;
    std::optional<std::string> platform;
    std::optional<std::string> client_id;
    std::optional<std::string> client_mode;
    std::optional<std::string> remote_ip;
    std::optional<int64_t> approved_at_ms;
};

// ============================================================================
// KnownNodeApprovedSource (对齐 OpenClaw line 17-31)
// ============================================================================

struct KnownNodeApprovedSource {
    std::string node_id;
    std::optional<std::string> display_name;
    std::optional<std::string> platform;
    std::optional<std::string> version;
    std::optional<std::string> core_version;
    std::optional<std::string> ui_version;
    std::optional<std::string> remote_ip;
    std::optional<std::string> device_family;
    std::optional<std::string> model_identifier;
    std::vector<std::string> caps;
    std::vector<std::string> commands;
    std::map<std::string, bool> permissions;
    std::optional<int64_t> approved_at_ms;
};

// ============================================================================
// KnownNodeEntry (对齐 OpenClaw line 33-39)
// ============================================================================

struct KnownNodeEntry {
    std::string node_id;
    std::optional<KnownNodeDevicePairingSource> device_pairing;
    std::optional<KnownNodeApprovedSource> node_pairing;
    std::optional<NodeSession> live;
    NodeListNode effective;
};

// ============================================================================
// KnownNodeCatalog (对齐 OpenClaw line 41-43)
// ============================================================================

struct KnownNodeCatalog {
    std::map<std::string, KnownNodeEntry> entries_by_id;
};

// ============================================================================
// Catalog Functions (对齐 OpenClaw line 138-193)
// ============================================================================

// 🎯 对齐 OpenClaw: createKnownNodeCatalog(params)
KnownNodeCatalog create_known_node_catalog(
    const std::vector<KnownNodeDevicePairingSource>& device_pairings,
    const std::vector<KnownNodeApprovedSource>& node_pairings,
    const std::vector<NodeSession>& connected_nodes);

// 🎯 对齐 OpenClaw: listKnownNodes(catalog)
std::vector<NodeListNode> list_known_nodes(const KnownNodeCatalog& catalog);

// 🎯 对齐 OpenClaw: getKnownNodeEntry(catalog, nodeId)
std::optional<KnownNodeEntry> get_known_node_entry(
    const KnownNodeCatalog& catalog, const std::string& node_id);

// 🎯 对齐 OpenClaw: getKnownNode(catalog, nodeId)
std::optional<NodeListNode> get_known_node(
    const KnownNodeCatalog& catalog, const std::string& node_id);

} // namespace nodes
} // namespace hunterclaw
