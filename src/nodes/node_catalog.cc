// 🎯 对齐 OpenClaw: gateway/node-catalog.ts (完整实现)
// 合并三个数据源构建已知节点目录

#include "hunterclaw/nodes/node_catalog.h"
#include <algorithm>
#include <set>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// Helper: uniqueSortedStrings (对齐 OpenClaw line 45-59)
// ============================================================================

static std::vector<std::string> unique_sorted_strings(
    const std::vector<std::string>& a,
    const std::vector<std::string>& b = {}) {
    std::set<std::string> values;
    for (const auto& v : a) {
        std::string trimmed = v;
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        if (!trimmed.empty()) values.insert(trimmed);
    }
    for (const auto& v : b) {
        std::string trimmed = v;
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        if (!trimmed.empty()) values.insert(trimmed);
    }
    return {values.begin(), values.end()};
}

// ============================================================================
// buildEffectiveKnownNode (对齐 OpenClaw line 91-121)
// 优先级: Live > NodePairing > DevicePairing
// ============================================================================

static NodeListNode build_effective_known_node(
    const std::string& node_id,
    const std::optional<KnownNodeDevicePairingSource>& dp,
    const std::optional<KnownNodeApprovedSource>& np,
    const std::optional<NodeSession>& live) {

    NodeListNode n;
    n.node_id = node_id;

    // 🎯 对齐 OpenClaw: live?.displayName ?? nodePairing?.displayName ?? devicePairing?.displayName
    n.display_name = live ? (live->display_name.empty() ? std::nullopt : std::optional(live->display_name))
                    : np ? np->display_name
                    : dp ? dp->display_name : std::nullopt;
    n.platform = live ? (live->platform.empty() ? std::nullopt : std::optional(live->platform))
                : np ? np->platform
                : dp ? dp->platform : std::nullopt;
    n.version = live ? (live->version.empty() ? std::nullopt : std::optional(live->version))
              : np ? np->version : std::nullopt;
    n.core_version = live ? (live->core_version.empty() ? std::nullopt : std::optional(live->core_version))
                   : np ? np->core_version : std::nullopt;
    n.ui_version = live ? (live->ui_version.empty() ? std::nullopt : std::optional(live->ui_version))
                 : np ? np->ui_version : std::nullopt;
    n.client_id = live ? (live->client_id.empty() ? std::nullopt : std::optional(live->client_id))
                : dp ? dp->client_id : std::nullopt;
    n.client_mode = live ? (live->client_mode.empty() ? std::nullopt : std::optional(live->client_mode))
                  : dp ? dp->client_mode : std::nullopt;
    n.device_family = live ? (live->device_family.empty() ? std::nullopt : std::optional(live->device_family))
                    : np ? np->device_family : std::nullopt;
    n.model_identifier = live ? (live->model_identifier.empty() ? std::nullopt : std::optional(live->model_identifier))
                        : np ? np->model_identifier : std::nullopt;
    n.remote_ip = live ? (live->remote_ip.empty() ? std::nullopt : std::optional(live->remote_ip))
                : np ? np->remote_ip
                : dp ? dp->remote_ip : std::nullopt;

    // 🎯 对齐 OpenClaw: caps/commands use live if available, fallback to nodePairing
    n.caps = live ? unique_sorted_strings(live->caps)
           : np ? unique_sorted_strings(np->caps) : std::vector<std::string>{};
    n.commands = live ? unique_sorted_strings(live->commands)
               : np ? unique_sorted_strings(np->commands) : std::vector<std::string>{};

    n.path_env = live ? (live->path_env.empty() ? std::nullopt : std::optional(live->path_env)) : std::nullopt;
    n.permissions = live ? live->permissions : (np ? np->permissions : std::map<std::string, bool>{});

    n.connected_at_ms = live ? std::optional(live->connected_at_ms) : std::nullopt;
    n.approved_at_ms = np ? np->approved_at_ms : (dp ? dp->approved_at_ms : std::nullopt);

    n.paired = dp.has_value() || np.has_value();
    n.connected = live.has_value();

    return n;
}

// ============================================================================
// createKnownNodeCatalog (对齐 OpenClaw line 138-176)
// ============================================================================

KnownNodeCatalog create_known_node_catalog(
    const std::vector<KnownNodeDevicePairingSource>& device_pairings,
    const std::vector<KnownNodeApprovedSource>& node_pairings,
    const std::vector<NodeSession>& connected_nodes) {

    std::map<std::string, KnownNodeDevicePairingSource> dp_by_id;
    for (const auto& dp : device_pairings) {
        dp_by_id[dp.node_id] = dp;
    }
    std::map<std::string, KnownNodeApprovedSource> np_by_id;
    for (const auto& np : node_pairings) {
        np_by_id[np.node_id] = np;
    }
    std::map<std::string, NodeSession> live_by_id;
    for (const auto& s : connected_nodes) {
        live_by_id[s.node_id] = s;
    }

    // 🎯 对齐 OpenClaw: const nodeIds = new Set([...dpKeys, ...npKeys, ...liveKeys])
    std::set<std::string> all_ids;
    for (const auto& [id, _] : dp_by_id) all_ids.insert(id);
    for (const auto& [id, _] : np_by_id) all_ids.insert(id);
    for (const auto& [id, _] : live_by_id) all_ids.insert(id);

    KnownNodeCatalog catalog;
    for (const auto& node_id : all_ids) {
        auto dp_it = dp_by_id.find(node_id);
        auto np_it = np_by_id.find(node_id);
        auto live_it = live_by_id.find(node_id);

        std::optional<KnownNodeDevicePairingSource> dp = dp_it != dp_by_id.end() ? std::optional(dp_it->second) : std::nullopt;
        std::optional<KnownNodeApprovedSource> np = np_it != np_by_id.end() ? std::optional(np_it->second) : std::nullopt;
        std::optional<NodeSession> live = live_it != live_by_id.end() ? std::optional(live_it->second) : std::nullopt;

        KnownNodeEntry entry;
        entry.node_id = node_id;
        entry.device_pairing = dp;
        entry.node_pairing = np;
        entry.live = live;
        entry.effective = build_effective_known_node(node_id, dp, np, live);

        catalog.entries_by_id[node_id] = std::move(entry);
    }
    return catalog;
}

// ============================================================================
// compareKnownNodes (对齐 OpenClaw line 123-136)
// ============================================================================

static bool compare_known_nodes(const NodeListNode& a, const NodeListNode& b) {
    if (a.connected != b.connected) {
        return a.connected > b.connected;  // connected first
    }
    std::string a_name = a.display_name.value_or(a.node_id);
    std::string b_name = b.display_name.value_or(b.node_id);
    // 简化 normalize: to_lower
    std::transform(a_name.begin(), a_name.end(), a_name.begin(), ::tolower);
    std::transform(b_name.begin(), b_name.end(), b_name.begin(), ::tolower);
    if (a_name != b_name) return a_name < b_name;
    return a.node_id < b.node_id;
}

// ============================================================================
// listKnownNodes (对齐 OpenClaw line 178-182)
// ============================================================================

std::vector<NodeListNode> list_known_nodes(const KnownNodeCatalog& catalog) {
    std::vector<NodeListNode> result;
    result.reserve(catalog.entries_by_id.size());
    for (const auto& [id, entry] : catalog.entries_by_id) {
        result.push_back(entry.effective);
    }
    std::sort(result.begin(), result.end(), compare_known_nodes);
    return result;
}

// ============================================================================
// getKnownNodeEntry (对齐 OpenClaw line 184-189)
// ============================================================================

std::optional<KnownNodeEntry> get_known_node_entry(
    const KnownNodeCatalog& catalog, const std::string& node_id) {
    auto it = catalog.entries_by_id.find(node_id);
    if (it == catalog.entries_by_id.end()) return std::nullopt;
    return it->second;
}

// ============================================================================
// getKnownNode (对齐 OpenClaw line 191-193)
// ============================================================================

std::optional<NodeListNode> get_known_node(
    const KnownNodeCatalog& catalog, const std::string& node_id) {
    auto entry = get_known_node_entry(catalog, node_id);
    if (!entry) return std::nullopt;
    return entry->effective;
}

} // namespace nodes
} // namespace hunterclaw
