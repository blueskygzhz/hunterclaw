#pragma once

// 🎯 对齐 OpenClaw: gateway/node-pending-work.ts
// Pending work 队列：status.request / location.request 等

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// Constants (对齐 OpenClaw line 3-7, 36-44)
// ============================================================================

enum class NodePendingWorkType {
    StatusRequest,    // "status.request"
    LocationRequest,  // "location.request"
};

enum class NodePendingWorkPriority {
    Default,  // "default" — rank 1
    Normal,   // "normal"  — rank 2
    High,     // "high"    — rank 3
};

// ============================================================================
// NodePendingWorkItem (对齐 OpenClaw line 9-16)
// ============================================================================

struct NodePendingWorkItem {
    std::string id;
    NodePendingWorkType type;
    NodePendingWorkPriority priority;
    int64_t created_at_ms = 0;
    std::optional<int64_t> expires_at_ms;
    std::map<std::string, std::string> payload;
};

// ============================================================================
// DrainResult (对齐 OpenClaw line 29-33)
// ============================================================================

struct DrainResult {
    int revision = 0;
    std::vector<NodePendingWorkItem> items;
    bool has_more = false;
};

// ============================================================================
// Functions (对齐 OpenClaw line 103-201)
// ============================================================================

struct EnqueueResult {
    int revision;
    NodePendingWorkItem item;
    bool deduped;
};

// 🎯 对齐 OpenClaw: enqueueNodePendingWork(params)
EnqueueResult enqueue_node_pending_work(
    const std::string& node_id,
    NodePendingWorkType type,
    NodePendingWorkPriority priority = NodePendingWorkPriority::Normal,
    std::optional<int> expires_in_ms = std::nullopt,
    const std::map<std::string, std::string>& payload = {});

// 🎯 对齐 OpenClaw: drainNodePendingWork(nodeId, opts)
DrainResult drain_node_pending_work(
    const std::string& node_id,
    int max_items = 4,
    bool include_default_status = true);

struct AckResult {
    int revision;
    std::vector<std::string> removed_item_ids;
};

// 🎯 对齐 OpenClaw: acknowledgeNodePendingWork({nodeId, itemIds})
AckResult acknowledge_node_pending_work(
    const std::string& node_id,
    const std::vector<std::string>& item_ids);

// 🎯 对齐 OpenClaw: resetNodePendingWorkForTests()
void reset_node_pending_work_for_tests();

} // namespace nodes
} // namespace hunterclaw
